# ck-mc-load

One process that flies many offline Minecraft 26.3 clients. It replaces a pile of LoadWalker JVMs for a mesh climb. Each bot is one TCP connection inside this process. The process does not chat, break blocks, or speak CK's UDP protocol.

The source is `cks-loadtest/minecraft/cpp` on `feature/cpp-flying-load-clients` (`029bb9c`). Linux waits with epoll and eventfd. This builder also has a local macOS change that waits with kqueue and a pipe. That change is not in `029bb9c`. The Linux path is the same code it was.

## What one bot does

1. Opens a TCP connection and logs in as an offline player. The UUID is Java's `UUID.nameUUIDFromBytes("OfflinePlayer:" + name)`. Load1 is `0992312c-c078-3fb3-907d-03a0edcf65ef`.
2. Waits for the server's position packet. That feet Y is `walkY`.
3. Flies `clearance` blocks above that Y (default 8). `onGround` is clear and the player-abilities flying bit is set. The server needs `allow-flight=true` or it will kick the bot for flying.
4. Sends, every client tick (50 ms, 20 Hz):
   - a position packet, stepping 0.21 blocks (about 4.2 blocks/s) on `seam` and `triangle`
   - forward input, and sprint on alternate seconds
   - an arm swing every 40 ticks
   - a client tick end
5. Stops at `--max-seconds` (default 180), on disconnect, or when told to stop.

A relay handoff is not a transfer: the same TCP connection stays up, and the server that accepted the login plays the bot on the other side. When that handoff cannot start, the server sends `ClientboundTransferPacket`. The bot follows it, the way the game client and the headless Java client do: it reconnects to the named host, with handshake state 3, and keeps the cookies the last server stored. The log line is `Load1 transferred to host:port`. A relay crossing has no such line.

The bot does not answer an age menu. If the server is waiting for a birthday, it will hold the bot on the spot it spawned. Seed an adult answer first, or run a server that is not asking.

A large clearance in one packet is rejected. The first move is a jump of the whole clearance. Clearance 8 is accepted. Clearance 50 shows up on the server as `moved too quickly` and the bot never walks. To put bots high in the sky, put the spawn (or a teleport) up there and leave clearance at 8. The mod's heightmap snap pulls a record down when it is within 64 blocks of the ground and the player is not already flying, so a teleport has to land more than 64 blocks up if you want them to stay there.

## Paths

Same horizontal paths as LoadWalker. The seam numbers are the local two-server mesh: the line is block x 2000, the margin is 8, fully west is x ≤ 1991, fully east is x ≥ 2008.

| Path | What it does |
| --- | --- |
| `seam` | Walk east to about x=2013, pause 4 s, walk west to about x=1986, pause 4 s, repeat. If the server position does not move toward the target for about 10 s, the bot steps 8 blocks off its Z lane and tries again. |
| `triangle` | Walk the waypoints in `--waypoints` / `LT_MC_WAYPOINTS`, shaped `x,y,z,side;x,y,z,side;...`. The y in a waypoint is not the fly height. `side` is `east`, `west`, or `north` and only labels the pause. |
| `wander` | A random heading at `--bps` blocks per second (default 2). |

## Lines on stdout

These are the lines to count. They also go to `--log` when that path is set.

| Line | Meaning |
| --- | --- |
| `Load1 connecting to 127.0.0.1:25599` | TCP connect started. |
| `Load1 login packet, waiting for a position` | Configuration finished, play started, no position yet. |
| `Load1 at 1990.5000 74.0000 -55.0000` | The server's position packet. x y z are feet, four decimals. This is the line whose x changes side on a crossing the server confirms with a position packet. |
| `Load1 phase=play placed=1 x=... y=... z=... walkY=... dir=east ... transfers=N path=seam` | Still in play. Printed every 2 s. `y` is about `walkY + clearance`. `transfers` is how many transfer packets this bot has followed. |
| `Load1 east pause start` / `Load1 east pause done east_pauses=1` | Reached the east end of `seam` or a triangle point labeled east. Same shape for `west` and `north`. A pause by itself is not a crossing. |
| `Load1 blocked at x=1990.5000` | Server x has not moved toward the target. The bot is about to change Z lane. |
| `Load1 transferred to 127.0.0.1:25600` | The server could not keep the connection and named the next one. The bot joins it and keeps playing. |
| `Load1 closed: ...` | Socket closed. Reasons include `login disconnect`, `config disconnect`, `play disconnect`, `connection closed`, `max runtime`. |

A relay crossing often does not print a new `at` line, because the server does not send another position packet on the original connection. The server log is the other record: `Load1 crossed into east's chunks and was handed on`. A followed transfer does print `transferred to`, and the next `phase=play` line is on the new connection.

## Server requirements

The dedicated server has to be offline-mode. Whitelist the offline UUID above, not just the name. `allow-flight=true`. For a local mesh leave `server-ip=127.0.0.1`. The mod refuses offline mode when the listen address is not this machine, so do not bind `0.0.0.0` and do not put the client on a Docker bridge in front of that server.

A bot that joins the wrong side of a cluster for its saved position gets a transfer at login (`belongs on east; sending them there`). The bot follows that transfer and plays on the server that owns the spot.

