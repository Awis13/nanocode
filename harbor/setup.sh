#!/usr/bin/env bash
# setup.sh — Install nanocode inside a Terminal-Bench Debian container.
#
# Sourced by AbstractInstalledAgent.perform_task() after env vars are loaded.
# Expected env vars (set by agent.py):
#   ANTHROPIC_API_KEY   — required at runtime
#   NANOCODE_REPO_URL   — git repo to clone (default: GitHub)
#   NANOCODE_REF        — branch/tag/commit to check out (default: main)

set -euo pipefail

REPO_URL="${NANOCODE_REPO_URL:-https://github.com/Awis13/nanocode.git}"
REPO_REF="${NANOCODE_REF:-main}"
BUILD_DIR="/tmp/nanocode-build"
INSTALL_PREFIX="/usr/local"

echo "[nanocode setup] Installing build dependencies..."
apt-get update -qq
apt-get install -y --no-install-recommends \
    build-essential \
    git \
    ca-certificates \
    > /dev/null

echo "[nanocode setup] Cloning ${REPO_URL} @ ${REPO_REF}..."
rm -rf "$BUILD_DIR"
git clone --depth 1 --branch "$REPO_REF" "$REPO_URL" "$BUILD_DIR"

echo "[nanocode setup] Building nanocode (RELEASE=1)..."
cd "$BUILD_DIR"
make RELEASE=1

echo "[nanocode setup] Installing binary to ${INSTALL_PREFIX}/bin/..."
install -m 755 nanocode "${INSTALL_PREFIX}/bin/nanocode"

echo "[nanocode setup] Verifying install..."
nanocode --version 2>/dev/null || nanocode --help 2>/dev/null || true

echo "[nanocode setup] Done."
