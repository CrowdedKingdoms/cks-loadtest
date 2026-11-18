# Minecraft seam worker (cks-loadtest control shell)

This directory is the **Minecraft** worker for the SoftMax-style ladder. It speaks
the same HTTP control API as `cks-loadtest` (`/health`, `/v1/status`, `/v1/stats`,
`/v1/clients/add`, `/v1/rung/open`, `/v1/rung/close`, `/v1/shutdown`) but each
client is a **LoadWalker** JVM process (offline Minecraft 26.3 TCP), not Buddy UDP.

## Locked movement

Every bot must:

1. Walk ~5 blocks into east (`x ≈ 2013`), pause ~4s with jumps
2. Walk ~5 blocks into west (`x ≈ 1986`), pause ~4s with jumps
3. Repeat on the **same TCP session** — `ClientboundTransferPacket` is a failure

That choreography lives in CrowdedKingdoms `cks-minecraft-mod` as
`LoadWalker` (`server/.../dev/LoadWalker.java`). Build a portable dist with
`./gradlew :server:loadWalkerDist` and point `LT_MC_DIST` at
`server/build/loadwalker-dist`. It is the path when `LT_MC_HOST_NORTH` is unset.

## Three-server triangle

Set `LT_MC_HOST_NORTH` (and `COEFF_NORTH` for the driver) to run a closed
triangle instead of the even/odd seam. Vertices, in order:

1. west about `(1975, 1975)`
2. east about `(2025, 1975)`
3. north about `(2000, 2025)`

Each bot pauses at a vertex, walks to the next, and loops. Global index `% 3`
picks the first vertex, and the bot joins the server that owns it. Even/odd
placement is not used. A lap counts only after that bot has been on west,
east, and north.

