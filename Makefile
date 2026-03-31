CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS :=
DESTDIR ?= /usr/local

# Debug build: make DEBUG=1
ifdef DEBUG
CFLAGS  += -g -O0 -fsanitize=address,undefined -DDEBUG
LDFLAGS += -fsanitize=address,undefined
endif

# Hardened release build
ifdef RELEASE
CFLAGS  += -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
LDFLAGS += -pie -Wl,-z,relro,-z,now
endif

# ---------------------------------------------------------------------------
# Source collection
# ---------------------------------------------------------------------------

SRC_DIRS := src src/core src/agent src/api src/tools src/tui src/util
SRCS     := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))

# BearSSL: use pre-built static library from vendor/bearssl/build/
BEARSSL_LIB  := vendor/bearssl/build/libbearssl.a
BEARSSL_OBJS :=

OBJS     := $(SRCS:.c=.o)
BIN      := nanocode

INCLUDES := -Iinclude            \
            -Ivendor/jsmn        \
            -Ivendor/bearssl/inc \
            -Isrc

.PHONY: all clean install test asan bearssl

all: bearssl $(BIN)

bearssl:
	$(MAKE) -C vendor/bearssl

# ASan/UBSan convenience target
asan:
	$(MAKE) DEBUG=1 all test_stream

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(BEARSSL_LIB)

# ---------------------------------------------------------------------------
# test_stream — Phase 1 validation binary
# ---------------------------------------------------------------------------

TEST_STREAM_SRCS := bin/test_stream.c  \
                    src/core/loop.c    \
                    src/util/arena.c   \
                    src/util/buf.c     \
                    src/util/json.c    \
                    src/api/client.c   \
                    src/api/sse.c      \
                    src/api/provider.c

test_stream: bearssl $(TEST_STREAM_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INCLUDES) -o $@ $(TEST_STREAM_SRCS) $(BEARSSL_LIB)

# ---------------------------------------------------------------------------
# Compilation rules
# ---------------------------------------------------------------------------

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN) test_stream

install: $(BIN)
	install -d $(DESTDIR)/bin
	install -m 755 $(BIN) $(DESTDIR)/bin/

test:
	$(MAKE) DEBUG=1 all
	./tests/run.sh
