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
`server/build/loadwalker-dist`.

## Run on a generator

```bash
export LT_CONTROL_BIND=100.x.y.z:9109   # tailnet address, never 0.0.0.0 without token
export LT_CONTROL_TOKEN=...
# West (even indices 0,2,4…). LT_MC_HOST alone still works as west-only.
export LT_MC_HOST_WEST=40.160.15.239
export LT_MC_HOST_EAST=40.160.99.136    # odd indices 1,3,5…; enables alternation
export LT_MC_PORT=25565                 # or LT_MC_PORT_WEST / LT_MC_PORT_EAST
export LT_LOGIN_STAGGER_MS=5000         # delay between each bot pose+login (default 5s)
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
poses the bot at the **same seam** (`seamTargetForIndex`), claims/saves on the
side it will join (`west` or `east`), then starts one LoadWalker against that
Fabric host. Bots are started **one at a time** with `LT_LOGIN_STAGGER_MS`
between them. Even global indices join west; odd join east — both walk the
locked east/west seam choreography toward/past each other.

If a bot dies in the early window (e.g. east join gets `ClientboundTransferPacket`
because the cluster says they belong on west), the harness **retries that slot**
(up to `LT_ADD_RETRIES`) and redirects to the transfer destination. If the hub
Pos write succeeds but the walker stays at **world spawn** (far from the seam,
often with `blocked` / `lane` in the log — the Load6–8 failure mode), that is a
**spawn-miss**: the harness kills the JVM, rewrites the seam pose with backoff,
and retries (preferring west after an east miss). A permanent failure advances
to the next index so the ramp does not deadlock waiting for `active` to recover,
and does **not** leave a stranded spawn bot counted as stable.

## Metrics

`/v1/stats` exposes:

- `active` — walkers still on one TCP session
- `transfers` — `ClientboundTransferPacket` count (failures)
- `east_pauses` / `west_pauses` — completed deep targets
- `relays` — east_pauses + west_pauses (same-socket crossings observed)

## Install on OVH loadgen VMs

See `minecraft/install-generators.sh` (SoftMax pattern: fleet from
`loadgen-fleet.mjs`, then ship dist + harness over Tailscale SSH).
