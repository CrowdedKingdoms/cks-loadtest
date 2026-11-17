#!/usr/bin/env bash
# SoftMax-style install / verify / collect for Minecraft LoadWalker generators.
#
# Mirrors infra-control-plane/scripts/ops/loadtest-generators.sh:
#   - one harness process per bare OVH VM (no containers)
#   - disjoint LT_INDEX_BASE / LT_INDEX_LIMIT slices from fleet order
#   - control bind on the host's OWN tailnet address :9109 (never 0.0.0.0)
#   - hosts.txt for fleet-ctl / cks-loadtest-ctl fan-out
#   - apt daily timers disabled; Restart=no so crashes stay visible
#
#   minecraft/install-generators.sh install --run $RUN --dist /path/to/loadwalker-dist \
#       --clients-per-host 25 [--port 9109]
#   minecraft/install-generators.sh verify  --run $RUN
#   minecraft/install-generators.sh collect --run $RUN
#   minecraft/install-generators.sh hosts   --run $RUN
#
# $RUN must contain:
#   loadgen-fleet.json   from minecraft/loadgen-fleet.mjs up
#   lt.env               LT_MC_HOST[_WEST|_EAST], CROWDY_*, LT_INDEX_WIDTH (optional), …
#   control-token        bearer for :9109
#
# Does NOT require Buddy roster.json (offline Minecraft names).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
CMD="${1:-}"; shift || true
RUN=""; DIST=""; CPH=""; PORT=9109
while [ $# -gt 0 ]; do
  case "$1" in
    --run) RUN="${2:-}"; shift 2 ;;
    --dist) DIST="${2:-}"; shift 2 ;;
    --clients-per-host) CPH="${2:-}"; shift 2 ;;
    --port) PORT="${2:-}"; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "install-generators: unknown $1" >&2; exit 2 ;;
  esac
done
die() { echo "install-generators: $*" >&2; exit 2; }
[ -n "$CMD" ] || die "usage: install|verify|collect|hosts --run DIR ..."
[ -n "$RUN" ] && [ -d "$RUN" ] || die "--run DIR required"
# SoftMax inventory may be a sibling of the one-box shortcut state:
#   LOADGEN_STATE=$RUN/loadgen-fleet-softmax.json
STATE="${LOADGEN_STATE:-$RUN/loadgen-fleet.json}"
[ -f "$STATE" ] || die "no $STATE (set LOADGEN_STATE or write loadgen-fleet.json)"
echo "install-generators: using fleet state $STATE" >&2
command -v tailscale >/dev/null || die "tailscale required"
command -v jq >/dev/null || die "jq required"
command -v curl >/dev/null || die "curl required"

NAMES=()
while IFS= read -r n; do
  [ -n "$n" ] && NAMES+=("$n")
done < <(jq -r '.vms[].name' "$STATE")
[ "${#NAMES[@]}" -gt 0 ] || die "no VMs in fleet"

# SoftMax: flavorActual / vcpus from what OVH built (recorded by loadgen-fleet.mjs).
declare -A FLAVOR VCPUS
flavor_vcpus() { case "$1" in b3-8) echo 2 ;; b3-16) echo 4 ;; b3-32) echo 8 ;; b3-64) echo 16 ;; *) echo "" ;; esac; }
while IFS=$'\t' read -r n f fa vc; do
  if [ -n "$fa" ] && [ "$fa" != null ]; then FLAVOR[$n]="$fa"; else FLAVOR[$n]="$f"; fi
  if [ -n "$vc" ] && [ "$vc" != null ]; then VCPUS[$n]="$vc"; else VCPUS[$n]="$(flavor_vcpus "${FLAVOR[$n]}")"; fi
done < <(jq -r '.vms[] | [.name, (.flavor // ""), (.flavorActual // ""), (.vcpus // "")] | @tsv' "$STATE")

declare -A ADDR
resolve_addrs() {
  local status ips count
  status="$(tailscale status)"
  for n in "${NAMES[@]}"; do
    ips="$(printf '%s\n' "$status" | awk -v n="$n" '$2 == n || index($2, n "-") == 1 { if ($0 !~ /offline/) print $1 }')"
    count="$(grep -c . <<<"$ips" || true)"
    [ "$count" -eq 1 ] || die "$n: want 1 online tailnet node, got $count"
    ADDR[$n]="$ips"
  done
}
resolve_addrs
tssh() { tailscale ssh "ubuntu@$1" -- "${@:2}"; }

