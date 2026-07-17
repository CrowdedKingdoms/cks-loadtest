#!/usr/bin/env bash
# Run cks-loadtest against the local builder stack (management api on :3001,
# game api on :3000, Buddies running). Intended for development smoke tests.
#
# Usage: scripts/run-local.sh [extra cks-loadtest args...]
set -euo pipefail

cd "$(dirname "$0")/.."

BIN=build/cks-loadtest
if [[ ! -x "$BIN" ]]; then
  echo "building..."
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build -j"$(nproc)" >/dev/null
fi

export LT_EMAIL="${LT_EMAIL:-loadtest-local@test.invalid}"
export LT_PASSWORD="${LT_PASSWORD:-loadtest-password-1}"
export LT_MANAGEMENT_API_URL="${LT_MANAGEMENT_API_URL:-http://127.0.0.1:3001}"
export LT_GAME_API_URL="${LT_GAME_API_URL:-http://127.0.0.1:3000}"
export LT_APP_ID="${LT_APP_ID:-1}"
export LT_CLIENTS="${LT_CLIENTS:-10}"
export LT_DURATION_SEC="${LT_DURATION_SEC:-30}"

exec "$BIN" "$@"
