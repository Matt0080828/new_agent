# t830-slim-agent

[English](README.md) | **繁體中文** | [简体中文](README.zh-CN.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Español](README.es.md)

一個可以直接跑在 Askey T830 / Fibocom FG370 CPE 上的小型 agent（OpenWrt 23.05.5、aarch64 musl，
**系統 image 裡沒有 python3**）：透過 OpenAI 相容端點對話、對 markdown 做關鍵字式 RAG、白名單工具層、
markdown 技能 —— 而且任何寫入動作前都先過 fail-closed 政策。

兩份 client、同一套行為：`slim/cpp/` 是跑在 CPE 上的那份（交叉編譯為 aarch64 musl，另有完全不需
安裝任何東西的靜態版本），`slim/` 則是功能相同、給有 Python 的主機用的 Python client。本專案是
獨立專案 —— 不是 Hermes，也不是 Hermes IoT／Pi2 fork。它原本在 Hermes fork
`Matt0080828/new_agent` 內開發，後來（`cc8a67c`）抽離成自己的 repo，與該 fork 沒有共用歷史。

```text
slim/            Python client：政策、session 儲存、工具、RAG、測試
slim/cpp/        C++ client（功能相同），另有四個測試執行檔
slim/deploy/     run-on-t830.sh 與 on-device-smoke.sh —— 部署路徑
slim/README.md   完整手冊：建置、部署、安裝、設定、在裝置上跑模型（英文）
```

## 在主機上建置與測試

```bash
cd slim/cpp && make && make test                              # 74 + 54 + 39 + 43 = 210 checks
cd slim && python3 -m unittest discover -s . -p 'test_*.py'    # 89 tests，只用標準庫
```

## 為 CPE 建置

```bash
make -C slim/cpp t830           # 動態連結，121,736 bytes；裝置上需要 libstdc++/libgcc
make -C slim/cpp t830-static    # 698,664 bytes、NEEDED 0、已 strip；裝置上不需安裝任何東西
```

## 放到 CPE 上

```bash
./slim/deploy/run-on-t830.sh    # 容器 + adb、推送、兩邊比對 sha256、裝置端 smoke test
```

裝置只能透過 **USB 上的 adb** 存取 —— 管理 IP 會回 ARP 但所有 TCP port 都是關的、`/dev/ttyACM*`
是 modem 的 AT port、RNDIS 也不發 DHCP。因為 USB 節點只有 root 能開，adb 跑在特權容器裡；而容器
看不到主機路徑，所以這些腳本會先用 `docker cp` 把要推的檔案送進容器。

## 安裝位置，以及它怎麼設定

`/tmp` 是 tmpfs，推上去的檔案重開機就沒了。要裝就裝在 `/data`（12.5 GB 可用）或 `/overlay`
（116 MB）；兩者都沒有掛 `noexec`。腳本與手冊假設的佈局是
`/data/slim/{slim-agent,run,data,skills,docs}` —— 700 KB 起，再加上你自己的語料。

C++ client **沒有設定檔**：只有旗標與環境變數，而且**旗標贏過環境變數**。它的預設值是相對工作目錄
的（`--data-dir` 預設 `./slim/data`），在唯讀的根目錄下執行會出現「有回答但不會留下紀錄」的情況 ——
所以永遠明確帶上 `--data-dir`。完整的旗標／環境變數／預設值對照表、安裝指令，以及把設定集中在裡面
的 wrapper 腳本，都在 `slim/README.md`。

## 使用跑在 T830 自己身上的模型

agent 只認一個 OpenAI 相容端點，所以要把 llama.cpp server 跑在裝置上，只是改 `--base-url`
而已 —— 不需要 LAN、其他設定都不動：

```bash
# 裝置端：在 loopback 上啟動 server（沒有 service entry；按需啟動，停止用 kill）
cd /data/slim && nohup ./llama-server -m models/qwen2.5-0.5b-instruct-q4_k_m.gguf \
  --host 127.0.0.1 --port 8080 -c 2048 -t 4 > llama-server.log 2>&1 &

# agent 指向它
./slim-agent --data-dir /data/slim/data --session local --non-interactive \
  --model qwen2.5-0.5b-instruct --base-url http://127.0.0.1:8080/v1 --stream --once "Reply with one word: pong"
```

放得下多大的模型：CPE 有 1.7 GB RAM，`Qwen2.5-0.5B-Instruct-Q4_K_M`（469 MB，推送花了 47 秒）
載入後佔 646 MB RSS，還剩約 1.1 GB 可用 —— 超過 1B 左右就放不下了。交叉編譯的 server 只需要
`libstdc++.so.6`、`libgcc_s.so.1` 與 musl `libc`，這些 image 裡都有。用 `-t 4` 時一個短回合約
4 秒，server 自報 prompt eval 12 tok/s、生成 7.9 tok/s。這個大小的模型不是你可以從 LAN 服務的
7B：答案會弱一些，而且請用斜線指令驅動它（`/rag`、`/read`、`/write`、`/mqtt`、`/history`）——
它不會穩定吐出可用的 tool JSON。兩者可以依每次呼叫並存，`--fallback-url` 還能做出
「本地優先、LAN 備援」。`slim/README.md` 有推送指令、實測數字與注意事項。

## 在真實硬體上驗證過的項目

| 檢查項目 | CPE 上的結果 |
| --- | --- |
| artifact 完整性 | `sha256 5de6ea50...` 在主機與裝置兩邊一致（`NEEDED 0`、已 strip） |
| `./slim/deploy/run-on-t830.sh` | exit 0：斜線指令寫入後讀回、session 儲存、`--dry-run-writes` 確實沒寫檔、每一項 fail-closed 拒絕都正確 |
| 走 LAN 的 live model 回合 | 串流回答；接著 `--history 6` 重播並答出前一回合的數字；`--history 0` 則答不出來 —— 負向對照 |
| **模型跑在裝置自己身上** | loopback 上的 `llama-server` + 0.5B Q4：4 秒回 `PONG!`、prompt 12 tok/s、生成 7.9 tok/s、`/history` 能重播那些回合，且 `--fallback-url` 在主端點壞掉時仍能作答 |
| session 儲存 | `sessions/<name>.jsonl`、權限 `0600`、一行一個 JSON 物件 |
| 模型失敗（HTTP 400） | 回報 `HTTP status 400: <伺服器訊息>`，且不寫入 session 檔 |

重新建置這棵樹會產生與上面**完全相同**的 artifact，所以部署前請比對 sha256。完整的驗證表格、
裝置實測數據與部署腳本都在 `slim/README.md` 與 `slim/deploy/`。
