# third_party

## mbedtls/

mbedTLS **3.6.2**, vendored for the opt-in TLS builds (`make tls`, `make tls-test`,
`make t830-tls`, `make t830-static-tls`). Only the parts needed to compile the
library are kept: `include/`, `library/` and `LICENSE` (Apache-2.0). Tests,
programs and docs from the upstream release are not included.

- Upstream: https://github.com/Mbed-TLS/mbedtls
- Release: `mbedtls-3.6.2.tar.bz2` (github.com/Mbed-TLS/mbedtls/releases)
- License: Apache-2.0 (see `mbedtls/LICENSE`)

The default `host` / `t830` / `t830-static` builds do **not** use this; they stay
TLS-free so the on-device artifact keeps `NEEDED 0` and its small size. The
Makefile compiles `mbedtls/library/*.c` with `$(CC)` and archives the result, so
upgrading means replacing `include/` and `library/` and rebuilding.
