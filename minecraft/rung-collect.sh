#!/usr/bin/env bash
# Start (or attach) per-rung artifact collection for an OVH Minecraft ladder step.
#
# Always starts fabric-metrics.sh (west/east CPU / RAM / NIC @ 5s) into
#   $RUN_DIR/fabric-metrics.jsonl
# for the duration of the rung (default 900s = covers 600s max-runtime + ramp).
#
# Usage:
#   minecraft/rung-collect.sh --run-dir DIR --west IP --east IP \
#     [--seconds N] [--interval 5] [--ssh-key PATH]
#
# Writes PID to $RUN_DIR/fabric-metrics.pid; stop with:
#   kill "$(cat $RUN_DIR/fabric-metrics.pid)"
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_DIR=""; WEST=""; EAST=""
KEY="${HOME}/.local/state/cks-test-mc-cluster/id_ed25519"
INTERVAL=5
SECONDS_N=900
while [ $# -gt 0 ]; do
  case "$1" in
    --run-dir) RUN_DIR="${2:-}"; shift 2 ;;
    --west) WEST="${2:-}"; shift 2 ;;
    --east) EAST="${2:-}"; shift 2 ;;
    --seconds) SECONDS_N="${2:-}"; shift 2 ;;
    --interval) INTERVAL="${2:-}"; shift 2 ;;
    --ssh-key) KEY="${2:-}"; shift 2 ;;
    -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "rung-collect: unknown $1" >&2; exit 2 ;;
  esac
done
[ -n "$RUN_DIR" ] && [ -n "$WEST" ] && [ -n "$EAST" ] || {
  echo "rung-collect: need --run-dir --west --east" >&2
  exit 2
}
mkdir -p "$RUN_DIR"
OUT="$RUN_DIR/fabric-metrics.jsonl"
PIDFILE="$RUN_DIR/fabric-metrics.pid"
LOG="$RUN_DIR/fabric-metrics.out"
if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
  echo "rung-collect: fabric-metrics already running pid=$(cat "$PIDFILE") -> $OUT" >&2
  exit 0
fi
nohup bash "$HERE/fabric-metrics.sh" \
  --west "$WEST" --east "$EAST" --ssh-key "$KEY" \
  --out "$OUT" --interval "$INTERVAL" --seconds "$SECONDS_N" \
  >"$LOG" 2>&1 &
echo $! >"$PIDFILE"
echo "rung-collect: fabric-metrics pid=$(cat "$PIDFILE") interval=${INTERVAL}s seconds=${SECONDS_N} -> $OUT" >&2
