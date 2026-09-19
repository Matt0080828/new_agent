# t830-slim-agent

[English](README.md) | [繁體中文](README.zh-TW.md) | [简体中文](README.zh-CN.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | **Español**

Un agente pequeño que se ejecuta en el CPE Askey T830 / Fibocom FG370 (OpenWrt 23.05.5, aarch64 musl,
**sin python3 en la imagen**): conversación por un endpoint compatible con OpenAI, RAG por palabras clave
sobre markdown, una capa de herramientas con lista blanca, habilidades en markdown — y una política
fail-closed delante de cualquier escritura.

Dos clientes, un mismo comportamiento: `slim/cpp/` es el que corre en el CPE (compilado de forma cruzada
para aarch64 musl, con una variante estática que no necesita nada instalado) y `slim/` contiene un cliente
en Python con las mismas funciones para máquinas que sí tienen Python. Este proyecto es independiente: no
es Hermes ni el fork Hermes IoT/Pi2. Se desarrolló dentro del fork de Hermes en `Matt0080828/new_agent` y
se extrajo de allí (`cc8a67c`) a un repositorio propio, sin historia compartida con ese fork.

```text
slim/            Cliente Python: política, almacén de sesiones, herramientas, RAG, pruebas
slim/cpp/        Cliente C++ con las mismas funciones, más cuatro binarios de prueba
slim/deploy/     run-on-t830.sh y on-device-smoke.sh — la ruta de despliegue
slim/README.md   El manual: compilación, despliegue, instalación, configuración, modelo en el dispositivo (en inglés)
```

## Compilar y probar en el equipo anfitrión

```bash
cd slim/cpp && make && make test                              # 81 + 54 + 39 + 53 = 227 checks
cd slim && python3 -m unittest discover -s . -p 'test_*.py'    # 97 tests, solo biblioteca estándar
```

## Compilar para el CPE

```bash
make -C slim/cpp t830           # enlazado dinámico, 138,240 bytes; necesita libstdc++/libgcc en el dispositivo
make -C slim/cpp t830-static    # 715,048 bytes, NEEDED 0, sin símbolos; no hay que instalar nada allí
```

## Ponerlo en el CPE

```bash
./slim/deploy/run-on-t830.sh    # contenedor + adb, copia, sha256 en ambos lados, prueba de humo en el dispositivo
```

Al dispositivo solo se llega por **adb sobre USB**: su IP de gestión responde a ARP pero todos los puertos
TCP están cerrados, `/dev/ttyACM*` son puertos AT del módem y el gadget RNDIS no reparte DHCP. `adb` se
ejecuta dentro de un contenedor privilegiado porque el nodo USB es solo para root, y un contenedor no puede
leer rutas del anfitrión, así que los scripts pasan cada archivo por `docker cp` antes de copiarlo.

## Dónde instalarlo y cómo se configura

`/tmp` es tmpfs, así que la copia que se envía desaparece al reiniciar. Para instalarlo, usa `/data`
(12,5 GB libres) o `/overlay` (116 MB); ninguno está montado con `noexec`. La disposición que asumen los
scripts y el manual es `/data/slim/{slim-agent,run,data,skills,docs}`: 700 KB más el corpus que añadas.

El cliente C++ **no tiene archivo de configuración**: solo opciones y variables de entorno, y **una opción
gana a la variable de entorno**. Sus valores por defecto son relativos al directorio de trabajo
(`--data-dir` es `./slim/data`), así que ejecutarlo desde la raíz de solo lectura responde pero no registra
nada; pasa siempre `--data-dir`. La tabla completa de opciones, variables y valores por defecto, los comandos
de instalación y un script envoltorio que guarda la configuración están en `slim/README.md`.

## Usar un modelo que se ejecuta en el propio T830

El agente solo conoce un endpoint compatible con OpenAI, así que un servidor de llama.cpp en el dispositivo
es un cambio de `--base-url` y nada más: sin LAN y sin tocar ninguna otra opción.

```bash
# En el dispositivo: arranca el servidor en loopback (no hay entrada de servicio; bajo demanda, se para con kill)
cd /data/slim && nohup ./llama-server -m models/qwen2.5-0.5b-instruct-q4_k_m.gguf \
  --host 127.0.0.1 --port 8080 -c 2048 -t 4 > llama-server.log 2>&1 &

# El agente apuntando ahí
./slim-agent --data-dir /data/slim/data --session local --non-interactive \
  --model qwen2.5-0.5b-instruct --base-url http://127.0.0.1:8080/v1 --stream --once "Reply with one word: pong"
```

Qué cabe: el CPE tiene 1,7 GB de RAM y `Qwen2.5-0.5B-Instruct-Q4_K_M` (469 MB, enviados en 47 s) cargó
con 646 MB de RSS y aún ~1,1 GB libres; mucho más allá de 1B no entra. El servidor compilado de forma
cruzada solo necesita `libstdc++.so.6`, `libgcc_s.so.1` y la `libc` de musl, presentes en la imagen. Con
`-t 4` un turno corto tarda ~4 s y el servidor informó 12 tok/s de prompt eval y 7,9 tok/s de generación.
Un modelo de este tamaño no es el 7B que puedes servir desde la LAN: espera respuestas más flojas y
dirígelo con los comandos de barra (`/rag`, `/read`, `/write`, `/mqtt`, `/history`), porque no emite tool
JSON fiable. Ambos pueden convivir por invocación, y `--fallback-url` da «primero local, LAN como escape».
`slim/README.md` tiene los comandos de copia, las cifras medidas y las advertencias.

## Verificado en hardware real

| Comprobación | Resultado en el CPE |
| --- | --- |
| integridad del artefacto | `sha256 1c2c17b3...` coincide en anfitrión y dispositivo (`NEEDED 0`, sin símbolos) |
| `./slim/deploy/run-on-t830.sh` | exit 0: comandos de barra con relectura, el almacén de sesiones, `--dry-run-writes` sin escribir nada y todos los rechazos fail-closed |
| turno en vivo por la LAN | respuesta en streaming y después `--history 6` recordando el número del turno anterior; `--history 0` sin recordarlo — el control negativo |
| **modelo en el propio dispositivo** | `llama-server` en loopback + un 0.5B Q4: `PONG!` en 4 s, 12 tok/s de prompt y 7,9 tok/s de generación, `/history` reproduciendo esos turnos y `--fallback-url` respondiendo con el endpoint principal caído |
| almacén de sesiones | `sessions/<name>.jsonl`, modo `0600`, un objeto JSON por línea |
| modelo que falla (HTTP 400) | informa `HTTP status 400: <mensaje del servidor>` y no escribe archivo de sesión |

Recompilar este árbol reproduce exactamente ese artefacto, así que compara el sha256 antes de desplegar.
Las tablas completas de verificación, los datos medidos en el dispositivo y los scripts de despliegue están
en `slim/README.md` y `slim/deploy/`.
