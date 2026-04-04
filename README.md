# nanocode

An AI coding agent in a single 350KB C binary. Zero dependencies. Runs anywhere.

```sh
git clone https://github.com/yourorg/nanocode && cd nanocode
make
sudo make install   # copies nanocode to /usr/local/bin
```

<!-- demo GIF goes here -->

## Why nanocode?

Every other AI coding agent requires a runtime — Python, Node.js, Go, Electron. nanocode is a single static binary that runs on any Linux or macOS machine with no install step beyond copying the file.

- **350KB** — smaller than most favicons
- **Zero runtime deps** — TLS via BearSSL, JSON via jsmn, both statically linked
- **Provider-agnostic** — Claude API, OpenAI-compatible endpoints, Ollama
- **Runs anywhere** — containers, air-gapped servers, CI/CD pipelines, `FROM scratch` Docker images
- **Arena allocator** — no heap fragmentation, predictable memory, no GC pauses

## Quick start

Set your API key and point nanocode at your project:

```sh
export ANTHROPIC_API_KEY=sk-ant-...
nanocode
```

Or use any OpenAI-compatible endpoint:

```sh
export OPENAI_API_KEY=...
export OPENAI_BASE_URL=http://localhost:11434/v1   # Ollama
nanocode --provider openai --model llama3
```

## Build

```sh
make              # release build
make DEBUG=1      # debug build with ASan + UBSan
make RELEASE=1    # hardened release build
```

Requires: a C11 compiler (gcc or clang). No other dependencies.

## Tools

nanocode ships with a core set of tools the agent can use:

| Tool | Description |
|------|-------------|
| `bash` | Run shell commands |
| `read_file` | Read file contents |
| `write_file` | Write or patch files |
| `grep` | Search file contents |
| `web_fetch` | Fetch URLs |

All tool output is treated as data — never re-parsed as instructions.

## Audit log

Every tool call and sandbox denial is written as structured JSONL to `~/.nanocode/audit.log`. No network telemetry.

## License

[MIT](LICENSE)
