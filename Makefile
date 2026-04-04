CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS := -lpthread
DESTDIR ?= /usr/local

.DEFAULT_GOAL := all

# Debug build: make DEBUG=1
ifdef DEBUG
CFLAGS  += -g -O0 -fsanitize=address,undefined -DDEBUG
LDFLAGS += -fsanitize=address,undefined
endif

# Hardened release build
ifdef RELEASE
CFLAGS  += -D_FORTIFY_SOURCE=2 -fstack-protector-strong
ifeq ($(shell uname),Linux)
CFLAGS  += -fPIE
LDFLAGS += -pie -Wl,-z,relro,-z,now
endif
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
            -Isrc                \
            -Isrc/core

# ---------------------------------------------------------------------------
# Unit tests — each test file is a separate binary
# ---------------------------------------------------------------------------

TEST_CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -g -O0 -DDEBUG
TEST_LDFLAGS := -lpthread

TEST_BINS := tests/test_arena tests/test_buf tests/test_json tests/test_executor \
             tests/test_fileops tests/test_bash tests/test_context tests/test_grep \
             tests/test_renderer tests/test_statusbar tests/test_diff_sandbox \
             tests/test_oom tests/test_retry tests/test_conversation \
             tests/test_prompt tests/test_lsp tests/test_input tests/test_repomap tests/test_git \
             tests/test_config tests/test_mcp tests/test_tool_display \
             tests/test_session tests/test_loop tests/test_memory \
             tests/test_tool_protocol \
             tests/test_sandbox \
             tests/test_editor \
             tests/test_session_timeout \
             tests/test_json_output \
             tests/test_status_file tests/test_daemon \
             tests/test_dryrun \
             tests/test_oneshot \
             tests/test_pet \
             tests/test_commands \
             tests/test_audit \
             tests/test_pipe \
             tests/test_history

tests/test_arena: tests/test_arena.c src/util/arena.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

tests/test_buf: tests/test_buf.c src/util/buf.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

tests/test_json: tests/test_json.c src/util/json.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -Ivendor/jsmn -o $@ $^

# CMP-210: audit.c is linked because executor.c calls audit_tool_call
tests/test_executor: tests/test_executor.c src/tools/executor.c src/util/arena.c \
                     src/util/json.c src/core/status_file.c src/core/audit.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

tests/test_fileops: tests/test_fileops.c src/tools/fileops.c \
                    src/util/arena.c src/util/buf.c src/core/audit.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

tests/test_bash: tests/test_bash.c src/tools/bash.c src/tools/executor.c \
                 src/util/arena.c src/util/json.c src/core/status_file.c src/core/audit.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

tests/test_context: tests/test_context.c src/agent/context.c src/util/arena.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

tests/test_grep: tests/test_grep.c src/tools/grep.c \
                 src/util/arena.c src/util/buf.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

tests/test_renderer: tests/test_renderer.c src/tui/renderer.c \
                     src/util/arena.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

tests/test_statusbar: tests/test_statusbar.c src/tui/statusbar.c src/tui/pet.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

tests/test_diff_sandbox: tests/test_diff_sandbox.c src/tools/diff_sandbox.c \
                          src/util/arena.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-144: OOM protection — requires arena_alloc to return NULL on exhaustion
tests/test_oom: tests/test_oom.c src/util/arena.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-144: retry/backoff — requires src/api/retry.c implementation
tests/test_retry: tests/test_retry.c src/api/retry.c src/api/thinking.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-118: conversation manager
tests/test_conversation: tests/test_conversation.c src/agent/conversation.c \
                         src/util/arena.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-121: system prompt builder
tests/test_prompt: tests/test_prompt.c src/agent/prompt.c src/agent/git.c \
                   src/tools/executor.c src/tools/memory.c src/util/arena.c \
                   src/util/buf.c src/util/json.c \
                   src/core/sandbox.c src/core/config.c src/core/status_file.c src/core/audit.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-147: repo map — symbol extraction, context injection
tests/test_repomap: tests/test_repomap.c src/agent/repomap.c src/util/arena.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-148: git integration — repo detection, status, tools
tests/test_git: tests/test_git.c src/agent/git.c src/tools/executor.c \
                src/util/arena.c src/util/buf.c src/util/json.c \
                src/core/status_file.c src/core/audit.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-141: config system — TOML parser, provider config
tests/test_config: tests/test_config.c src/core/config.c src/util/arena.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-150: MCP client — JSON-RPC 2.0, tool discovery, config
tests/test_mcp: tests/test_mcp.c src/agent/mcp.c src/tools/executor.c \
                src/util/arena.c src/util/buf.c src/util/json.c src/core/status_file.c src/core/audit.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-124: tool output display — invocation header, result truncation, diff colouring