`protocol_test` has to pass before any bot touches a mesh. It checks the offline UUID, the flying bit, the airborne move, forward and sprint input, the arm swing, and that ten bots in one process stay under one Java walker's 512 MB heap.

## Build

C++20, OpenSSL (libcrypto, for the offline UUID's MD5), zlib, pthreads.

Linux:

```bash
cmake -S minecraft/cpp -B minecraft/cpp/build
cmake --build minecraft/cpp/build
minecraft/cpp/build/protocol_test
```

macOS, Homebrew OpenSSL:

```bash
cmake -S minecraft/cpp -B minecraft/cpp/build \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build minecraft/cpp/build
minecraft/cpp/build/protocol_test
```

`protocol_test` prints `protocol_test: ok`. On Linux it reads `VmRSS` from `/proc/self/status`. On macOS the same line is the resident size from the Mach task info.

Packet ids are already in `include/PacketIds.hpp` for protocol 777 (Minecraft 26.3). Do not regenerate them unless `minecraft_version` in the mod is no longer 26.3. The generator, if you ever need it, is `dump_packet_ids.py` against that version's deobfuscated common jar.

## One bot

```bash
minecraft/cpp/build/ck-mc-load \
  --name Load1 --host 127.0.0.1 --port 25599 \
  --path seam --clearance 8 --max-seconds 180
```

The process exits when that bot finishes. `--log PATH` appends the same lines to a file.

| Flag | Default | Meaning |
| --- | --- | --- |
| `--name` | `Load1` | Offline username. |
| `--host`, `--port` | `127.0.0.1:25565` | Game server. The control socket is not this port. |
| `--path` | `seam` | `seam`, `triangle`, or `wander`. |
| `--waypoints` | empty | Triangle only. `x,y,z,side;...` |
| `--bps` | `2` | Wander speed, blocks per second. |
| `--clearance` | `8` | Blocks above the last server feet Y. |
| `--max-seconds` | `180` | Then the bot stops. |
| `--threads` | `1` | Worker threads. Bots are handed out round-robin. |
| `--control host:port` | off | Many bots, see below. |
| `--log` | off | Extra copy of this bot's lines. |

## Many bots, one process

```bash
minecraft/cpp/build/ck-mc-load --control 127.0.0.1:9119 --threads 4
```

The process listens on that IPv4 address, prints `ck-mc-load: control on 127.0.0.1:9119` to stderr, and accepts one control connection at a time. Commands are one line, fields separated by spaces. Every reply is `ok`, or `err ...`.

```text
add Load1 127.0.0.1 25599 seam 2 8 180 - /tmp/Load1.log
stop Load1
stop-all
quit
```

`add` is exactly nine fields after the word `add`:

```text
name host port path bps clearance maxSeconds waypoints log
```

`waypoints` is `-` when you are not on `triangle`. `log` is a path, or `/dev/null`. There must be no spaces in the path. The control parser splits on whitespace.

`--threads` is how many wait loops share the bots. One thread is enough until the process is busy. Four threads are the right start when you are trying to fill several cores. The help text still says "epoll workers" on both platforms.

## From the existing harness

Leave the Java default alone until you mean to switch.

```bash
export LT_MC_ENGINE=cpp
export LT_MC_CPP=minecraft/cpp/build/ck-mc-load   # default is that path
export LT_MC_CPP_CONTROL=127.0.0.1:9119
export LT_MC_THREADS=4
export LT_MC_FLY_CLEARANCE=8
export LT_MC_PATH=seam          # or triangle, or wander
export LT_MC_WAYPOINTS='1968,0,0,west;1976,0,0,east'
export LT_MC_WANDER_BPS=2
export LT_MC_MAX_RUNTIME_S=180
```

`LT_MC_DIST` is not required for `cpp`. The harness still does seam pose, age JSON, the whitelist, and the HTTP API. It sends the `add` line above. Login stagger defaults to 100 ms in cpp mode and 2 s in java mode.

## Wait loop

`fleet.cpp` is the only platform split.

- Linux: epoll, and an eventfd the control thread writes so a new bot is picked up without waiting out the 50 ms tick.
- macOS: kqueue, and a nonblocking pipe in place of the eventfd. Packets, movement, and log lines are unchanged.

Do not compile the Linux epoll path on a Mac. It will not build.

## Scale, from one local run on 24 Nov 2026

These are measurements, not a guarantee.

Against a login sink that accepts the 26.3 handshake and then only drains the socket, one process capped at 4 cores kept 7,000 bots at a full 20 Hz tick (about 1,750 bots per core, about 124 MB resident). 8,192 bots on that same 4-core cap did not all reach play. The game servers were not in that run. Two real servers fall over long before a 4-core client does.

On the local two-server mesh, each game server held to one core, view distance 6 and simulation distance 4, east owning only chunks x 124–127 and z −4–4: 64 bots on `seam` all crossed, no transfer, both servers still about 19.4 ticks per second. At 80 bots the rate was about 16. At 96 the east server was under 15. That crowd was in one neighborhood, crossing the same line, so each player was in view of the others.
