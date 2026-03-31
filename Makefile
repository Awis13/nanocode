CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS :=
DESTDIR ?= /usr/local

# Debug build: make DEBUG=1
ifdef DEBUG
CFLAGS += -g -O0 -fsanitize=address,undefined -DDEBUG
LDFLAGS += -fsanitize=address,undefined
endif

# Hardened release build
ifdef RELEASE
CFLAGS += -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
LDFLAGS += -pie -Wl,-z,relro,-z,now
endif

SRC_DIRS := src src/core src/agent src/api src/tools src/tui src/util
SRCS     := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
OBJS     := $(SRCS:.c=.o)
BIN      := nanocode

VENDOR_DIRS := vendor/jsmn
INCLUDES    := -Iinclude -Ivendor/jsmn

.PHONY: all clean install test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN)

install: $(BIN)
	install -d $(DESTDIR)/bin
	install -m 755 $(BIN) $(DESTDIR)/bin/

test:
	$(MAKE) DEBUG=1 all
	./tests/run.sh
