# slim-agent

[English](README.md) | [繁體中文](README.zh-TW.md) | [简体中文](README.zh-CN.md) | [日本語](README.ja.md) | **한국어** | [Español](README.es.md)

기준 CPE(OpenWrt 23.05.5, aarch64 musl, **이미지에 python3 없음**)에서 바로
동작하는 작은 agent입니다. OpenAI 호환 엔드포인트로 대화하고, markdown에 대한 키워드 RAG, 화이트리스트
도구 계층, markdown 스킬을 제공하며 — 쓰기 동작 앞에는 fail-closed 정책이 있습니다.

클라이언트 둘, 동작 하나: `slim/cpp/`는 CPE에서 실행되는 쪽(aarch64 musl 크로스 빌드, 아무것도 설치하지
않아도 되는 정적 산출물 포함)이고, `slim/`은 같은 기능을 가진 Python 클라이언트로 Python이 있는 호스트용
입니다. 이 프로젝트는 독립적이며 Hermes도, Hermes IoT/Pi2 fork도 아닙니다. Hermes fork
`Matt0080828/new_agent` 안에서 개발되었고 거기서(`cc8a67c`) 자체 저장소로 분리되었기 때문에 그 fork와
공통 이력이 없습니다.

```text
slim/            Python 클라이언트: 정책, session 저장소, 도구, RAG, 테스트
slim/cpp/        같은 기능의 C++ 클라이언트와 테스트 바이너리 4개
slim/deploy/     run-on-t830.sh 와 on-device-smoke.sh — 배포 경로
slim/README.md   전체 매뉴얼: 빌드, 배포, 설치, 설정, 기기에서의 모델 (영문)
```

## 호스트에서 빌드와 테스트

```bash
cd slim/cpp && make && make test                              # 89 + 54 + 51 + 63 = 257 checks
cd slim && python3 -m unittest discover -s . -p 'test_*.py'    # 105 tests, 표준 라이브러리만 사용
```

## CPE용 빌드

```bash
make -C slim/cpp t830           # 동적 링크, 142,352 bytes; 기기에 libstdc++/libgcc 필요
make -C slim/cpp t830-static    # 719,144 bytes, NEEDED 0, strip 완료; 기기에 설치할 것이 없음
```

## CPE에 넣기

```bash
./slim/deploy/run-on-t830.sh    # 컨테이너 + adb, push, 양쪽 sha256 비교, 기기에서 smoke test
```

기기에는 **USB를 통한 adb**로만 접근할 수 있습니다. 관리 IP는 ARP에는 응답하지만 모든 TCP 포트가
닫혀 있고, `/dev/ttyACM*`은 modem의 AT 포트이며, RNDIS는 DHCP를 주지 않습니다. USB 노드는 root
전용이라 adb는 특권 컨테이너 안에서 실행되고, 컨테이너는 호스트 경로를 볼 수 없으므로 스크립트는
보낼 파일을 먼저 `docker cp`로 컨테이너에 넣습니다.

## 설치 위치와 설정 방법

`/tmp`는 tmpfs이므로 push한 사본은 재부팅하면 사라집니다. 설치하려면 `/data`(여유 12.5 GB) 또는
`/overlay`(116 MB)에 넣으세요. 둘 다 `noexec`로 마운트되어 있지 않습니다. 스크립트와 매뉴얼이
가정하는 구조는 `/data/slim/{slim-agent,run,data,skills,docs}`이며, 700 KB에 추가한 코퍼스가 더해집니다.

C++ 클라이언트에는 **설정 파일이 없습니다**. 플래그와 환경 변수뿐이고, **플래그가 환경 변수보다
우선합니다**. 기본값은 작업 디렉터리 기준 상대 경로이며(`--data-dir` 기본값 `./slim/data`), 읽기 전용
루트에서 실행하면 "답변은 하지만 기록이 남지 않는" 상태가 됩니다. 따라서 `--data-dir`는 항상 명시하세요.
docs와 skills 디렉터리도 작업 디렉터리 기준 상대 경로 규칙이 동일합니다(`./slim/docs`, `./slim/skills`):
위의 구조를 쓰면 `/data`에서 실행하거나(`SLIM_DOCS_DIR` / `SLIM_SKILLS_DIR` 설정) 해야 하고, 그렇지
않으면 `/rag`가 아무것도 찾지 못합니다.
플래그/환경 변수/기본값 전체 표, 설치 명령, 설정을 모아 두는 wrapper 스크립트는 `slim/README.md`에
있습니다.

