CXX ?= g++
CXXFLAGS ?= -O2 -std=c++11 -Wall
SRCS = agent.cpp http.cpp util.cpp policy.cpp
CORE = util.cpp policy.cpp http.cpp
BIN = slim-agent
TESTBIN = tests/test_slim

TC ?= /media/matt/D/public/fg370-t830-sdk_v1016/mtk_src/mtk/prebuilt/openwrt-toolchain/arm64-a55_neon-gcc-9.3.0_musl
CROSS_CXX = $(TC)/bin/aarch64-openwrt-linux-musl-g++
CROSS_BIN = slim-agent-t830

.PHONY: all host t830 test clean

all: host

host: $(BIN)

$(BIN): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

t830: $(CROSS_BIN)

$(CROSS_BIN): $(SRCS)
	STAGING_DIR=$(TC) $(CROSS_CXX) $(CXXFLAGS) -o $@ $(SRCS)

# Host-only: the core (util + policy) is platform independent, and these tests
# pin the regressions that shipped in 3eaa62e.
test: $(TESTBIN)
	./$(TESTBIN)

$(TESTBIN): tests/test_slim.cpp $(CORE)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_slim.cpp $(CORE)

clean:
	rm -f $(BIN) $(CROSS_BIN) $(TESTBIN)