write_hosts() {
  : > "$RUN/hosts.txt"
  for n in "${NAMES[@]}"; do
    echo "http://${ADDR[$n]}:$PORT" >> "$RUN/hosts.txt"
  done
  chmod 0600 "$RUN/hosts.txt"
  # SoftMax-style probe list (name role tailnetIp)
  : > "$RUN/probe-hosts.generators.txt"
  for n in "${NAMES[@]}"; do
    echo "$n generator ${ADDR[$n]} ${FLAVOR[$n]:-unknown}" >> "$RUN/probe-hosts.generators.txt"
  done
  chmod 0600 "$RUN/probe-hosts.generators.txt"
  echo "wrote $RUN/hosts.txt (${#NAMES[@]} hosts) and probe-hosts.generators.txt" >&2
}

check_nproc_all() {
  local n got rc=0
  echo "install-generators: checking nproc on ${#NAMES[@]} host(s)" >&2
  for n in "${NAMES[@]}"; do
    got="$(tssh "${ADDR[$n]}" nproc 2>/dev/null | tr -d '[:space:]')"
    if [ -n "${VCPUS[$n]:-}" ] && [ "$got" != "${VCPUS[$n]}" ]; then
      echo "FLAVOR MISMATCH $n: flavor ${FLAVOR[$n]} expects ${VCPUS[$n]} vCPU, nproc='$got'" >&2
      rc=1
    else
      echo "  $n nproc=$got flavor=${FLAVOR[$n]:-?}" >&2
    fi
  done
  [ "$rc" = 0 ] || die "nproc does not match fleet state; fix before install"
}

case "$CMD" in
  hosts) write_hosts ;;

  install)
    [ -n "$DIST" ] && [ -d "$DIST" ] || die "--dist DIR (loadwalker-dist) required"
    [ -n "$CPH" ] && [ "$CPH" -gt 0 ] || die "--clients-per-host N required"
    [ -f "$RUN/lt.env" ] || die "no $RUN/lt.env"
    [ -f "$RUN/control-token" ] || die "no $RUN/control-token"
    TOKEN="$(cat "$RUN/control-token")"
    check_nproc_all

    TAR="$(mktemp -t ck-mc-dist.XXXXXX.tar.gz)"
    tar -C "$DIST" -czf "$TAR" .
    HARNESS_TAR="$(mktemp -t ck-mc-harness.XXXXXX.tar.gz)"
    tar -C "$REPO" -czf "$HARNESS_TAR" \
      minecraft/mc-harness.mjs minecraft/seam-pose.mjs \
      minecraft/fleet-ctl.mjs minecraft/package.json \
      minecraft/package-lock.json minecraft/README.md

    i=0
    for n in "${NAMES[@]}"; do
      ip="${ADDR[$n]}"
      base=$(( i * CPH ))
      echo "== $n ($ip, flavor=${FLAVOR[$n]:-?}): indices $base..$(( base + CPH - 1 ))" >&2
      tssh "$ip" 'sudo mkdir -p /opt/ck-mc-loadwalker /opt/ck-mc-harness /var/lib/ck-mc-lt/stats && sudo chown -R ubuntu:ubuntu /opt/ck-mc-loadwalker /opt/ck-mc-harness /var/lib/ck-mc-lt'
      # SoftMax: no unattended upgrades mid-rung
      tssh "$ip" 'sudo systemctl disable --now apt-daily.timer apt-daily-upgrade.timer 2>/dev/null || true; sudo systemctl mask unattended-upgrades.service apt-daily-upgrade.service apt-daily.service 2>/dev/null || true'
      tssh "$ip" 'command -v java >/dev/null || (sudo apt-get update -qq && sudo apt-get install -y -qq openjdk-25-jre-headless || sudo apt-get install -y -qq openjdk-21-jre-headless)'
      tssh "$ip" 'command -v node >/dev/null || (curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash - && sudo apt-get install -y -qq nodejs)'
      cat "$TAR" | tssh "$ip" 'tar -xzf - -C /opt/ck-mc-loadwalker && chmod +x /opt/ck-mc-loadwalker/run.sh'
      cat "$HARNESS_TAR" | tssh "$ip" 'tar -xzf - -C /opt/ck-mc-harness && cd /opt/ck-mc-harness/minecraft && npm install --omit=dev --silent'
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
EOF
      } > "$env_file"
      cat "$env_file" | tssh "$ip" 'cat > /opt/ck-mc-harness/lt.env && chmod 600 /opt/ck-mc-harness/lt.env'
      rm -f "$env_file"
      unit="$(mktemp)"
      cat > "$unit" <<'EOF'
[Unit]
Description=CK Minecraft LoadWalker harness (SoftMax-style generator)
After=network-online.target tailscaled.service
Wants=network-online.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/opt/ck-mc-harness
EnvironmentFile=/opt/ck-mc-harness/lt.env
ExecStart=/usr/bin/node /opt/ck-mc-harness/minecraft/mc-harness.mjs
# SoftMax: a crash must stay visible rather than silently re-provision mid-rung.
Restart=no
LimitNOFILE=200000
KillSignal=SIGTERM
TimeoutStopSec=30