tests/test_tool_display: tests/test_tool_display.c src/tui/tool_display.c \
                         src/tools/executor.c src/util/arena.c \
                         src/util/json.c src/core/status_file.c src/core/audit.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-183: session event log — bounded NDJSON with rotation
tests/test_session: tests/test_session.c src/core/session.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-180: event loop fd lookup optimization — O(1) bitmap-indexed sparse array
tests/test_loop: tests/test_loop.c src/core/loop.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-153: cross-session memory — memory_write tool and memory_load
tests/test_memory: tests/test_memory.c src/tools/memory.c src/tools/executor.c \
                   src/util/arena.c src/util/json.c src/core/status_file.c src/core/audit.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-119: tool use protocol — parse + dispatch tool calls, schema payload
tests/test_tool_protocol: tests/test_tool_protocol.c \
                           src/agent/tool_protocol.c \
                           src/agent/conversation.c \
                           src/tools/executor.c \
                           src/util/arena.c \
                           src/util/buf.c \
                           src/util/json.c \
                           src/core/status_file.c src/core/audit.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) $(TEST_LDFLAGS) -o $@ $^

# CMP-200: OS-level sandbox enforcement — macOS SBPL + Linux Landlock
tests/test_sandbox: tests/test_sandbox.c src/core/sandbox.c \
                    src/core/config.c src/util/arena.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -DSANDBOX_TEST -o $@ $^

# CMP-215: editor integration — $VISUAL/$EDITOR fallback, sandbox path check
tests/test_editor: tests/test_editor.c src/tools/editor.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-212: session timeout — parse_duration()
tests/test_session_timeout: tests/test_session_timeout.c src/util/duration.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-223: --json output mode
tests/test_json_output: tests/test_json_output.c src/core/json_output.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-216-A: status file — atomic write, remove, null-safe
tests/test_status_file: tests/test_status_file.c src/core/status_file.c \
                        src/util/json.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-216-A: daemon — Unix socket create/destroy/protocol
tests/test_daemon: tests/test_daemon.c src/core/daemon.c src/core/loop.c \
                   src/util/json.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-226: --dry-run and --readonly execution modes
tests/test_dryrun: tests/test_dryrun.c src/tools/executor.c src/util/arena.c \
                   src/util/json.c src/core/status_file.c src/core/audit.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-243: pet module — sprite data model + state machine
tests/test_pet: tests/test_pet.c src/tui/pet.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-189: one-shot mode — -c/--command flag parsing, auto-apply, exit codes
tests/test_oneshot: tests/test_oneshot.c src/core/oneshot.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-142: slash-command system (CMP-143: now includes history.c for /resume /search /export)
tests/test_commands: tests/test_commands.c src/tui/commands.c \
                     src/agent/conversation.c src/tools/diff_sandbox.c \
                     src/util/arena.c src/util/buf.c src/core/history.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-143: conversation history — save/resume/search/export
tests/test_history: tests/test_history.c src/core/history.c \
                    src/agent/conversation.c src/util/arena.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-210: structured audit log — JSONL tool call + sandbox denial logging
tests/test_audit: tests/test_audit.c src/core/audit.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-188: Unix pipe mode — provider resolution and stdin buffering
tests/test_pipe: tests/test_pipe.c src/util/buf.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-149: LSP client + shared JSON-RPC layer
tests/test_lsp: tests/test_lsp.c src/agent/lsp.c src/agent/jsonrpc.c \
                src/util/arena.c src/util/buf.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

# CMP-126: input system — line editor, history, tab completion
tests/test_input: tests/test_input.c src/tui/input.c src/util/arena.c
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -o $@ $^

.PHONY: all clean install test asan bearssl unit-test

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

TEST_STREAM_SRCS := bin/test_stream.c    \
                    src/core/loop.c      \
                    src/util/arena.c     \
                    src/util/buf.c       \
                    src/util/json.c      \
                    src/api/client.c     \
                    src/api/retry.c      \
                    src/api/tls_ca.c     \
                    src/api/sse.c        \
                    src/api/thinking.c   \
                    src/api/provider.c

test_stream: bearssl $(TEST_STREAM_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(INCLUDES) -o $@ $(TEST_STREAM_SRCS) $(BEARSSL_LIB)

# ---------------------------------------------------------------------------
# Compilation rules
# ---------------------------------------------------------------------------

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN) test_stream $(TEST_BINS) \
	      tests/test_grep tests/test_grep_asan \
	      tests/test_pipe tests/test_subagent

install: $(BIN)
	install -d $(DESTDIR)/bin
	install -m 755 $(BIN) $(DESTDIR)/bin/

unit-test: $(TEST_BINS)
	@failed=0; \
	for t in $(TEST_BINS); do \
	    $$t 2>&1 || failed=$$((failed + 1)); \
	done; \
	exit $$failed

test: unit-test $(BIN)
	./tests/run.sh
