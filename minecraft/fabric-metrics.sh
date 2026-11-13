#!/usr/bin/env bash
# Sample CPU / RAM / NIC on the two Fabric game VMs (servers under test).
#
#   minecraft/fabric-metrics.sh --west 40.160.15.239 --east 40.160.99.136 \
#     --ssh-key ~/.local/state/cks-test-mc-cluster/id_ed25519 \
#     --out /path/to/run/fabric-metrics.jsonl --interval 5 --seconds 60
set -euo pipefail
WEST=""; EAST=""; KEY="${HOME}/.local/state/cks-test-mc-cluster/id_ed25519"
OUT=""; INTERVAL=5; SECONDS_N=60
while [ $# -gt 0 ]; do
  case "$1" in
    --west) WEST="${2:-}"; shift 2 ;;
    --east) EAST="${2:-}"; shift 2 ;;
    --ssh-key) KEY="${2:-}"; shift 2 ;;
    --out) OUT="${2:-}"; shift 2 ;;
    --interval) INTERVAL="${2:-}"; shift 2 ;;
    --seconds) SECONDS_N="${2:-}"; shift 2 ;;
    *) echo "fabric-metrics: unknown $1" >&2; exit 2 ;;
  esac
done
[ -n "$WEST" ] && [ -n "$EAST" ] && [ -n "$OUT" ] || { echo "need --west --east --out" >&2; exit 2; }
mkdir -p "$(dirname "$OUT")"
SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=accept-new)
sample() {
  local ip="$1" label="$2"
  "${SSH[@]}" "ubuntu@$ip" 'python3 -' <<'PY'
import json, time, os
def mem():
    d = {}
    for line in open("/proc/meminfo"):
        k, v = line.split(":")[0], line.split(":")[1].strip().split()[0]
        d[k] = int(v)
    total = d["MemTotal"]; avail = d.get("MemAvailable", d["MemFree"])
    return {"total_kb": total, "used_kb": total - avail, "used_pct": round(100*(total-avail)/total, 2)}
def cpu():
    def read():
        with open("/proc/stat") as f:
            parts = f.readline().split()
        nums = list(map(int, parts[1:]))
        idle = nums[3] + (nums[4] if len(nums) > 4 else 0)
        return idle, sum(nums)
    i1, t1 = read(); time.sleep(0.2); i2, t2 = read()
    dt = max(t2 - t1, 1); di = i2 - i1
    return {"busy_pct": round(100 * (1 - di/dt), 2)}
def nic():
    rows = {}
    for line in open("/proc/net/dev"):
        if ":" not in line or line.strip().startswith("Inter"): continue
        name, rest = line.split(":", 1)
        name = name.strip()
        cols = rest.split()
        rows[name] = {"rx_bytes": int(cols[0]), "tx_bytes": int(cols[8])}
    return rows
print(json.dumps({"cpu": cpu(), "mem": mem(), "nic": nic()}))
PY
}
end=$(( $(date +%s) + SECONDS_N ))
: > "$OUT"
while [ "$(date +%s)" -lt "$end" ]; do
  ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  for pair in "west:$WEST" "east:$EAST"; do
    label="${pair%%:*}"; ip="${pair#*:}"
    raw="$(sample "$ip" "$label" 2>/dev/null || echo '{}')"
    echo "{\"ts\":\"$ts\",\"host\":\"$label\",\"ip\":\"$ip\",\"sample\":$raw}" >> "$OUT"
  done
  sleep "$INTERVAL"
done
echo "fabric-metrics: wrote $OUT"
