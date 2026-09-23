# slim-agent

[English](README.md) | [繁體中文](README.zh-TW.md) | **简体中文** | [日本語](README.ja.md) | [한국어](README.ko.md) | [Español](README.es.md)

一个可以直接跑在参考 CPE 上的小型 agent（OpenWrt 23.05.5、aarch64 musl，
**系统镜像里没有 python3**）：通过 OpenAI 兼容端点对话、对 markdown 做关键词 RAG、白名单工具层、
markdown 技能 —— 并且在任何写入动作之前都先经过 fail-closed 策略。

两个 client、同一套行为：`slim/cpp/` 跑在 CPE 上（交叉编译为 aarch64 musl，另有完全不需要在装置上
安装任何东西的静态版本），`slim/` 则是功能相同、给装了 Python 的主机用的 Python client。本项目是
独立项目 —— 不是 Hermes，也不是 Hermes IoT/Pi2 fork。它原本在 Hermes fork
`Matt0080828/new_agent` 里开发，后来（`cc8a67c`）抽离成自己的仓库，与该 fork 没有共同历史。

```text
slim/            Python client：策略、session 存储、工具、RAG、测试
slim/cpp/        C++ client（功能相同），外加四个测试可执行文件
slim/deploy/     run-on-t830.sh 与 on-device-smoke.sh —— 部署路径
slim/README.md   完整手册：构建、部署、安装、配置、在装置上运行模型（英文）
```

## 在主机上构建与测试

```bash
cd slim/cpp && make && make test                              # 81 + 54 + 39 + 63 = 237 checks
cd slim && python3 -m unittest discover -s . -p 'test_*.py'    # 103 tests，只用标准库
```

## 为 CPE 构建

```bash
make -C slim/cpp t830           # 动态链接，138,240 bytes；装置上需要 libstdc++/libgcc
make -C slim/cpp t830-static    # 715,048 bytes、NEEDED 0、已 strip；装置上无需安装任何东西
```

## 放到 CPE 上

```bash
./slim/deploy/run-on-t830.sh    # 容器 + adb、推送、双方比对 sha256、装置端 smoke test
```

装置只能通过 **USB 上的 adb** 访问 —— 管理 IP 会响应 ARP 但所有 TCP 端口都是关闭的、`/dev/ttyACM*`
是 modem 的 AT 端口、RNDIS 也不发 DHCP。因为 USB 节点只有 root 能打开，adb 运行在特权容器里；而容器
看不到主机路径，所以这些脚本会先用 `docker cp` 把要推送的文件送进容器。

## 安装位置，以及它如何配置

`/tmp` 是 tmpfs，推上去的文件重启后就没了。要安装就装在 `/data`（12.5 GB 可用）或 `/overlay`
（116 MB）；两者都没有挂 `noexec`。脚本与手册假定的布局是
`/data/slim/{slim-agent,run,data,skills,docs}` —— 700 KB 起，再加上你自己的语料。

C++ client **没有配置文件**：只有标志与环境变量，而且**标志优先于环境变量**。它的默认值是相对于
工作目录的（`--data-dir` 默认 `./slim/data`），在只读根目录下运行会出现「有回答但不会留下记录」的
情况 —— 所以永远显式带上 `--data-dir`。docs 与 skills 目录同样是「相对工作目录」的规则（`./slim/docs`、`./slim/skills`）：用上面的布局时要从 `/data` 执行（或设 `SLIM_DOCS_DIR` / `SLIM_SKILLS_DIR`），否则 `/rag` 什么都搜不到。完整的标志／环境变量／默认值对照表、安装命令，以及把配置
集中其中的 wrapper 脚本，都在 `slim/README.md`。

## 使用运行在 CPE 自身的模型

agent 只认一个 OpenAI 兼容端点，所以把 llama.cpp server 跑在装置上，只是改 `--base-url`
而已 —— 不需要 LAN，其他配置都不动：

```bash
# 装置端：在 loopback 上启动 server（没有 service entry；按需启动，停止用 kill）
cd /data/slim && nohup ./llama-server -m models/qwen2.5-0.5b-instruct-q4_k_m.gguf \
  --host 127.0.0.1 --port 8080 -c 2048 -t 4 > llama-server.log 2>&1 &

# agent 指向它
./slim-agent --data-dir /data/slim/data --session local --non-interactive \
  --model qwen2.5-0.5b-instruct --base-url http://127.0.0.1:8080/v1 --stream --once "Reply with one word: pong"
```

能装多大的模型：CPE 有 1.7 GB RAM，`Qwen2.5-0.5B-Instruct-Q4_K_M`（469 MB，推送耗时 47 秒）
加载后占 646 MB RSS，还剩约 1.1 GB 可用 —— 超过 1B 左右就装不下了。交叉编译的 server 只需要
`libstdc++.so.6`、`libgcc_s.so.1` 与 musl `libc`，这些镜像里都有。用 `-t 4` 时一个短回合约
4 秒，server 自报 prompt eval 12 tok/s、生成 7.9 tok/s。这个尺寸的模型不是你可以从 LAN 服务的
7B：答案会弱一些；它能输出 prompt 里展示的 tool JSON 格式，但不稳定，所以驱动动作要用斜线命令
（`/rag`、`/read`、`/write`、`/mqtt`、`/history`）才靠得住。两者可以按每次调用并存，`--fallback-url` 还能做出
「本地优先、LAN 备援」。`slim/README.md` 有推送命令、实测数字与注意事项。

## 在真实硬件上验证过的项目

| 检查项 | CPE 上的结果 |
| --- | --- |
| artifact 完整性 | `sha256 1c2c17b3...` 在主机与装置两边一致（`NEEDED 0`、已 strip） |
| `./slim/deploy/run-on-t830.sh` | exit 0：斜线命令写入后读回、session 存储、`--dry-run-writes` 确实没写文件、每一项 fail-closed 拒绝都正确 |
| 走 LAN 的 live model 回合 | 流式回答；随后 `--history 6` 重放并答出上一回合的数字；`--history 0` 则答不出来 —— 负向对照 |
| **模型跑在装置自身** | loopback 上的 `llama-server` + 0.5B Q4：4 秒回 `PONG!`、prompt 12 tok/s、生成 7.9 tok/s、`/history` 能重放那些回合，且 `--fallback-url` 在主端点挂掉时仍能作答 |
| session 存储 | `sessions/<name>.jsonl`、权限 `0600`、一行一个 JSON 对象 |
| RAG 语料，不需重 build | 放进 `docs/` 与 `skills/` 的 markdown 会被即时搜到：`/rag mt753x`、多词的 `/rag probe -22`、`/rag HotSpotFlag` 与 `/rag OWE` 都把对的文件排在第一（2026-09-22） |
| 模型失败（HTTP 400） | 报告 `HTTP status 400: <服务器消息>`，且不写入 session 文件 |

重新构建这棵树会产生与上面**完全相同**的 artifact，所以部署前请比对 sha256。完整的验证表格、
装置实测数据与部署脚本都在 `slim/README.md` 与 `slim/deploy/`。
