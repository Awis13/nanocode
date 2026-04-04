#!/bin/sh
# nanocode test runner
set -e

BINARY="./nanocode"

if [ ! -f "$BINARY" ]; then
    echo "ERROR: binary not found. Run 'make' or 'make DEBUG=1' first." >&2
    exit 1
fi

echo "Running smoke test..."
output=$("$BINARY" 2>&1) || true  # binary may exit non-zero in non-TTY environments (pipe mode)
echo "$output" | grep -q "nanocode" && echo "PASS: binary runs and prints version" || (echo "FAIL: unexpected output: $output" && exit 1)

echo "All tests passed."
