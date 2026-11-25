# ck-mc-load

One process that flies many offline Minecraft 26.3 clients. It replaces a pile of LoadWalker JVMs for a mesh climb. Each bot is one TCP connection inside this process. The process does not chat, break blocks, or speak CK's UDP protocol.

The source is `cks-loadtest/minecraft/cpp`. Linux waits with epoll and an eventfd. macOS waits with kqueue and a pipe.

## What one bot does

1. Opens a TCP connection and logs in as an offline player. The UUID is Java's `UUID.nameUUIDFromBytes("OfflinePlayer:" + name)`. Load1 is `0992312c-c078-3fb3-907d-03a0edcf65ef`.
2. Waits for the server's position packet. That feet Y is `walkY`.
3. Flies `clearance` blocks above that Y (default 8), or at the Y an `altitude` command set. `onGround` is clear and the player-abilities flying bit is set. The server needs `allow-flight=true` or it will kick the bot for flying.
4. Sends, every client tick (50 ms, 20 Hz):
   - one position packet
   - forward input, and sprint on alternate 3 s
   - an arm swing every 40 ticks
   - a client tick end
5. In survival, asks to respawn when it dies and flies on from the new position.
6. Logs in again, after a delay, when its connection is lost after it was placed.
7. Stops for good at `--max-seconds` (default 180), when told to stop, when it could not get into the world the first time, or when it has used ten rejoins in a row.

`--max-seconds` counts from the bot's first tick. Time spent connecting or waiting to rejoin is part of it.

A relay handoff is not a transfer: the same TCP connection stays up, and the server that accepted the login plays the bot on the other side. When that handoff cannot start, the server sends `ClientboundTransferPacket`. The bot follows it, the way the game client and the headless Java client do: it reconnects to the named host, with handshake state 3, and keeps the cookies the last server stored. The log line is `Load1 transferred to host:port`. A relay crossing has no such line. A connection made to follow a transfer that closes before play is tried again at once, up to four times. After that it counts as a lost connection.

The bot does not answer an age menu. If the server is waiting for a birthday, it will hold the bot on the spot it spawned. Seed an adult answer first, or run a server that is not asking.

A large clearance in one packet is rejected. On the clearance rule the first move is a jump of the whole clearance. Clearance 8 is accepted. Clearance 50 shows up on the server as `moved too quickly` and the bot never walks. To fly high, leave clearance at 8 and use `altitude`, which climbs 0.4 blocks a tick. The mod's heightmap snap pulls a record down when it is within 64 blocks of the ground and the player is not already flying. The bot sends the flying bit before its first move. A climb through those 64 blocks has not been run against a real server.

## Paths

`seam` and `triangle` are the LoadWalker paths. The seam numbers are the local two-server mesh: the line is block x 2000, the margin is 8, fully west is x ≤ 1991, fully east is x ≥ 2008.

| Path | What it does |
| --- | --- |
| `seam` | Walk east to about x=2013, pause 4 s, walk west to about x=1986, pause 4 s, repeat, 0.21 blocks a tick (4.2 blocks/s). If the server position does not move toward the target for about 10 s, the bot steps 8 blocks off its Z lane and tries again. |
| `triangle` | Walk the waypoints in `--waypoints` / `LT_MC_WAYPOINTS`, shaped `x,y,z,side;x,y,z,side;...`, 0.21 blocks a tick. The y in a waypoint is not the fly height. `side` is `east`, `west`, or `north` and only labels the pause. |
| `wander` | Random headings inside a box. See below. |

## Wander

A wandering bot picks a heading and keeps it for 8 to 20 s (160 to 400 ticks, drawn each time). Then it turns 30 to 90 degrees, to the left or the right, and holds again. A turn is never under 30 degrees. It is never over 90, so the bot keeps going roughly the way it was. With turns of up to 180 degrees a bot doubles back half the time and stays near where it started.

Speed is the `bps` of the bot's `add` line (`--bps`, default 2, never under 0.05), in blocks per second. A `speed` command replaces it for every bot.

