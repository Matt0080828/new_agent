CXX ?= g++
CC ?= gcc
AR ?= ar
CXXFLAGS ?= -O2 -std=c++11 -Wall
SRCS = agent.cpp http.cpp util.cpp policy.cpp sse.cpp session.cpp tools.cpp
CORE = util.cpp policy.cpp http.cpp sse.cpp session.cpp tools.cpp
BIN = slim-agent
TESTS = tests/test_slim tests/test_sse tests/test_session tests/test_tools

# Reference CPE toolchain. Override on the command line when it lives elsewhere:
#   make t830-static TC=/path/to/arm64-a55_neon-gcc-9.3.0_musl
TC ?= /media/matt_chang/D/public/fg370-t830-sdk_v1016/mtk_src/mtk/prebuilt/openwrt-toolchain/arm64-a55_neon-gcc-9.3.0_musl
CROSS_CXX = $(TC)/bin/aarch64-openwrt-linux-musl-g++
CROSS_CC = $(TC)/bin/aarch64-openwrt-linux-musl-gcc
CROSS_AR = $(TC)/bin/aarch64-openwrt-linux-musl-ar
CROSS_BIN = slim-agent-t830
CROSS_STATIC_BIN = slim-agent-t830-static

# TLS (https://) is opt-in and vendored: mbedTLS 3.6.2 under third_party/. The default
# host/t830/t830-static builds stay TLS-free, so the on-device binary keeps `NEEDED 0`
# and its small size; `tls` / `t830-static-tls` link the TLS stack in on request.
MBEDTLS = third_party/mbedtls
MBEDTLS_INC = -I$(MBEDTLS)/include -I$(MBEDTLS)/library
MBEDTLS_SRCS = $(wildcard $(MBEDTLS)/library/*.c)
MBEDTLS_HOST_OBJS = $(patsubst $(MBEDTLS)/library/%.c,build/mbedtls-host/%.o,$(MBEDTLS_SRCS))
MBEDTLS_CROSS_OBJS = $(patsubst $(MBEDTLS)/library/%.c,build/mbedtls-t830/%.o,$(MBEDTLS_SRCS))
TLS_DEF = -DSLIM_WITH_TLS

.PHONY: all host tls t830 t830-static t830-tls t830-static-tls test tls-test clean

all: host

host: $(BIN)

$(BIN): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

# ---------------------------------------------------------------- TLS (host)
tls: $(BIN)-tls

$(BIN)-tls: $(SRCS) build/mbedtls-host/libmbedtls_all.a
	$(CXX) $(CXXFLAGS) $(TLS_DEF) $(MBEDTLS_INC) -o $@ $(SRCS) build/mbedtls-host/libmbedtls_all.a

build/mbedtls-host/%.o: $(MBEDTLS)/library/%.c
	@mkdir -p build/mbedtls-host
	$(CC) -O2 -fPIC $(MBEDTLS_INC) -c $< -o $@

build/mbedtls-host/libmbedtls_all.a: $(MBEDTLS_HOST_OBJS)
	$(AR) rcs $@ $^

# ---------------------------------------------------------------- cross builds
t830: $(CROSS_BIN)

$(CROSS_BIN): $(SRCS)
	STAGING_DIR=$(TC) $(CROSS_CXX) $(CXXFLAGS) -s -Wl,-rpath,/usr/lib:/lib -o $@ $(SRCS)

# Self-contained build: musl and libstdc++ are linked in, so the device needs no shared
# C++ runtime. OpenWrt images usually have none, and the C++ target is the one artifact
# whose libraries cannot be checked without logging into the device.
#
# -lgcc_eh must come AFTER the sources: linking it earlier leaves the link with
# `undefined reference to _Unwind_Resume`.
t830-static: $(CROSS_STATIC_BIN)

$(CROSS_STATIC_BIN): $(SRCS)
	STAGING_DIR=$(TC) $(CROSS_CXX) $(CXXFLAGS) -static -s -o $@ $(SRCS) -lgcc_eh

# ---------------------------------------------------------------- TLS (cross)
t830-tls: $(CROSS_BIN)-tls

$(CROSS_BIN)-tls: $(SRCS) build/mbedtls-t830/libmbedtls_all.a
	STAGING_DIR=$(TC) $(CROSS_CXX) $(CXXFLAGS) $(TLS_DEF) $(MBEDTLS_INC) -s -Wl,-rpath,/usr/lib:/lib -o $@ $(SRCS) build/mbedtls-t830/libmbedtls_all.a

t830-static-tls: $(CROSS_STATIC_BIN)-tls

$(CROSS_STATIC_BIN)-tls: $(SRCS) build/mbedtls-t830/libmbedtls_all.a
	STAGING_DIR=$(TC) $(CROSS_CXX) $(CXXFLAGS) $(TLS_DEF) $(MBEDTLS_INC) -static -s -o $@ $(SRCS) build/mbedtls-t830/libmbedtls_all.a -lgcc_eh

build/mbedtls-t830/%.o: $(MBEDTLS)/library/%.c
	@mkdir -p build/mbedtls-t830
	STAGING_DIR=$(TC) $(CROSS_CC) -O2 -fPIC $(MBEDTLS_INC) -c $< -o $@

build/mbedtls-t830/libmbedtls_all.a: $(MBEDTLS_CROSS_OBJS)
	$(CROSS_AR) rcs $@ $^

# ---------------------------------------------------------------- tests
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

$(TESTS): %: %.cpp $(CORE)
	$(CXX) $(CXXFLAGS) -o $@ $< $(CORE)

TLS_TESTS = $(addsuffix -tls,$(TESTS))

tls-test: $(TLS_TESTS)
	@for t in $(TLS_TESTS); do echo "== $$t"; ./$$t || exit 1; done

$(TLS_TESTS): %-tls: %.cpp $(CORE) build/mbedtls-host/libmbedtls_all.a
	$(CXX) $(CXXFLAGS) $(TLS_DEF) $(MBEDTLS_INC) -o $@ $< $(CORE) build/mbedtls-host/libmbedtls_all.a

clean:
	rm -f $(BIN) $(BIN)-tls $(CROSS_BIN) $(CROSS_BIN)-tls $(CROSS_STATIC_BIN) $(CROSS_STATIC_BIN)-tls $(TESTS) $(TLS_TESTS)
	rm -rf build
