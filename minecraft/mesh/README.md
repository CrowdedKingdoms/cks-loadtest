# A mesh of Minecraft servers under load

The tools that put 3,009 players in one survival world on 2026-11-25: a grid of CrowdyCraft servers (the server mod in
`cks-minecraft-mod`), each running its own chunks, with the C++ load client's bots wandering across all of them and
real game clients among them. The record of that run is
`cks-project-root/project-root-docs/HANDOFF-2026-11-25-minecraft-mesh-3000.md`; what the mod had to change is in
`cks-minecraft-mod/docs/seamless-handoff.md`, "A hundred servers, 3,000 players".

| File | What it does |
| --- | --- |
| `mesh.py` | Plans a grid, builds the payload, brings servers up (locally or on VMs over Tailscale SSH), reads every server's meter, adds bots through the generators |
| `scenario.py` | Plays one of the mod's two-server scenarios on a pair of a running mesh's servers |
| `meshcam/` | A Fabric client mod and launcher that film a crowd from a real game client with nobody at the keyboard |
| `../loadgen-fleet.mjs` | Creates and deletes the VMs (OVH) and keeps the state file that the teardown needs |
| `../cpp/` | `ck-mc-load`, the load client (`CK-MC-LOAD.md` there) |
| `sky-2026-11-24/` | The scripts of the run before this one, kept for the record |

Everything about one mesh lives in a run folder outside git (`--run`): `mesh.json` (the plan), `cluster.json` (what
every server loads), `hosts.txt` and `generators.txt`, `allow.json`, `settings.json`, the payload. Secrets come from a
file you name (`--secrets-env`: `CROWDY_API_ORIGIN`, `CROWDY_APP_ID`, `CROWDY_ORG_TOKEN`, `CROWDY_ID_PEPPER`,
`CROWDY_CLUSTER_SECRET`); none are written into git or printed.

## The run, step by step

```
# 1. Machines. Names that tier-power does not match, so they are deleted by this state file and nothing else.
LOADGEN_PREFIX=mc-mesh-g  LOADGEN_STATE=$RUN/../game.json    node minecraft/loadgen-fleet.mjs up 101 c3-8
LOADGEN_PREFIX=mc-mesh-lg LOADGEN_STATE=$RUN/../loadgen.json node minecraft/loadgen-fleet.mjs up 12 b3-16

# 2. The plan: 10 x 10 cells of 12 chunks and a default server for the land around them. A cell edge at chunk 125
#    puts a line at block x = 2000, where the mod's scenarios expect one; --name-cell names that pair west and east.
python3 minecraft/mesh/mesh.py hosts --run $RUN --game-state $RUN/../game.json --loadgen-state $RUN/../loadgen.json
python3 minecraft/mesh/mesh.py plan  --run $RUN --cols 10 --rows 10 --cell 12 --origin-chunk-x 65 --origin-chunk-z -54 \
        --hosts $RUN/hosts.txt --name-cell 4,4=west --name-cell 5,4=east

# 3. The payload and the servers. A plain `up` copies the payload and keeps the world; --restart-only keeps the files
#    too (a changed flag, not a changed jar); --fresh starts from no world.
python3 minecraft/mesh/mesh.py payload --run $RUN --mod-jar <crowdycraft-server.jar> --fabric-api <jar> --fabric-launch <jar> \
        --bots 3200 --secrets-env <file> --country-database <GeoLite2-Country.mmdb> --xmx 3G
python3 minecraft/mesh/mesh.py up --run $RUN
python3 minecraft/mesh/mesh.py pregen --run $RUN          # once: every server makes its own chunks before anyone flies over them

# 4. The load client on the generators, then the bots. --wave is per generator: 12 generators x 5 every 5 s is
#    12 logins a second, which CK's dev tier took; 12 x 60 every 5 s is 144 a second, which it did not.
python3 minecraft/mesh/mesh.py gen-install --run $RUN
python3 minecraft/mesh/mesh.py bots --run $RUN --count 3012 --wave 5 --wave-seconds 5 --roam --start

# 5. Watching.
python3 minecraft/mesh/mesh.py watch  --run $RUN            # every server's ticks a second and whole-tick times, the worst listed
python3 minecraft/mesh/mesh.py botctl --run $RUN --sum stats  # the bots: in play, closed, rejoins, crossings

# 6. Teardown. Then check the provider's instance list has none of the names left.
LOADGEN_PREFIX=mc-mesh-g  LOADGEN_STATE=$RUN/../game.json    node minecraft/loadgen-fleet.mjs down --confirm-benjamin-teardown
LOADGEN_PREFIX=mc-mesh-lg LOADGEN_STATE=$RUN/../loadgen.json node minecraft/loadgen-fleet.mjs down --confirm-benjamin-teardown
```

