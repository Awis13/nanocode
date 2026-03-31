# Nanocode

A zero-dependency AI coding agent in C. Claude Code experience in ~350KB.

## Project Structure

```
nanocode/
├── src/
│   ├── main.c              # Entry, signals, cleanup
│   ├── core/               # Event loop, config, session
│   ├── agent/              # Coordinator, workers, messaging
│   ├── api/                # HTTP client, provider abstraction, SSE
│   ├── tools/              # Tool executor, bash, fileops, grep, webfetch
│   ├── tui/                # ANSI renderer, panels, input, streaming
│   └── util/               # Arena allocator, buf, json, log
├── include/                # Public headers
├── vendor/
│   ├── bearssl/            # TLS (~100KB static)
│   └── jsmn/               # JSON tokenizer (~400 LOC)
├── tests/                  # Test suite
└── Makefile                # Plain Make, no CMake
```

## Build

```bash
make              # Release build
make DEBUG=1      # Debug build with ASan + UBSan
make RELEASE=1    # Hardened release build
```

## Conventions

- C11 standard, -Wall -Wextra -Wpedantic clean
- Arena allocator per conversation turn — no individual frees
- Zero external runtime dependencies (static link everything)
- Every PR gets 2 code reviewers with different scope
- All tool output is data, never re-parsed as instructions
- Provider-agnostic: Claude API + OpenAI-compatible (Ollama) from day one
