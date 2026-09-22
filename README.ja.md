# t830-slim-agent

[English](README.md) | [繁體中文](README.zh-TW.md) | [简体中文](README.zh-CN.md) | **日本語** | [한국어](README.ko.md) | [Español](README.es.md)

Askey T830 / Fibocom FG370 CPE（OpenWrt 23.05.5、aarch64 musl、**イメージに python3 は入っていません**）
上でそのまま動く小さな agent です。OpenAI 互換エンドポイント経由のチャット、markdown に対する
キーワード RAG、ホワイトリスト方式のツール層、markdown スキル —— そして書き込みの手前には
fail-closed ポリシーがあります。

2 つのクライアント、1 つの挙動：`slim/cpp/` は CPE 上で動くほう（aarch64 musl 向けクロスビルド、
何もインストールせずに動く静的版もあります）、`slim/` は同じ機能を持つ Python クライアントで、
Python のあるホスト用です。本プロジェクトは独立したもので、Hermes でも Hermes IoT/Pi2 fork でも
ありません。Hermes fork `Matt0080828/new_agent` の中で開発され、そこから（`cc8a67c`）独立した
リポジトリとして切り出されたため、その fork と共通の履歴はありません。

```text
slim/            Python クライアント：ポリシー、session ストア、ツール、RAG、テスト
slim/cpp/        C++ クライアント（同じ機能）と 4 つのテストバイナリ
slim/deploy/     run-on-t830.sh と on-device-smoke.sh —— 配備の手順
slim/README.md   詳細マニュアル：ビルド、配備、インストール、設定、端末上のモデル（英語）
```

## ホストでのビルドとテスト

```bash
cd slim/cpp && make && make test                              # 81 + 54 + 39 + 53 = 227 checks
cd slim && python3 -m unittest discover -s . -p 'test_*.py'    # 97 tests、標準ライブラリのみ
```

## CPE 向けビルド

```bash
make -C slim/cpp t830           # 動的リンク、138,240 bytes。端末側に libstdc++/libgcc が必要
make -C slim/cpp t830-static    # 715,048 bytes、NEEDED 0、strip 済み。端末側に何も要りません
```

## CPE へ入れる

```bash
./slim/deploy/run-on-t830.sh    # コンテナ + adb、push、両側で sha256 照合、端末上の smoke test
```

端末へは **USB 経由の adb** でしか到達できません。管理 IP は ARP には応答しますが TCP ポートは
すべて閉じており、`/dev/ttyACM*` は modem の AT ポート、RNDIS は DHCP を出しません。USB ノードは
root 専用なので adb は特権コンテナ内で動き、コンテナはホストのパスを読めないため、スクリプトは
先に `docker cp` で送るものをコンテナへ入れます。

## インストール先と、設定の方法

`/tmp` は tmpfs なので、push したコピーは再起動で消えます。入れるなら `/data`（空き 12.5 GB）か
`/overlay`（116 MB）へ。どちらも `noexec` ではマウントされていません。スクリプトとマニュアルが
前提とする構成は `/data/slim/{slim-agent,run,data,skills,docs}` で、700 KB に加えて用意した
コーパス分です。

C++ クライアントに**設定ファイルはありません**。フラグと環境変数のみで、**フラグが環境変数に
優先します**。既定値は作業ディレクトリからの相対（`--data-dir` の既定は `./slim/data`）なので、
読み取り専用のルートで実行すると「回答はするが記録が残らない」状態になります。したがって
`--data-dir` は常に明示してください。docs と skills のディレクトリも同じ作業ディレクトリ相対の規則
（`./slim/docs`、`./slim/skills`）です：上記のレイアウトなら `/data` から実行するか、
`SLIM_DOCS_DIR` / `SLIM_SKILLS_DIR` を設定するか、さもないと `/rag` は何も見つけません。フラグ／環境変数／既定値の一覧、インストール手順、設定を
まとめて持つ wrapper スクリプトは `slim/README.md` にあります。

## T830 自身で動くモデルを使う

agent が知っているのは OpenAI 互換エンドポイントだけなので、llama.cpp の server を端末で動かす
場合の違いは `--base-url` だけです。LAN もその他の設定も不要です：

```bash
# 端末側：loopback で server を起動（service entry はありません。必要時に起動し、停止は kill）
cd /data/slim && nohup ./llama-server -m models/qwen2.5-0.5b-instruct-q4_k_m.gguf \
  --host 127.0.0.1 --port 8080 -c 2048 -t 4 > llama-server.log 2>&1 &

# agent をそこへ向ける
./slim-agent --data-dir /data/slim/data --session local --non-interactive \
  --model qwen2.5-0.5b-instruct --base-url http://127.0.0.1:8080/v1 --stream --once "Reply with one word: pong"
```

入るモデルの目安：CPE は RAM 1.7 GB で、`Qwen2.5-0.5B-Instruct-Q4_K_M`（469 MB、push に 47 秒）
はロード後 RSS 646 MB、なお約 1.1 GB 空いていました —— 1B を大きく超えるものは入りません。
クロスビルドした server が必要とするのは `libstdc++.so.6`、`libgcc_s.so.1`、musl `libc` だけで、
いずれもイメージに入っています。`-t 4` で短いターンは約 4 秒、server の自己申告は prompt eval
12 tok/s、生成 7.9 tok/s でした。この大きさのモデルは LAN で動かせる 7B とは別物です。回答は
弱くなりますし、動かすのはスラッシュコマンド（`/rag`、`/read`、`/write`、`/mqtt`、`/history`）
にしてください —— 実用的な tool JSON は安定して出しません。呼び出しごとに両者を使い分けられ、
`--fallback-url` を使えば「ローカル優先・LAN を保険に」できます。push 手順、実測値、注意点は
`slim/README.md` にあります。

## 実機で確認済みの項目

| 確認項目 | CPE 上での結果 |
| --- | --- |
| artifact の同一性 | `sha256 1c2c17b3...` がホストと端末で一致（`NEEDED 0`、strip 済み） |
| `./slim/deploy/run-on-t830.sh` | exit 0：スラッシュコマンドの書き込みと読み戻し、session ストア、`--dry-run-writes` が何も書かないこと、fail-closed の拒否がすべて期待どおり |
| LAN 経由の live model ターン | ストリーム回答、続いて `--history 6` が前のターンの数字を再現、`--history 0` では再現できない —— 負のコントロール |
| **モデルが端末自身で動く場合** | loopback の `llama-server` + 0.5B Q4：4 秒で `PONG!`、prompt 12 tok/s、生成 7.9 tok/s、`/history` がそれらのターンを再生、主エンドポイントが落ちても `--fallback-url` が回答 |
| session ストア | `sessions/<name>.jsonl`、モード `0600`、1 行 1 JSON オブジェクト |
| RAG コーパス、再ビルド不要 | `docs/` と `skills/` に置いた markdown はそのまま検索対象になる: `/rag mt753x`、複数語の `/rag probe -22`、`/rag HotSpotFlag`、`/rag OWE` いずれも正しいファイルを先頭で返した（2026-09-22） |
| モデル失敗（HTTP 400） | `HTTP status 400: <サーバーのメッセージ>` を報告し、session ファイルを書きません |

このツリーを再ビルドすると上記と**同一の** artifact が得られます。配備前に sha256 を照合して
ください。検証表の全文、実測した端末の事実、配備スクリプトは `slim/README.md` と
`slim/deploy/` にあります。
