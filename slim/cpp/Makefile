CXX ?= g++
CXXFLAGS ?= -O2 -std=c++11 -Wall
SRCS = agent.cpp http.cpp util.cpp policy.cpp sse.cpp session.cpp tools.cpp
CORE = util.cpp policy.cpp http.cpp sse.cpp session.cpp tools.cpp
BIN = slim-agent
TESTS = tests/test_slim tests/test_sse tests/test_session tests/test_tools

TC ?= /media/matt/D/public/fg370-t830-sdk_v1016/mtk_src/mtk/prebuilt/openwrt-toolchain/arm64-a55_neon-gcc-9.3.0_musl
CROSS_CXX = $(TC)/bin/aarch64-openwrt-linux-musl-g++
CROSS_BIN = slim-agent-t830
CROSS_STATIC_BIN = slim-agent-t830-static

.PHONY: all host t830 t830-static test clean

all: host

host: $(BIN)

$(BIN): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

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

# Host-only: the core is platform independent, and these tests pin the regressions
# that shipped in 3eaa62e plus the SSE/streaming and session contracts.
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

$(TESTS): %: %.cpp $(CORE)
	$(CXX) $(CXXFLAGS) -o $@ $< $(CORE)

clean:
	rm -f $(BIN) $(CROSS_BIN) $(CROSS_STATIC_BIN) $(TESTS)
