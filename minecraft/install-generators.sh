#!/usr/bin/env bash
# Install the Minecraft LoadWalker harness onto OVH loadgen VMs (SoftMax pattern).
#
#   minecraft/install-generators.sh --run $RUN --dist /path/to/loadwalker-dist \
#       --clients-per-host 4
#
# $RUN must contain:
#   loadgen-fleet.json   from loadgen-fleet.mjs up
#   lt.env               LT_MC_HOST, LT_MC_PORT, LT_INDEX_WIDTH, …
#   control-token        bearer for :9109
#
# Does NOT require Buddy roster.json (offline Minecraft names).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
RUN=""; DIST=""; CPH=""; PORT=9109
while [ $# -gt 0 ]; do
  case "$1" in
    --run) RUN="${2:-}"; shift 2 ;;
    --dist) DIST="${2:-}"; shift 2 ;;
    --clients-per-host) CPH="${2:-}"; shift 2 ;;
    --port) PORT="${2:-}"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "install-generators: unknown $1" >&2; exit 2 ;;
  esac
done
die() { echo "install-generators: $*" >&2; exit 2; }
[ -n "$RUN" ] && [ -d "$RUN" ] || die "--run DIR required"
[ -n "$DIST" ] && [ -d "$DIST" ] || die "--dist DIR (loadwalker-dist) required"
[ -n "$CPH" ] && [ "$CPH" -gt 0 ] || die "--clients-per-host N required"
STATE="$RUN/loadgen-fleet.json"
[ -f "$STATE" ] || die "no $STATE"
[ -f "$RUN/lt.env" ] || die "no $RUN/lt.env"
[ -f "$RUN/control-token" ] || die "no $RUN/control-token"
command -v tailscale >/dev/null || die "tailscale required"
command -v jq >/dev/null || die "jq required"

NAMES=()
while IFS= read -r n; do
  [ -n "$n" ] && NAMES+=("$n")
done < <(jq -r '.vms[].name' "$STATE")
[ "${#NAMES[@]}" -gt 0 ] || die "no VMs in fleet"

declare -A ADDR
status="$(tailscale status)"
for n in "${NAMES[@]}"; do
  ips="$(printf '%s\n' "$status" | awk -v n="$n" '$2 == n || index($2, n "-") == 1 { if ($0 !~ /offline/) print $1 }')"
  count="$(grep -c . <<<"$ips" || true)"
  [ "$count" -eq 1 ] || die "$n: want 1 online tailnet node, got $count"
  ADDR[$n]="$ips"
done

TOKEN="$(cat "$RUN/control-token")"
TAR="$(mktemp -t ck-mc-dist.XXXXXX.tar.gz)"
tar -C "$DIST" -czf "$TAR" .
HARNESS_TAR="$(mktemp -t ck-mc-harness.XXXXXX.tar.gz)"
tar -C "$REPO" -czf "$HARNESS_TAR" minecraft/mc-harness.mjs minecraft/seam-pose.mjs \
    minecraft/package.json minecraft/package-lock.json minecraft/README.md

: > "$RUN/hosts.txt"
i=0
for n in "${NAMES[@]}"; do
  ip="${ADDR[$n]}"
  base=$(( i * CPH ))
  echo "== $n ($ip) indices $base..$(( base + CPH - 1 ))" >&2
  tailscale ssh "ubuntu@$ip" -- 'sudo mkdir -p /opt/ck-mc-loadwalker /opt/ck-mc-harness /var/lib/ck-mc-lt/stats && sudo chown -R ubuntu:ubuntu /opt/ck-mc-loadwalker /opt/ck-mc-harness /var/lib/ck-mc-lt'
  # Java 25
  tailscale ssh "ubuntu@$ip" -- 'command -v java >/dev/null || (sudo apt-get update -qq && sudo apt-get install -y -qq openjdk-25-jre-headless || sudo apt-get install -y -qq openjdk-21-jre-headless)'
  # Node
  tailscale ssh "ubuntu@$ip" -- 'command -v node >/dev/null || (curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash - && sudo apt-get install -y -qq nodejs)'
  cat "$TAR" | tailscale ssh "ubuntu@$ip" -- 'tar -xzf - -C /opt/ck-mc-loadwalker && chmod +x /opt/ck-mc-loadwalker/run.sh'
  cat "$HARNESS_TAR" | tailscale ssh "ubuntu@$ip" -- 'tar -xzf - -C /opt/ck-mc-harness && cd /opt/ck-mc-harness/minecraft && npm install --omit=dev --silent'
  env_file="$(mktemp)"
  {
    grep -Ev '^\s*(#|$)' "$RUN/lt.env"
    cat <<EOF
LT_CLIENTS=0
LT_INDEX_BASE=$base
LT_INDEX_LIMIT=$CPH
LT_INSTANCE_ID=$n
LT_CONTROL_BIND=$ip:$PORT
LT_CONTROL_TOKEN=$TOKEN
LT_MC_DIST=/opt/ck-mc-loadwalker
LT_STATS_DIR=/var/lib/ck-mc-lt/stats
# LT_MC_HOST / LT_MC_HOST_WEST / LT_MC_HOST_EAST / LT_LOGIN_STAGGER_MS come from $RUN/lt.env
EOF
  } > "$env_file"
  # CROWDY_* and LT_MC_HOST_* for seam-pose + west/east alternation must already be in $RUN/lt.env
  cat "$env_file" | tailscale ssh "ubuntu@$ip" -- 'cat > /opt/ck-mc-harness/lt.env && chmod 600 /opt/ck-mc-harness/lt.env'
  rm -f "$env_file"
  unit="$(mktemp)"
  cat > "$unit" <<EOF
[Unit]
Description=CK Minecraft LoadWalker harness
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/opt/ck-mc-harness
EnvironmentFile=/opt/ck-mc-harness/lt.env
ExecStart=/usr/bin/node /opt/ck-mc-harness/minecraft/mc-harness.mjs
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
  cat "$unit" | tailscale ssh "ubuntu@$ip" -- 'sudo tee /etc/systemd/system/ck-mc-harness.service >/dev/null && sudo systemctl daemon-reload && sudo systemctl enable --now ck-mc-harness.service'
  rm -f "$unit"
  echo "http://$ip:$PORT" >> "$RUN/hosts.txt"
  i=$(( i + 1 ))
done
rm -f "$TAR" "$HARNESS_TAR"
chmod 600 "$RUN/hosts.txt"
echo "wrote $RUN/hosts.txt (${#NAMES[@]} hosts)" >&2
echo "verify: curl -s -H \"Authorization: Bearer \$(cat $RUN/control-token)\" \$(head -1 $RUN/hosts.txt)/v1/status" >&2
