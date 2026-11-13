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
export LT_MC_HOST=40.160.15.239         # west Fabric game VM
export LT_MC_PORT=25565
export LT_MC_DIST=/opt/ck-mc-loadwalker # loadwalker-dist tree
export LT_INDEX_BASE=0
export LT_INDEX_LIMIT=8
export LT_CLIENTS=0                     # wait for /v1/clients/add
export LT_STATS_DIR=/var/lib/ck-mc-lt/stats
export LT_INSTANCE_ID=test-mc-loadgen-1
node minecraft/mc-harness.mjs
```

Drive with the existing `cks-loadtest-ctl` against `hosts.txt`.

## Metrics

`/v1/stats` exposes:

- `active` — walkers still on one TCP session
- `transfers` — `ClientboundTransferPacket` count (failures)
- `east_pauses` / `west_pauses` — completed deep targets
- `relays` — east_pauses + west_pauses (same-socket crossings observed)

## Install on OVH loadgen VMs

See `minecraft/install-generators.sh` (SoftMax pattern: fleet from
`loadgen-fleet.mjs`, then ship dist + harness over Tailscale SSH).