## CPE 자체에서 도는 모델 사용하기

agent가 아는 것은 OpenAI 호환 엔드포인트뿐이므로, llama.cpp server를 기기에서 돌리는 경우 달라지는
것은 `--base-url` 하나뿐입니다. LAN도, 다른 설정도 필요 없습니다:

```bash
# 기기: loopback에서 server 시작 (service entry 없음, 필요할 때 시작하고 kill로 중지)
cd /data/slim && nohup ./llama-server -m models/qwen2.5-0.5b-instruct-q4_k_m.gguf \
  --host 127.0.0.1 --port 8080 -c 2048 -t 4 > llama-server.log 2>&1 &

# agent를 그쪽으로 향하게
./slim-agent --data-dir /data/slim/data --session local --non-interactive \
  --model qwen2.5-0.5b-instruct --base-url http://127.0.0.1:8080/v1 --stream --once "Reply with one word: pong"
```

들어가는 모델 규모: CPE는 RAM 1.7 GB이고, `Qwen2.5-0.5B-Instruct-Q4_K_M`(469 MB, push에 47초)은
로드 후 RSS 646 MB, 약 1.1 GB가 남았습니다 — 1B를 크게 넘으면 들어가지 않습니다. 크로스 빌드한
server가 요구하는 것은 `libstdc++.so.6`, `libgcc_s.so.1`, musl `libc`뿐이며 모두 이미지에 있습니다.
`-t 4`에서 짧은 턴은 약 4초였고, server가 보고한 수치는 prompt eval 12 tok/s, 생성 7.9 tok/s입니다.
이 크기의 모델은 LAN에서 서비스하는 7B와 다릅니다. 답변이 약해지고, prompt에 보이는 tool JSON
형식을 내놓긴 하지만 불안정하므로, 액션을 확실하게 구동하려면 슬래시 명령(`/rag`, `/read`, `/write`,
`/mqtt`, `/history`)입니다. 호출마다
둘을 함께 쓸 수 있고, `--fallback-url`로 "로컬 우선, LAN은 비상용"으로 만들 수 있습니다. push 명령,
실측 수치, 주의 사항은 `slim/README.md`에 있습니다.

## 실제 하드웨어에서 검증한 항목

| 검증 항목 | CPE에서의 결과 |
| --- | --- |
| artifact 무결성 | `sha256 1e365e37...`이 호스트와 기기에서 일치(`NEEDED 0`, strip 완료) |
| `./slim/deploy/run-on-t830.sh` | exit 0: 슬래시 명령 쓰기 후 읽어오기, session 저장소, `--dry-run-writes`가 아무것도 쓰지 않음, fail-closed 거부 모두 정상 |
| LAN 경유 live model 턴 | 스트림 답변, 이어서 `--history 6`이 이전 턴의 숫자를 재현, `--history 0`은 재현하지 못함 — 음성 대조 |
| **모델이 기기 자체에서 도는 경우** | loopback의 `llama-server` + 0.5B Q4: 4초에 `PONG!`, prompt 12 tok/s, 생성 7.9 tok/s, `/history`가 그 턴들을 재생, 주 엔드포인트가 죽어도 `--fallback-url`이 응답 |
| session 저장소 | `sessions/<name>.jsonl`, 모드 `0600`, 한 줄에 JSON 객체 하나 |
| RAG 코퍼스, 재빌드 불필요 | `docs/`와 `skills/`에 넣은 markdown은 라이브로 검색됨: `/rag mt753x`, 다중 단어인 `/rag probe -22`, `/rag HotSpotFlag`, `/rag OWE` 모두 올바른 파일을 첫 번째로 반환 (2026-09-22) |
| 모델 실패(HTTP 400) | `HTTP status 400: <서버 메시지>`를 보고하고 session 파일을 쓰지 않음 |

이 트리를 다시 빌드하면 위와 **동일한** artifact가 나옵니다. 배포 전에 sha256을 비교하세요.
검증 표 전체, 기기 실측 사실, 배포 스크립트는 `slim/README.md`와 `slim/deploy/`에 있습니다.