[Install]
WantedBy=multi-user.target
EOF
      cat "$unit" | tssh "$ip" 'sudo tee /etc/systemd/system/ck-mc-harness.service >/dev/null && sudo systemctl daemon-reload && sudo systemctl enable --now ck-mc-harness.service && sleep 2 && systemctl is-active ck-mc-harness.service'
      rm -f "$unit"
      i=$(( i + 1 ))
    done
    rm -f "$TAR" "$HARNESS_TAR"
    write_hosts
    echo "verify: $HERE/install-generators.sh verify --run $RUN" >&2
    ;;

  verify)
    [ -f "$RUN/control-token" ] || die "no $RUN/control-token"
    TOKEN="$(cat "$RUN/control-token")"
    rc=0
    i=0
    declare -A SEEN_BASE
    bases=()
    limits=()
    for n in "${NAMES[@]}"; do
      ip="${ADDR[$n]}"
      health="$(curl -sS --max-time 5 "http://$ip:$PORT/health" 2>&1 || echo FAIL)"
      noauth="$(curl -sS --max-time 5 -o /dev/null -w '%{http_code}' "http://$ip:$PORT/v1/status" 2>/dev/null || echo 000)"
      status="$(curl -sS --max-time 5 -H "Authorization: Bearer $TOKEN" "http://$ip:$PORT/v1/status" 2>/dev/null || echo '{}')"
      base="$(printf '%s' "$status" | jq -r '.index_base // .indexBase // "?"')"
      limit="$(printf '%s' "$status" | jq -r '.limit // .index_limit // "?"')"
      active="$(printf '%s' "$status" | jq -r '.active // "?"')"
      instance="$(printf '%s' "$status" | jq -r '.instance_id // .instanceId // "?"')"
      nproc_got="$(tssh "$ip" nproc 2>/dev/null | tr -d '[:space:]' || true)"
      unit="$(tssh "$ip" 'systemctl is-active ck-mc-harness.service 2>/dev/null || true' | tr -d '[:space:]' || true)"
      line="$n $ip flavor=${FLAVOR[$n]:-?} nproc=${nproc_got:-?}/${VCPUS[$n]:-?} health=${health:0:24} noauth=$noauth base=$base limit=$limit active=$active unit=$unit instance=$instance"
      bad=0
      [ "$noauth" = "401" ] || bad=1
      [ "$base" != "?" ] || bad=1
      [ "$unit" = "active" ] || bad=1
      if [ -n "${VCPUS[$n]:-}" ] && [ "$nproc_got" != "${VCPUS[$n]}" ]; then bad=1; fi
      if [ "$base" != "?" ]; then
        if [ -n "${SEEN_BASE[$base]:-}" ]; then
          echo "OVERLAP index_base=$base on $n and ${SEEN_BASE[$base]}" >&2
          bad=1
        fi
        SEEN_BASE[$base]="$n"
      fi
      if [ "$bad" = 1 ]; then
        echo "FAIL $line" >&2; rc=1
      else
        echo "ok   $line"
      fi
      bases+=("$base")
      limits+=("$limit")
      i=$(( i + 1 ))
    done
    # SoftMax contiguous slices when every host shares the same limit.
    if [ "${#limits[@]}" -gt 1 ] && [ "${limits[0]}" != "?" ]; then
      cph="${limits[0]}"
      for ((i=0; i<${#NAMES[@]}; i++)); do
        exp=$(( i * cph ))
        if [ "${limits[$i]}" != "$cph" ] || [ "${bases[$i]}" != "$exp" ]; then
          echo "FAIL shard geometry: ${NAMES[$i]} base=${bases[$i]} limit=${limits[$i]} want base=$exp limit=$cph" >&2
          rc=1
        fi
      done
    fi
    exit $rc
    ;;

  collect)
    mkdir -p "$RUN/generators"
    for n in "${NAMES[@]}"; do
      ip="${ADDR[$n]}"
      mkdir -p "$RUN/generators/$n"
      if tssh "$ip" 'sudo tar czf - -C /var/lib/ck-mc-lt .' | tar xzf - -C "$RUN/generators/$n"; then
        tssh "$ip" 'sudo journalctl -u ck-mc-harness --no-pager -o short-iso 2>/dev/null | tail -n 400' > "$RUN/generators/$n/journal.log" 2>/dev/null || true
        echo "collected $n -> $RUN/generators/$n" >&2
      else
        echo "collect $n FAILED" >&2
      fi
    done
    ;;

  *) die "unknown command $CMD (install|verify|collect|hosts)" ;;
esac
