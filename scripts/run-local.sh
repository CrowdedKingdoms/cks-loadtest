#!/usr/bin/env bash
# Run cks-loadtest against the local builder stack: one cks-game-api on :3000
# plus Buddies. Intended for development smoke tests.
#
# ONE ORIGIN, NOT TWO. This script defaulted the management URL to :3001 and the
# game URL to :3000, which described the split cks-management-api / cks-game-api
# deployment that has not existed since 2026-08-06. cks-game-api serves both
# surfaces and defaults to PORT 3000, so the old default pointed at nothing and
# every local run died in provisioning. Override LT_MANAGEMENT_API_URL if you run
# it on another port.
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
export LT_MANAGEMENT_API_URL="${LT_MANAGEMENT_API_URL:-http://127.0.0.1:3000}"
export LT_GAME_API_URL="${LT_GAME_API_URL:-http://127.0.0.1:3000}"
export LT_APP_ID="${LT_APP_ID:-1}"
export LT_CLIENTS="${LT_CLIENTS:-10}"
export LT_DURATION_SEC="${LT_DURATION_SEC:-30}"

exec "$BIN" "$@"
