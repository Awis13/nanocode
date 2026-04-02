# nanocode

## Overview

nanocode is a zero-dependency AI coding agent written in C. It provides a Claude Code–style interactive experience in a ~350 KB static binary with no external runtime dependencies. It integrates with the Claude API and OpenAI-compatible providers (e.g., Ollama), manages multi-turn conversations, executes tools (bash, file I/O, grep, git), enforces OS-level sandboxing, and can operate non-interactively via its `--json` flag for scripting and agent-to-agent use.

## Installation

```bash
git clone <repo>
cd nanocode
make              # release build
sudo make install # installs to /usr/local/bin/nanocode
```

For a debug build with AddressSanitizer and UBSan:

```bash
make DEBUG=1
```

The binary has no shared-library runtime requirements beyond libc. BearSSL (TLS) is statically linked from `vendor/bearssl/`.

## CLI Flags

| Flag | Description |
|------|-------------|
| `--json` | Non-interactive JSON output mode — emits a single JSON object to stdout and exits |
| `--no-sandbox` | Disable sandbox enforcement (overrides config) |
| `--sandbox` | Force sandbox enabled (overrides config) |
| `--sandbox-profile <name>` | Override sandbox profile: `strict`, `permissive`, or `custom` |
| `--model <id>` | Model identifier (e.g., `claude-opus-4-5`, `ollama/llama3`) |
| `--session <id>` | Resume a named session |
| `--dry-run` | Parse and validate; do not call the API |
| `--read-only` | Disable all write tools for this session |
| `--pipe` | Read prompt from stdin, print response to stdout, exit |
| `--debug` | Verbose internal logging to stderr |
| `--timeout <duration>` | Session timeout (e.g., `30m`, `1h`, `90s`) |
| `--daemon` | Run as Unix socket daemon (`nanocode.sock`) |
| `--version` | Print version string and exit |
| `edit <path>[:<line>]` | Open a file in the user's terminal editor and exit |

## JSON Mode

When `--json` is passed (or `NANOCODE_JSON=1` with non-TTY stdout), nanocode suppresses all TUI output and emits a single JSON object on exit.

### Output Schema

```json
{
  "status":         "done",
  "result":         "Assistant final message text, or null",
  "files_modified": ["src/main.c", "include/foo.h"],
  "files_read":     ["README.md"],
  "tool_calls": [
    {
      "tool":        "bash",
      "args":        {"cmd": "make test"},
      "result_size": 1024
    }
  ],
  "duration_ms": 4321
}
```

### Field Descriptions

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | `"done"`, `"error"`, `"timeout"`, or `"sandbox_violation"` |
| `result` | string \| null | Final assistant message text; `null` if no response |
| `files_modified` | array of strings | Paths written or created during the session |
| `files_read` | array of strings | Paths read during the session |
| `tool_calls` | array of objects | Each tool invocation with name, args, and result byte count |
| `duration_ms` | integer | Wall-clock time in milliseconds from start to exit |

### Example Output

```json
{
  "status": "done",
  "result": "Added error handling to src/api/client.c and updated tests.",
  "files_modified": ["src/api/client.c", "tests/test_client.c"],
  "files_read": ["src/api/client.c", "include/client.h"],
  "tool_calls": [
    {"tool": "read_file",  "args": {"path": "src/api/client.c"}, "result_size": 3200},
    {"tool": "write_file", "args": {"path": "src/api/client.c"}, "result_size": 0},
    {"tool": "bash",       "args": {"cmd": "make test"},         "result_size": 512}
  ],
  "duration_ms": 8430
}
```

## Exit Codes

| Code | Constant | Meaning |
|------|----------|---------|
| `0` | `NC_EXIT_OK` | Success — task completed |
| `1` | `NC_EXIT_ERROR` | General error (API failure, config error, etc.) |
| `2` | `NC_EXIT_SANDBOX_VIOLATION` | Tool call blocked by sandbox policy |
| `3` | `NC_EXIT_TIMEOUT` | Session timed out before completion |

## Pipe Protocol

For non-interactive stdin/stdout use without full JSON output, pass `--pipe`:

```bash
echo "Add a docstring to src/main.c" | nanocode --pipe
```

- Reads the prompt from stdin (single message)
- Prints the assistant response to stdout
- Exits when the response is complete
- Does not write a status file or open a TUI

Combine with `--json` for machine-readable output:

```bash
echo "Fix the null dereference in src/api/client.c" | nanocode --pipe --json
```

## Environment Variables

| Variable | Description |
|----------|-------------|
| `NANOCODE_JSON` | Set to `1` to enable JSON output mode when stdout is not a TTY |
| `NANOCODE_API_KEY` | API key for the configured provider (overrides config file) |
| `VISUAL` | Preferred terminal editor (used by `nanocode edit`) |
| `EDITOR` | Fallback editor if `VISUAL` is not set |

## Integration Examples

### From a shell script

```bash
#!/bin/bash
result=$(echo "Summarise the changes in src/" | nanocode --pipe --json)
status=$(echo "$result" | jq -r '.status')
if [ "$status" != "done" ]; then
  echo "nanocode failed: $status" >&2
  exit 1
fi
echo "$result" | jq -r '.result'
```

### From Python subprocess

```python
import subprocess, json

proc = subprocess.run(
    ["nanocode", "--json"],
    input="Refactor src/util/arena.c to remove the global allocator",
    capture_output=True,
    text=True,
)
out = json.loads(proc.stdout)
print(out["status"], out["result"])
for f in out["files_modified"]:
    print("modified:", f)
```

### From another AI agent (Paperclip / Claude Code)

```bash
# In an agent tool call:
nanocode --json --pipe <<'EOF'
Add comprehensive error handling to src/api/client.c.
Cover all malloc failures and curl error codes.
EOF
```

The agent reads the JSON from stdout and inspects `status`, `files_modified`, and `result` to decide next steps. Exit code `0` means success; any other code should be treated as a failure and surfaced to the user.
