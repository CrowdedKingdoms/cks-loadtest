# ck-mc-load

One process, many offline Minecraft **26.3** clients. Each client logs in, flies
above the last server feet position, and sends the movement, sprint, and arm-swing
inputs a player sends. It does not chat, break blocks, or speak the CK UDP protocol.

Horizontal paths match LoadWalker so a mesh climb can still count seam crossings:

- `seam` — east to about x=2013, pause, west to about x=1986, pause, repeat
- `triangle` — `LT_MC_WAYPOINTS=x,y,z,side;...` (the y is not a fly height)
- `wander` — random heading at `LT_MC_WANDER_BPS` blocks per second (default 2)

`ClientboundTransferPacket` is logged as `TRANSFER FAILURE` and not followed.
The same stdout lines LoadWalker prints (`phase=play`, `at x y z`, `east pause done`)
go to stdout and, when the harness asks, to that bot's log file.

Flight is `LT_MC_FLY_CLEARANCE` blocks above the last server teleport (default 8),
with `onGround` clear and the player-abilities flying bit set. These servers already
run with `allow-flight=true`. The harness widens its near-seam Y check by the clearance.

Packet ids are generated from the deobfuscated 26.3 common jar:

```bash
python3 dump_packet_ids.py --jar /path/to/minecraft-common-deobf-26.3.jar --out include/PacketIds.hpp
```

## Build

```bash
cmake -S minecraft/cpp -B minecraft/cpp/build
cmake --build minecraft/cpp/build
minecraft/cpp/build/protocol_test
```

## One bot

```bash
minecraft/cpp/build/ck-mc-load --name Load1 --host 127.0.0.1 --port 25565 --path seam
```

## From the harness

`LT_MC_ENGINE=cpp` starts this binary and sends `add` on `LT_MC_CPP_CONTROL`
(default `127.0.0.1:9119`). Seam pose, age JSON, whitelist, and the HTTP API stay
in `mc-harness.mjs`. `LT_MC_DIST` is not required in this mode. `LT_MC_THREADS`
is the epoll worker count (default 1).

```text
add <name> <host> <port> <path> <bps> <clearance> <maxSeconds> <waypoints|-> <log>
stop <name>
stop-all
quit
```