`mesh.py up` also does what a fresh Ubuntu VM needs before it can hold a game server steady: Java, needrestart in
list-only mode and the apt timers off (unattended upgrades restarted the game service three times in four minutes on
one VM, which looks exactly like a server dropping its players), and one iptables chain that lets only the run's own
machines reach the game and link ports. `mesh.py allow --run $RUN <address>` adds one more (a camera's, a tester's)
and keeps it across later `up`s.

## What the numbers mean

`watch` reads the server mod's meter (`-Dcrowdedkingdoms.server.meter=true`, which `payload` sets): ticks a second and
the whole tick's time, from the start of a server tick to the end of the mod's end-of-tick work. The game's own tick
time ends before Fabric's end-of-tick handlers, so it leaves out most of what the mod does; do not use it.

`botctl --sum stats` adds up the generators. `play` counts clients in the play state; `closed` are bots the servers
refused at login (they are not retried); `rejoins` counts bots that lost their connection and joined again;
`crossings` counts changes of the server a bot is played on.

## Scenarios on a live mesh

```
python3 minecraft/mesh/scenario.py --run $RUN --tests <mod>/tools/cluster-tests --scenario blocks \
        --west s0204 --east s0205 --names Sd --client-script <mod>/tools/vanilla-client.sh
```

A scenario's stage is at a z of its own, so the pair is the one whose row holds it: on the 10 x 10 plan above, rows
1 to 4 of columns 4 and 5 (`s0104`/`s0105` to `west`/`east`). Stages north of the grid need the pair to own land
there too (a second range for `west` and `east` in `cluster.json`). What to know before trusting a result:

- **Fresh names** (`--names Sd`: Tester1 plays as Sd1). A new player starts in the chunks of the server they first
  join, which is where every scenario expects its players. Test players' records persist in CK, and scenarios end by
  sending them to 0, 0, the default server's land on a mesh; the runner sends them to the west server's middle
  instead. The names must be on the whitelist (`payload --players`).
- **The scripts' own clients** (`client Tester1 25599`) connect to the server's own port 25599 or 25600. Run a
  forwarder for each on both servers of the pair (`socat TCP-LISTEN:25599,bind=127.0.0.1,fork,reuseaddr
  TCP:<west address>:25565`, and 25600 to the east server).
- **One at a time.** Scenarios share the test players' clients on the machine that runs them, and a server plays its
  step file in order. A scenario that does not finish is aborted (the mod's script takes an `abort` line); the
  cleanup at its end goes with it, so put back any cluster-wide rule it changed (`spawn_mobs`, `mob_griefing`,
  the time).
- **Steps a mesh cannot meet** are dropped by name (`--drop '^\?relays'`: "exactly one relay on this server").
- **Scenarios that restart a server** (`rules`) are refused. `sleep` needs half the world's players in bed.
- **Do not change the load while a scenario plays.** A crowd called to one corner, or the bots' speed raised, made
  scenarios fail that pass otherwise.

## Filming

`meshcam/build.sh` compiles the camera mod with javac against the 26.3 client jar (the game ships unobfuscated);
`meshcam/client.sh <name> <server address>` starts the stock client under Fabric with it. The mod writes the game's
own frames, so it works with the Mac's screen locked, when nothing outside the game can capture the window or send
it a key. It plays a script from `<game folder>/meshcam/control.txt` (`MeshCam.java` lists the steps) and writes the
game's frames a second to `stats.csv` every second.

```
printf 'f3 on\nrec flyover 50\nfly 2260 214 0 -90 6 48\n' > <game folder>/meshcam/control.txt
ffmpeg -framerate 20 -i <game folder>/meshcam/flyover/%05d.png -c:v libx264 -pix_fmt yuv420p out.mp4
```

Frames are written once a game tick, so the movie is 20 frames a second whatever the game's frame rate; the frame
rate is in `stats.csv` and on the F3 overlay in the frames.

## Limits found

- About 30 to 45 wandering players a server (c3-8) keeps every server at 20 ticks a second. **A crowd called to a
  corner where four servers meet does not hold**: with 72 to 111 players on each of the four, two fell to 7 and 11
  ticks a second, their players timed out and joined again, and the burst of sign-ins slowed CK for everyone.
- CK's dev tier took 12 sign-ins a second. Its service that runs hubs needed two settings changed to carry 3,000
  players' hubs (the handoff has them).
- The bots fly above the clouds and cannot be hurt (`loadAccounts` in the server's config): the run measures servers
  with mob spawning on, not players fighting mobs.