The box, the first of these that is set:

1. the rally square, while the bot is rallied
2. the bot's own box: the `waypoints` field of its `add` line, shaped `minX,minZ,maxX,maxZ`, when its path is `wander`
3. the process box: `--bounds minX,minZ,maxX,maxZ`, or the `bounds` command

With no box the bot wanders without limit.

At the edge: when the next step would leave the box, the heading is mirrored on that edge and the bot keeps its speed. It never stops there and never steps outside. A bot that finds itself outside (the box changed, or the server moved it) flies straight to the nearest spot one block inside, then wanders. Its hold timer does not run while it is outside.

The random numbers come from the bot's name: FNV-1a of the name seeds a splitmix64 generator. The same name draws the same headings, holds and rejoin jitter on every platform and in every run. The path still depends on where the server puts the bot and on when commands arrive.

How much of a box a bot sees depends on its speed. Measured with the wander code alone, 5,000 names, a 400 x 400 box, 20,000 ticks (1,000 s), starting 100 blocks from a corner, counting names that entered all four quadrants:

| Speed | All four quadrants |
| --- | --- |
| 2 blocks/s | 37% |
| 4 blocks/s | 92.5% |
| 6 blocks/s | 99.6% |
| 8 blocks/s | 99.98% |
| 10 blocks/s | 100% |

At 2 blocks/s a bot flies 2,000 blocks in that time, in legs of 16 to 40 blocks. Raise the speed to cross a large grid.

## Altitude

Without an `altitude` command the bot flies at the last server feet Y plus its clearance, as before.

`altitude y` makes every bot fly at feet Y `y`. A bot moves toward that Y by at most 0.4 blocks a tick (8 blocks/s) and then holds it. When the server sends a position, the climb starts again from the server's Y.

`altitude off` goes back to the clearance rule. A bot that is not at its clearance height moves there by at most 0.4 blocks a tick. Once there it follows the clearance rule again, and a new server position is again answered with the whole clearance in one move.

## Survival

The bot reads three more play packets.

- `SET_HEALTH` with health 0 or less, or `PLAYER_COMBAT_KILL` naming this player, is a death. The player's entity id is the first field of the play login packet. If that packet was too short to hold one, any combat kill counts. The two packets of one death count once.
- On a death the bot logs `Load1 died`, sends `ServerboundClientCommandPacket` with action 0 (perform respawn, serverbound play id 12), and stops sending positions. It keeps sending the client tick end. It asks again every 2 s until the server sends `RESPAWN`, or a health above 0.
- The first position packet after that `RESPAWN` ends the death. The bot accepts the teleport, sends player-loaded and the flying abilities, logs `Load1 respawned`, and flies on.
- A health above 0 with no `RESPAWN` before it (a rejoin that found the player alive) ends the death too, as soon as a position has arrived on that connection.
- A `RESPAWN` with no death before it (a dimension change, or a server re-sending state) logs `Load1 respawn packet, waiting for a position`. The bot stops sending positions until the next position packet, then sends player-loaded and the flying abilities and flies on.
- If no position arrives within 3 s (60 ticks) of a `RESPAWN`, and the bot already had a position on this connection, it logs `Load1 no position after respawn, flying on` and flies on from where it was. This also ends a death.
- A play login packet on a connection that is already in play is answered with player-loaded. It does not pause the bot.

A bot that loses its connection while dead is still dead when it rejoins. It asks to respawn on the new connection. That death is counted once.

## Rejoin

A bot that has been placed at least once logs in again when it loses its connection. Lost means: the socket closed, a socket error, a failed send or receive, a frame that cannot be read, a disconnect packet in login, configuration or play, a rejoin that could not connect, or more than 1 MB the server has not taken.