The harness passes the loop to the JVM as `LT_MC_PATH=triangle` and
`LT_MC_WAYPOINTS` (`x,y,z,side` separated by `;`, this bot's vertex first).
`minecraft/coeff-series.py` with `COEFF_NORTH` set adds 10 bots and waits for
one lap each. It does not climb the two-server ladder. Leave `COEFF_NORTH`
unset and the ladder is unchanged.

## Run on a generator

```bash
export LT_CONTROL_BIND=100.x.y.z:9109   # tailnet address, never 0.0.0.0 without token
export LT_CONTROL_TOKEN=...
# West (even indices 0,2,4…). LT_MC_HOST alone still works as west-only.
export LT_MC_HOST_WEST=40.160.15.239
export LT_MC_HOST_EAST=40.160.99.136    # odd indices 1,3,5…; enables alternation
# export LT_MC_HOST_NORTH=203.0.113.10  # set for the triangle; drops even/odd
export LT_MC_PORT=25565                 # or LT_MC_PORT_WEST / LT_MC_PORT_EAST / LT_MC_PORT_NORTH
export LT_LOGIN_STAGGER_MS=2000         # delay between each bot pose+login (default 2s)
export LT_EARLY_STABLE_MS=20000         # watch window; early exit/transfer/spawn-miss → retry
export LT_ADD_RETRIES=2                 # extra attempts per slot after first
export LT_SEAM_POSE_ATTEMPTS=3          # hub Pos write attempts (backoff between)
export LT_SEAM_POSE_RETRY_MS=1500       # base backoff for pose rewrite (doubles each try)
export LT_SEAM_POSE_SETTLE_MS=750       # pause after successful hub Pos before JVM start
export LT_PLACEMENT_CONFIRM_MS=3000     # off-seam play time before spawn-miss retry
# Optional: LT_MC_ALTERNATE=0 to force west-only even when EAST is set
export LT_MC_DIST=/opt/ck-mc-loadwalker # loadwalker-dist tree
export LT_INDEX_BASE=0
export LT_INDEX_LIMIT=8
export LT_CLIENTS=0                     # wait for /v1/clients/add
export LT_STATS_DIR=/var/lib/ck-mc-lt/stats
export LT_INSTANCE_ID=test-mc-loadgen-1
# Required so every /v1/clients/add writes the bot's CK hub Pos to the seam
# before LoadWalker starts (otherwise Fabric places them at world spawn):
export CROWDY_ORG_TOKEN=...
export CROWDY_ID_PEPPER=...
export CROWDY_CLUSTER_SECRET=...
# Optional: CROWDY_API_ORIGIN (default https://ck.test.crowdedkingdoms.com), CROWDY_APP_ID
cd minecraft && npm install && cd ..
node minecraft/mc-harness.mjs
```

Drive with the existing `cks-loadtest-ctl` against `hosts.txt`. Each add:
poses the bot on the **home side of the seam** (`seamTargetForIndex` — west of
x=2000 for west joins, east of the line ~2009–2011 for east joins), claims/saves
on that side, then starts one LoadWalker against that Fabric host. Bots are
started **one at a time** with `LT_LOGIN_STAGGER_MS` between them. Even global
indices join west; odd join east — both walk the locked east/west seam
choreography toward/past each other.

If a bot dies in the early window (e.g. still gets `ClientboundTransferPacket`),
the harness **retries that slot** (up to `LT_ADD_RETRIES`) and redirects to the
transfer destination. The same transfer retry (exponential backoff, same attempt
budget) also applies **after** the bot is already stable — mid-run LoadWalker
exits on `ClientboundTransferPacket` no longer permanently drop `active`. If the
hub Pos write succeeds but the walker stays at **world spawn** (far from the
seam — the Load6–8 failure mode), that is a **spawn-miss**: the harness kills
the JVM, rewrites the seam pose with backoff, and retries on the **same** home
(no forced west fallback after an east miss). A permanent failure advances to
the next index so the ramp does not deadlock waiting for `active` to recover,
and does **not** leave a stranded spawn bot counted as stable. Max-runtime and
other non-transfer exits are not respawned.

## Metrics

`/v1/stats` exposes:

- `active` — walkers still on one TCP session
- `transfers` — `ClientboundTransferPacket` count (failures)
- `east_pauses` / `west_pauses` / `north_pauses` — pauses that finished (`east pause done` / `west pause done` / `north pause done`). A "pause start" line does not count.
- `relays` — same-socket crossings the server confirmed: the bot's reported position moved from one fully owned side to another. Two-server sides are west (x ≤ 1991) and east (x ≥ 2008). With a north host, north is z ≥ 2008, and west/east also require z ≤ 1991. A pause by itself is not a crossing. `transfers` remains the visible transfer-packet failure count.
- `laps` / `bots_lapped` — triangle only. A lap counts after that bot has been on west, east, and north. `unexpected_disconnects` counts a stable walker that left without the harness stopping it.

## SoftMax multi-VM fleet (preferred)

Crewrift SoftMax did **not** run bots as containers or as many JVMs on one small
box. It used **N bare Ubuntu 24.04 OVH VMs** (typically **b3-16**), Tailscale
`--ssh`, one harness process per VM with **disjoint** `LT_INDEX_BASE` /
`LT_INDEX_LIMIT`, and `cks-loadtest-ctl` (here: `fleet-ctl.mjs`) fanning
`POST /v1/clients/add` so each host adds the same per-host count.

The one-box shortcut `test-mc-loadgen-1` (b3-8) OOMed around ~25 Java walkers.
Keep it if useful; new capacity runs use a SoftMax-shaped prefix
(`test-mc-lg-*` by default) on **b3-16**.

### 1. Provision (test OVH project only)

```bash
RUN=$HOME/.local/state/cks-mc-loadtest-run
export LOADGEN_STATE=$RUN/loadgen-fleet-softmax.json
export LOADGEN_PREFIX=test-mc-lg          # SoftMax-shaped; leave test-mc-loadgen-1 alone
export LOADGEN_SECRETS_ENV=…/credentials.test.env
# up N [flavor] — SoftMax used 10× b3-16; start with 2–3 to prove sharding
node minecraft/loadgen-fleet.mjs up 3 b3-16
node minecraft/loadgen-fleet.mjs status
```

VMs stay up after every rung. Tear down **only** when Benjamin orders it:

```bash
node minecraft/loadgen-fleet.mjs down --confirm-benjamin-teardown
```

### 2. Install harness (disjoint index ranges)

```bash
# lt.env + control-token already in $RUN (same as one-box). Point LOADGEN_STATE
# at the SoftMax inventory when installing.
LOADGEN_STATE=$RUN/loadgen-fleet-softmax.json \
  minecraft/install-generators.sh install \
    --run $RUN --dist /path/to/loadwalker-dist --clients-per-host 25
minecraft/install-generators.sh verify --run $RUN
# hosts.txt now lists every generator; each owns [i*CPH, (i+1)*CPH)
```

### 3. Ramp (fan-out = SoftMax ctl)

`add --count N` means **N per host** (fleet total = N × hosts), same as
`cks-loadtest-ctl`:

```bash
CTL="node minecraft/fleet-ctl.mjs --run $RUN"
$CTL status
$CTL rung-open --id r-softmax-1
$CTL add --count 4
$CTL wait --active-delta 4 --timeout-sec 600
$CTL stats --out $RUN/r-softmax-1.stats.json
$CTL rung-close --id r-softmax-1 --out $RUN/r-softmax-1.fleet.json
```

You can also point a built `cks-loadtest-ctl --hosts $RUN/hosts.txt` at the
same control API.

## Install on OVH loadgen VMs (detail)

See `minecraft/install-generators.sh` and `minecraft/loadgen-fleet.mjs`.
Game VMs under test stay west `40.160.15.239` / east `40.160.99.136` — not loadgen.
