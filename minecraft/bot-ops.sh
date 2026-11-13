#!/usr/bin/env bash
# Whitelist + ages + seam playerdata for N offline Load bots on the OVH Fabric pair.
#
#   minecraft/bot-ops.sh --count 8 \
#     --west 40.160.15.239 --east 40.160.99.136 \
#     --ssh-key ~/.local/state/cks-test-mc-cluster/id_ed25519
set -euo pipefail
COUNT=0; WEST=""; EAST=""; KEY="${HOME}/.local/state/cks-test-mc-cluster/id_ed25519"
PREFIX=Load
while [ $# -gt 0 ]; do
  case "$1" in
    --count) COUNT="${2:-}"; shift 2 ;;
    --west) WEST="${2:-}"; shift 2 ;;
    --east) EAST="${2:-}"; shift 2 ;;
    --ssh-key) KEY="${2:-}"; shift 2 ;;
    --prefix) PREFIX="${2:-}"; shift 2 ;;
    *) echo "bot-ops: unknown $1" >&2; exit 2 ;;
  esac
done
die() { echo "bot-ops: $*" >&2; exit 2; }
[ "$COUNT" -gt 0 ] || die "--count N required"
[ -n "$WEST" ] && [ -n "$EAST" ] || die "--west and --east required"
[ -f "$KEY" ] || die "ssh key missing: $KEY"

SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=accept-new)

NAMES_CSV=""
for i in $(seq 1 "$COUNT"); do
  [ -n "$NAMES_CSV" ] && NAMES_CSV+=","
  NAMES_CSV+="${PREFIX}${i}"
done

prepare_host() {
  local ip="$1" side="$2" # west|east
  local wl="/opt/mc/${side}/whitelist.json"
  "${SSH[@]}" "ubuntu@$ip" "sudo NAMES_CSV='$NAMES_CSV' WL='$wl' SIDE='$side' python3 -" <<'PY'
import json, hashlib, uuid, os
names = os.environ["NAMES_CSV"].split(",")
path = os.environ["WL"]
side = os.environ["SIDE"]

def offline_uuid(name):
    h = hashlib.md5(("OfflinePlayer:" + name).encode()).digest()
    b = bytearray(h)
    b[6] = (b[6] & 0x0F) | 0x30
    b[8] = (b[8] & 0x3F) | 0x80
    return str(uuid.UUID(bytes=bytes(b)))

try:
    data = json.load(open(path))
except FileNotFoundError:
    data = []
by_name = {e.get("name"): e for e in data}
for n in names:
    by_name[n] = {"uuid": offline_uuid(n), "name": n}
out = list(by_name.values())
json.dump(out, open(path, "w"), indent=2)
print(f"whitelist {path}: {len(out)} entries")

# PlayerAges only reads <uuid>.json with born (yyyy-MM) + answered (yyyy-MM-dd).
# Plain date files are ignored → Join Ask() → world-spawn without CK → harness spawn-miss.
import datetime
today = datetime.date.today().isoformat()
payload = json.dumps({"born": "1990-01", "answered": today}, separators=(",", ":"))
for n in names:
    u = offline_uuid(n)
    for ages in [
        "/opt/mc/shared/ages",
        f"/opt/mc/{side}/Crowded Kingdoms/crowdedkingdoms/ages",
    ]:
        os.makedirs(ages, exist_ok=True)
        path = os.path.join(ages, f"{u}.json")
        open(path, "w").write(payload + "\n")
        try:
            os.chmod(path, 0o600)
        except OSError:
            pass
        # Drop legacy plain-uuid files so operators are not misled.
        plain = os.path.join(ages, u)
        if os.path.isfile(plain):
            try:
                os.remove(plain)
            except OSError:
                pass
    print(f"ages {n} {u} -> {payload}")
PY
}

prepare_host "$WEST" west
prepare_host "$EAST" east

# West playerdata at seam (local NBT; CK hub pose is authoritative — see fix-load-poses.mjs)
"${SSH[@]}" "ubuntu@$WEST" "sudo NAMES_CSV='$NAMES_CSV' python3 -" <<'PY' || echo "bot-ops: playerdata step skipped"
import hashlib, uuid, os
try:
    from nbtlib import File, Compound, Double, List as NList
except ImportError:
    print("nbtlib not on west; skip playerdata")
    raise SystemExit(0)

def offline_uuid(name):
    h = hashlib.md5(("OfflinePlayer:" + name).encode()).digest()
    b = bytearray(h)
    b[6] = (b[6] & 0x0F) | 0x30
    b[8] = (b[8] & 0x3F) | 0x80
    return uuid.UUID(bytes=bytes(b))

names = os.environ["NAMES_CSV"].split(",")
candidates = [
    "/opt/mc/west/Crowded Kingdoms/players/data",
    "/opt/mc/west/world/playerdata",
]
world = next((c for c in candidates if os.path.isdir(c)), None)
if not world:
    print("no playerdata dir")
    raise SystemExit(0)
for i, name in enumerate(names):
    u = offline_uuid(name)
    path = os.path.join(world, f"{u}.dat")
    x = 1990.5 + (i % 3) * 0.8
    z = -55.0 - (i % 5) * 0.6
    nbt = File.load(path, gzipped=True) if os.path.exists(path) else File(Compound(), gzipped=True)
    nbt["Pos"] = NList[Double]([Double(x), Double(63.0), Double(z)])
    nbt["Motion"] = NList[Double]([Double(0), Double(0), Double(0)])
    nbt.save(path, gzipped=True)
    print(f"playerdata {name} -> {x} 63 {z}")
PY

echo "bot-ops: prepared $COUNT bots ($NAMES_CSV) on west=$WEST east=$EAST"