- The rejoin goes to the host and port of the bot's `add` line, not to the last transfer target. It is a fresh login: handshake state 2, no cookies.
- The bot waits first: 2 s, 4 s, 8 s, 16 s, then 30 s for every later attempt. Each wait is drawn within 25% either way.
- The log line is `Load1 rejoin N after: <reason>`, written when the connection is lost. `N` is the attempt, from 1.
- 120 s (2,400 ticks) of play on one connection, counted from its first position, sets the attempt count back to 0.
- After ten attempts in a row the bot ends with `Load1 closed: <reason> (10 rejoins used)`.
- A bot waiting to rejoin has no socket. `stats` counts it as `connecting`.

A bot that was never placed does not rejoin. It ends with `Load1 closed: <reason>`. `stop`, `stop-all`, the maximum runtime, a server that asks for encryption, and a transfer packet that names no usable host also end a bot for good.

## Crossing counter

`--grid originX,originZ,cellBlocks,cols,rows` (or the `grid` command) describes the mesh as square cells, one per server. Cell (0, 0) starts at the origin. A position is in column `floor((x - originX) / cellBlocks)` and row `floor((z - originZ) / cellBlocks)`. The cell number is `row * cols + col`. A position outside the grid has no cell.

Each bot remembers the last cell it was in on its current TCP connection. Every position the server sends and every position the bot reports is checked. When the cell is not the remembered one, that is one crossing: the process total and the bot's own count go up. The log line is `Load1 crossed c12->c13`, at most once per bot per 2 s (40 ticks). The counts are not limited that way.

- A new connection (a followed transfer, a rejoin) forgets the cell. The first position on it is not a crossing.
- A position outside the grid changes nothing. A bot that leaves the grid from cell 12 and comes back in cell 13 has crossed once, when it came back.
- A server teleport into another cell on the same connection, such as a respawn, is a crossing.
- A new `grid` command makes every bot forget its cell.

## Lines

Every bot line goes through one writer thread, so a slow disk or pipe cannot hold a worker. A worker only appends the line to a queue in memory.

- Without `--journal` the lines go to stdout, and to the bot's own log when it has one. The writer runs every 100 ms.
- With `--journal PATH` the lines go to `PATH` (appended), and to the bot's own log when it has one. Nothing goes to stdout. The writer runs every 500 ms and flushes once per run.
- A bot's own log is the `log` field of `add`, or `--log`. An empty path, `-` and `/dev/null` open no file.
- The queue holds at most 16 MB. Past that, lines are dropped and stderr gets `ck-mc-load: log queue was full, N lines dropped`.
- At a normal exit the queue is written out. A killed process loses what was queued, at most one writer interval.

These are the lines to count.

| Line | Meaning |
| --- | --- |
| `Load1 box 0.0000 0.0000 400.0000 400.0000` / `Load1 box off` | Printed on the bot's first tick: the wander box in force then (minX minZ maxX maxZ). |
| `Load1 connecting to 127.0.0.1:25599` | TCP connect started. Printed again for a followed transfer and for a rejoin. |
| `Load1 login packet, waiting for a position` | Configuration finished, play started, no position yet. |
| `Load1 at 1990.5000 74.0000 -55.0000` | The server's position packet. x y z are feet, four decimals. This is the line whose x changes side on a crossing the server confirms with a position packet. |
| `Load1 phase=play placed=1 x=... y=... z=... walkY=... dir=east ... transfers=N rejoins=N deaths=N crossings=N path=seam` | Still in play. Printed once when the bot is first placed on a connection, then every 30 s (600 ticks). Each bot has its own offset into those 600 ticks, taken from its name, so the bots do not all print in the same tick. `y` is the fly height. The four counts are this bot's own. |
| `Load1 east pause start` / `Load1 east pause done east_pauses=1` | Reached the east end of `seam` or a triangle point labeled east. Same shape for `west` and `north`. A pause by itself is not a crossing. |
| `Load1 blocked at x=1990.5000` | Server x has not moved toward the target. The bot is about to change Z lane. |
| `Load1 transferred to 127.0.0.1:25600` | The server could not keep the connection and named the next one. The bot joins it and keeps playing. |
| `Load1 reconfigured on the same connection` | The server sent the bot back to configuration. |
| `Load1 rejoin 1 after: connection closed` | The connection was lost. The bot will log in again after the wait for attempt 1. |
| `Load1 died` / `Load1 respawned` | A death, and the position that ended it. |
| `Load1 respawn packet, waiting for a position` | A `RESPAWN` that was not a death. |
| `Load1 no position after respawn, flying on` | 3 s passed after a `RESPAWN` with no position. |
| `Load1 crossed c12->c13` | The bot's grid cell changed on one connection. |
| `Load1 max runtime (~180s)` | `--max-seconds` ran out. The bot stops. There is no `closed:` line. |
| `Load1 closed: ...` | The bot ended and will not retry. Reasons include `connect 127.0.0.1`, `connect failed`, `resolve host`, `login disconnect`, `config disconnect`, `connection closed`, `socket error`, `encryption requested`, and any of the rejoin reasons followed by `(10 rejoins used)`. |

A bot ended by `stop` or `stop-all` prints no line. `list closed` shows it as `Load1:stopped`.

A relay crossing often does not print a new `at` line, because the server does not send another position packet on the original connection. The server log is the other record: `Load1 crossed into east's chunks and was handed on`. With a grid set, `crossed` is the client's record of it. A followed transfer does print `transferred to`, and the next `phase=play` line is on the new connection.

## Server requirements

The dedicated server has to be offline-mode. Whitelist the offline UUID above, not just the name. `allow-flight=true`. For a local mesh leave `server-ip=127.0.0.1`. The mod refuses offline mode when the listen address is not this machine, so do not bind `0.0.0.0` and do not put the client on a Docker bridge in front of that server.

A bot that joins the wrong side of a cluster for its saved position gets a transfer at login (`belongs on east; sending them there`). The bot follows that transfer and plays on the server that owns the spot.

Give servers by IP address. A host name is resolved on the worker thread, and the worker's other bots wait while it resolves.

## Build

C++20, OpenSSL (libcrypto, for the offline UUID's MD5), zlib, pthreads.

Linux:

```bash
cmake -S minecraft/cpp -B minecraft/cpp/build
cmake --build minecraft/cpp/build
minecraft/cpp/build/protocol_test
python3 minecraft/cpp/tests/control_smoke.py
```

macOS, Homebrew OpenSSL:

```bash
cmake -S minecraft/cpp -B minecraft/cpp/build \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build minecraft/cpp/build
minecraft/cpp/build/protocol_test
python3 minecraft/cpp/tests/control_smoke.py
```

With no `CMAKE_BUILD_TYPE` the build is `RelWithDebInfo` (optimized, with symbols). Before, it had no optimization flag at all, and a worker's tick took about five times as long. Pass `-DCMAKE_BUILD_TYPE=Debug` for an unoptimized build.

Do not compile the Linux epoll path on a Mac. It will not build.

Packet ids are already in `include/PacketIds.hpp` for protocol 777 (Minecraft 26.3). Do not regenerate them unless `minecraft_version` in the mod is no longer 26.3. The generator, if you ever need it, is `dump_packet_ids.py` against that version's deobfuscated common jar.

## Tests

Both have to pass before any bot touches a mesh. Neither needs a Minecraft server. `ctest` runs both.

`protocol_test` prints `protocol_test: ok`. It takes about 15 s. With one argument it runs one section: `unit`, `basics`, `survival`, `rejoin`, `closed` or `mass`.

| Section | What it checks |
| --- | --- |
| `unit` | Several control lines in one buffer and their replies in order. The `stats` reply. Wander: 64 names stepped 20,000 ticks at 10 blocks/s in a 400 x 400 box never leave it and enter all four quadrants; turns of 30 to 90 degrees; holds of 160 to 400 ticks; a bot outside flies straight in; the same name repeats its path. The altitude ramp. The rejoin schedule. Grid cells with negative coordinates. The respawn packet's bytes. The status line's 600-tick cadence. |
| `basics` | The offline UUID, framing, one `seam` bot's packets (flying bit, airborne move, forward and sprint input, arm swing), its own log file, and that ten bots in one process stay under one Java walker's 512 MB heap. |
| `survival` | Against a scripted server: `altitude` climbs 0.4 a tick; a kill of another player is ignored; health 0 brings one respawn request with action 0 and no moves; the respawn position brings player-loaded, the flying abilities and `respawned`; a bot outside its box flies back; a `RESPAWN` with no death waits for a position; with no position the bot flies on after 3 s; the crossing counter and its line. |
| `rejoin` | A transfer uses handshake state 3 and keeps a cookie. After the transfer target drops the socket, the bot comes back to the first server no sooner than 1.4 s later, with state 2 and no cookie. A play disconnect is a second rejoin. |
| `closed` | 300 bots at a closed port all end as closed with a reason, and a capped `list` ends with `...`. |
| `mass` | 400 bots on two workers are all placed, and the longest tick pass is under 50 ms. `rally` picks the first ten in play. `list` is in add order. |

On Linux the memory line reads `VmRSS` from `/proc/self/status`. On macOS it is the resident size from the Mach task info.

`tests/control_smoke.py [path/to/ck-mc-load]` starts the binary with `--control`, `--threads 2` and `--journal`, opens three control connections, sends 500 `add` lines in one write at a closed local port, and checks every reply, that `stats` on another connection always answers within 100 ms, `list closed`, fifteen commands in one write, the journal, an empty stdout, and that `quit` exits with code 0. It prints `control_smoke: ok`.

The scripted server in `protocol_test` waits up to 30 s for a bot's packet. On a busy Mac, loopback data has been seen to sit unacknowledged in the sender's queue for more than 5 s while the receiving bot had nothing to read. The test as it was before failed about one run in eight that way, and a probe with plain blocking sockets and none of this code showed stalls of up to 1.6 s. The waits are long so that such a stall delays a run instead of failing it.

## One bot

```bash
minecraft/cpp/build/ck-mc-load \
  --name Load1 --host 127.0.0.1 --port 25599 \
  --path seam --clearance 8 --max-seconds 180
```

The process exits when that bot ends.

| Flag | Default | Meaning |
| --- | --- | --- |
| `--name` | `Load1` | Offline username. |
| `--host`, `--port` | `127.0.0.1:25565` | Game server. The control socket is not this port. |
| `--path` | `seam` | `seam`, `triangle`, or `wander`. |
| `--waypoints` | empty | `triangle`: `x,y,z,side;...`. `wander`: this bot's box, `minX,minZ,maxX,maxZ`. |
| `--bps` | `2` | Wander speed, blocks per second. |
| `--clearance` | `8` | Blocks above the last server feet Y. |
| `--max-seconds` | `180` | Then the bot stops. |
| `--log` | off | This bot's own copy of its lines. |
| `--threads` | `1` | Worker threads. Bots are handed out round-robin. |
| `--control host:port` | off | Many bots, see below. |
| `--journal PATH` | off | Every bot's lines go to `PATH` and not to stdout. |
| `--bounds minX,minZ,maxX,maxZ` | off | The process wander box. |
| `--grid originX,originZ,cellBlocks,cols,rows` | off | The grid for the crossing counter. `cellBlocks` above 0, `cols` and `rows` 1 to 10000. |

`--journal`, `--bounds` and `--grid` work with one bot and with `--control`. A flag that cannot be read prints the reason to stderr and exits with code 2.

At start the process raises its open-file limit to the hard limit. Each bot is one descriptor.

## Many bots, one process

```bash
minecraft/cpp/build/ck-mc-load --control 127.0.0.1:9119 --threads 4 \
  --journal /var/tmp/ck-mc-load.journal --grid -5120,-5120,1024,10,10
```

The process listens on that IPv4 address and prints `ck-mc-load: control on 127.0.0.1:9119` to stderr.

- One thread accepts control connections and does nothing else. The listen backlog is 64.
- Each connection has its own thread. Up to 64 connections can be open at once. One more gets `err too many control connections` and is closed.
- A command is one line, fields separated by spaces or tabs, ended by `\n` or `\r\n`. A line is at most 64 KB.
- One write may hold many lines. Each command gets one reply line, in order: `ok`, `ok ...` or `err ...`. An empty line gets no reply.
- No command waits for a worker. `add` and the settings are put in the workers' inboxes and answered at once. `stats` and `list` read values the workers publish. The control threads take no lock a worker holds during socket I/O or logging.
- A client that stops reading its replies for 5 s is disconnected.

```text
add Load1 127.0.0.1 25599 wander 4 8 3600 - /dev/null
stats
list closed
bounds -5120 -5120 5120 5120
altitude 200
speed 6
rally 0 0 48 100
unrally
stop Load1
stop-all
quit
```

| Command | Reply | What it does |
| --- | --- | --- |
| `add name host port path bps clearance maxSeconds waypoints log` | `ok` | Adds a bot. Exactly nine fields. `waypoints` is `-` when there are none. `log` is a path, `-` or `/dev/null`; no spaces. `port` is 1 to 65535. |
| `stop name` | `ok` | Ends every bot with that name. |
| `stop-all` | `ok` | Ends every bot. |
| `quit` | `ok` | The process exits with code 0. Lines after `quit` in the same write are not run. |
| `stats` | `ok total=N ...` | Counts, see below. |
| `list phase` | `ok name1,name2,...` | Names of the bots in that phase, in the order they were added. |
| `bounds minX minZ maxX maxZ` | `ok` | The process wander box, for every bot now and later. A bot's own box still wins for that bot. `bounds off` removes it. |
| `rally x z radius [count]` | `ok rallied=N` | The first `count` bots in play, in add order (all when `count` is left out), fly to (x, z). `N` is how many were picked. |
| `unrally` | `ok` | Every rallied bot goes back to the box it had. |
| `speed bps` | `ok` | Wander speed for every bot now and later, 0.05 to 100. `speed off` gives each bot its own `bps` back. |
| `altitude y` | `ok` | Every bot now and later flies at feet Y `y`, -2048 to 100000. `altitude off` goes back to the clearance rule. |
| `grid originX originZ cellBlocks cols rows` | `ok` | Sets the crossing grid. `grid off` removes it. |

A command that cannot be read is answered `err` and a reason: `err unknown`, `err add needs 9 fields`, `err bad number x`, `err bad whole number x`, `err bad port x`, `err unknown phase x`, `err bounds needs min below max`, and so on.

`rally`: a picked bot flies straight to the point at its wander speed, whatever its path (4.2 blocks/s for a `seam` or `triangle` bot, unless `speed` is set). Once within `radius` blocks it wanders inside the square of that half-width around the point. A bot picked while it is logging in again goes there once it is placed. A second `rally` moves the bots it picks and leaves the others where they were sent. After `unrally` a `seam` or `triangle` bot takes up its path from where it is, and a wander bot outside its box flies back in.

### stats

```text
ok total=400 connecting=3 login=2 config=1 play=390 placed=388 closed=4 transfers=17 rejoins=5 deaths=9 respawns=8 crossings=1234 tick_ms_max=6
```

The first seven are how many bots are in each phase now.

| Field | Meaning |
| --- | --- |
| `total` | Bots added since the process started. `connecting + login + config + play + closed`. |
| `connecting` | No TCP connection yet: the connect is in progress, or the bot is waiting to rejoin. |
| `login` | Connected, login not finished. |
| `config` | In configuration. |
| `play` | In play. |
| `placed` | In play, with a position from the server, and flying. Part of `play`. A bot waiting for a position after a death or a `RESPAWN` is in `play` and not in `placed`. |
| `closed` | Ended, will not retry. |
| `transfers` | Transfer packets followed, since the process started. |
| `rejoins` | Rejoins scheduled, since the process started. One per `rejoin N after:` line. |
| `deaths`, `respawns` | Since the process started. |
| `crossings` | Grid cell changes, since the process started. |
| `tick_ms_max` | The longest tick pass of any worker since the last `stats` on any connection, in whole milliseconds, rounded. A tick pass is one worker ticking each of its bots once. The tick is 50 ms. |

The counts are read from atomics while the workers run. Two fields of one reply can be a tick apart.

### list

`list connecting`, `list login`, `list config`, `list play`, `list placed`, `list closed`. The reply is `ok` and one space, then the names with commas between them. With no bot in the phase the reply is `ok` alone. `list closed` gives `name:reason`. A comma in a reason is written `;`.

A reply is at most 64 KB with its newline. A longer list is cut at a whole name and ends with `,...`.

The process remembers at least the 4,096 closed bots added last. Older closed bots leave the list. They stay in `closed` and `total`.

`--threads` is how many worker threads share the bots. One thread is enough until the process is busy. Four are the right start when you are trying to fill several cores.

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

`LT_MC_DIST` is not required for `cpp`. The harness still does seam pose, age JSON, the whitelist, and the HTTP API. It sends the `add` line above. Login stagger defaults to 100 ms in cpp mode and 2 s in java mode. The harness does not send the newer commands or flags.

## Workers

`fleet.cpp` holds the only wait loop, and the one place where Linux and macOS differ in how they wait.

- Linux: epoll, and an eventfd a control thread writes so a command is picked up without waiting out the 50 ms tick.
- macOS: kqueue, and a nonblocking pipe in place of the eventfd. Packets, movement, and log lines are the same.

A bot belongs to one worker for its whole life. Only that worker's thread touches it. A control thread reaches a bot in two ways: a message in the worker's inbox, taken at the top of the worker's loop, and atomics the worker writes (the phase, the placed flag, the totals, the tick maximum).

A worker writes a bot's packets to the socket at the end of the bot's tick and at the end of each read. It asks the kernel to watch for a writable socket only while a connect is in progress or a write did not fit.

A closed bot's memory is freed on the next tick. Its name and reason stay for `list closed`.

## Scale

These are measurements, not a guarantee.

From one local run on 24 Nov 2026, before the changes above and with a build that had no optimization flag: against a login sink that accepts the 26.3 handshake and then only drains the socket, one process capped at 4 cores kept 7,000 bots at a full 20 Hz tick (about 1,750 bots per core, about 124 MB resident). 8,192 bots on that same 4-core cap did not all reach play. The game servers were not in that run. Two real servers fall over long before a 4-core client does.

On the local two-server mesh, each game server held to one core, view distance 6 and simulation distance 4, east owning only chunks x 124–127 and z −4–4: 64 bots on `seam` all crossed, no transfer, both servers still about 19.4 ticks per second. At 80 bots the rate was about 16. At 96 the east server was under 15. That crowd was in one neighborhood, crossing the same line, so each player was in view of the others.

From `protocol_test mass` on 24 Nov 2026, with the changes above: 400 wander bots on two workers, 200 each, against the scripted test server on loopback. That server sends nothing after the first position, so a real server's chunks and entities are not in these numbers.

| Build | Typical tick pass | Longest in 3 s of play |
| --- | --- | --- |
| macOS, optimized (the default now) | about 1 ms | 3 to 5 ms in 8 runs |
| macOS, no optimization flag | about 6 ms | 5 to 13 ms in about 80 runs, and 36 ms in one |
| Linux container on the same Mac, g++ 12 `-O2` | about 6.5 ms | 9 to 13 ms |

The Mac was busy with other work during these runs (load average 13 to 21 on 24 cores). In the container a pass took about 8 ms with no optimization and 6.5 ms with `-O2`, so most of it there is not this program's own code.
