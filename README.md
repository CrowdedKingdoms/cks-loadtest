# cks-loadtest

An open-source UDP load tester for **Crowded Kingdoms** game servers (Buddy).
It simulates N game clients that walk around the world and exchange actor
updates, using the same public sign-in, token-minting, and server-assignment
flow a real native game client uses.

- Lightweight C++20: plain UDP sockets and a few worker threads drive
  thousands of simulated clients from a single host. A fleet of generators
  is a row of the same binary plus `cks-loadtest-ctl`.
- Runs natively on Ubuntu or anywhere Docker runs — no special host setup.
- Provisions its own player accounts deterministically and idempotently from
  one email + password.

> **AUTHENTICATION IS PART OF THE MEASUREMENT, and this harness does it in-band.**
> bcrypt at 10 rounds costs 0.4–1.4s per sign-in, so ten thousand bots
> authenticating at run start burns API CPU in exactly the first seconds where
> the numbers matter. Provisioning runs before the ramp (`provisionAll`, then
> the ramp), which keeps it out of the steady state — but it is inside the
> process you are timing. Prefer a pre-minted roster so the timed window prints
> `0 session(s) minted DURING the run`. A run whose latency tail looks wrong in
> the first minute, with a non-zero minted-during-run count, is measuring bcrypt.

## How it works

```
Management API (GraphQL)         Game API (GraphQL)            Buddy (UDP)
  login / register  ──►  mintAppToken(appId)  ──►  serverWithLeastClients
  per derived account      app token + gameTokenId     ip4 + clientPort
                                                       (installs UDP session)
                                                            │
                                              wait ~1.5s, then send
                                              ACTOR_UPDATE_REQUEST_2 at
                                              LT_UPDATE_HZ, HMAC-signed
                                              with the app token
```

> **ONE API, ONE ORIGIN.** The management and game GraphQL surfaces are two
> surfaces of the unified CK API. Point `LT_MANAGEMENT_API_URL` at that origin
> and everything else follows — `mintAppToken` still reveals `gameApiUrl` (the
> app's own datacenter, which is where its shards live), and
> `serverWithLeastClients` still returns the Buddy fleet. App ids are 64-bit
> snowflakes, unique per deployment; resolve yours rather than copying a number.
>
> The variable keeps its `MANAGEMENT` name for compatibility; it is the unified
> origin, not a second host.

1. **Identity.** You supply one email + password (`LT_EMAIL`, `LT_PASSWORD`).
   The tool derives one account per simulated client with plus-addressing:
   `alice@studio.com` becomes `alice+lt-0000@studio.com`,
   `alice+lt-0001@studio.com`, … (pattern configurable via
   `LT_EMAIL_PATTERN`). Each account is logged in, or registered on the first
   run — deterministic and idempotent across runs.
2. **App token.** Each account mints an app-scoped token for `LT_APP_ID`
   (`mintAppToken`), which also reveals the app's Game API URL.
3. **Server assignment.** Each client calls `serverWithLeastClients` on the
   Game API, which returns a Buddy address **and installs the client's UDP
   session** on it server-side.
4. **Traffic.** Each client opens one UDP socket, moves through the world
   (a random 2D walk around the origin, or a 3D drift inside a cube of chunks
   -- see *Pose profiles* below), and sends signed `ACTOR_UPDATE_REQUEST_2`
   messages at the configured rate. Inbound notifications, bundles, and errors
   are parsed and counted; the server epoch in notification tails yields a
   one-way latency estimate.
5. **Lifecycle.** App tokens are rotated before expiry (`refreshAppToken`; the
   client KEEPS its Buddy and the refreshed token is installed on first
   contact, as a real client's is), `COMMAND_RECONNECT` triggers reassignment
   to another Buddy, and the run fails fast if traffic goes out but nothing
   ever comes back.

## Pose profiles: a load test you can see in the game

The platform relays the actor-state payload opaquely, so the servers accept any
bytes -- but the game you point this at renders only the layout it speaks, and a
load test whose players are invisible in the game is measuring a population no
player would ever experience. Two profiles ship:

| `LT_POSE_FORMAT` | payload | positions | up axis | chunk |
|---|---|---|---|---|
| `ue5` (default) | 88-byte float64 state v2 (`version`, position, rotation, velocity, crouch, attachments) | Unreal units, local to the chunk | Z | 1600 uu |
| `bwf` | 48-byte float32 pose, the layout Blocks With Friends' own client encodes (`x y z yaw pitch vx vy vz flags heldBlockId _ updatedAt _`; see `Wire.hpp`, namespace `bwfpose`) | world blocks, absolute | Y | 16 blocks |

Against Blocks With Friends use `LT_POSE_FORMAT=bwf LT_VOLUME_CHUNKS=8`: the
fleet fills an 8 x 8 x 8 chunk cube standing on chunk y 0 (BWF's terrain is
chunk layers 0-2, so most bots drift in the sky above spawn), and a player at
spawn sees them as avatars with dots spread across the minimap. Sent with the
default `ue5` profile, BWF decodes the bytes as x ~ 0, y = 0 (below bedrock) and
a scattered z: an invisible population whose minimap dots form a straight line.
Every ladder up to 2026-09-05 was run that way; its fan-out numbers came from a
17 x 17 x 1 plane, not the cube, and are not comparable with a `bwf` run.

Visual confirmation is part of a ladder: on each steady rung open the game at
spawn and screenshot the view and the minimap into the run directory. Avatars
visible and moving, dots distributed, no straight line.

## Prerequisites

- Your app must be reachable through a CK deployment. That is **one origin**
  serving both the management and game GraphQL surfaces, not two hosts.
- **Entitlements are your job.** The load tester is tier-agnostic: it does not
  inspect or manage app tiers, entitlements, or load limits. If a derived
  account is not entitled to the app, `mintAppToken` returns `FORBIDDEN` and
  the run aborts with the account's email. Grant the derived accounts access
  ahead of time (they follow `LT_EMAIL_PATTERN`, so their emails are
  predictable), or configure the app so accounts are auto-entitled. Two
  things to check:
  - the accounts have (or auto-receive) an *active grant* on the app, and
  - the granted tier carries the runtime permissions (at minimum `access`) —
    a permission-less tier mints tokens fine but Buddy rejects every spatial
    message with `UNAUTHORIZED`.

  [scripts/grant-access.sh](scripts/grant-access.sh) is an owner-side helper
  that logs in each derived account and grants a chosen tier via
  `grantAppAccess` (run `cks-loadtest` once first so the accounts exist).
- **Email note.** Registration sends one confirmation email per new account.
  With the default plus-addressing pattern they all land in the `LT_EMAIL`
  inbox on the first run. Confirmation is *not* required for the load test to
  run.
- **The organization owning the app must be funded, or exempt from billing.**
  This is a precondition, not a step the harness performs, and it is worth
  stating plainly because an unfunded org does not fail cleanly — usage is
  metered and gated, so a run against one measures a tier refusing work rather
  than a tier doing it, and the numbers look like poor capacity. Fund the org
  through whatever path your deployment provides before the run.

  The harness deliberately holds no privileged path for this. Crediting a wallet
  is an operator-only mutation, so a harness that funded its own target would be
  unusable by anyone who is not the platform operator — including every tenant
  load testing their own app. Configure and fund out of band, then point this at
  it.

## Provisioning sessions out of band (recommended)

**Authentication is part of the measurement.** The API hashes passwords with
bcrypt at 10 rounds, which costs 0.4–1.4 s of server CPU per sign-in. A hundred
clients signing in at run start therefore spend API CPU in exactly the first
seconds where the numbers matter, and what you measure is partly your own login
storm.

Mint the sessions first, and the run performs no sign-in at all:

```bash
LT_MANAGEMENT_API_URL="$MANAGEMENT_API_URL" \
LT_EMAIL=bots@example.invalid \
LT_PASSWORD='...' \
LT_CLIENTS=100 \
LT_ROSTER_FILE=roster.json \
  scripts/provision-roster.sh

LT_ROSTER_FILE=roster.json LT_ROSTER_REQUIRED=1 ./build/cks-loadtest --clients 100
```

Sessions last 30 days, so one roster serves many runs. `provision-roster.sh`
uses only `login` and `register` — the public mutations any tenant can call —
and needs no operator or infrastructure access. For a large population prefer
`LT_PASSWORD_HMAC_SEED` over a single shared `LT_PASSWORD`: passwords are then
derived per account and recomputable without being stored.

**The point is the reported number, not the speed.** Every run prints its
sign-in tally, and the one that matters is the first figure:

```
sign-in: 0 session(s) minted DURING the run, 0 before the ramp, 100 reused from the roster
```

A harness that absorbed its sign-in cost would print nothing here and look
identical to one that had none. Four things are refused rather than tolerated,
because each would otherwise silently reintroduce the cost or authenticate as
the wrong population: a roster that cannot be read, one minted against a
different origin, one whose emails disagree with `LT_EMAIL_PATTERN`, and — under
`LT_ROSTER_REQUIRED=1` — one that covers only some of the clients.

In-band sign-in remains fully supported and is the default with no roster
configured. It is the simplest thing that works, and for small runs the cost is
not worth managing.

## Build

### Native (Ubuntu 24.04)

```bash
sudo apt-get install build-essential cmake libcurl4-openssl-dev libssl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build          # wire-format + HMAC unit tests
bash scripts/check-content-policy.sh   # source + build/ artefacts
```

### Docker

```bash
docker build -t cks-loadtest .
```

## Run

All options can be given as CLI flags, `LT_*` environment variables, or a
`KEY=VALUE` file via `--config` (precedence: CLI > env > file). See
[.env.example](.env.example) for the full annotated list.

```bash
export LT_PASSWORD='your-password'   # prefer env over --password: argv is visible in ps(1)
./build/cks-loadtest \
  --email you@studio.com \
  --management-api-url "$MANAGEMENT_API_URL" \
  --app-id "$APP_ID" \
  --clients 100 --threads 4 --update-hz 10 --duration-sec 300
```

No hostname is checked in to this repository, deliberately. Set
`LT_MANAGEMENT_API_URL` to the GraphQL origin your game clients already use —
the same host they dial for sign-in. A second, "internal" origin that also
answers is the wrong one to measure: the run will pass while exercising a host
players never reach.

With Docker:

```bash
docker run --rm --env-file loadtest.env cks-loadtest --clients 100
```

While running, a stats line prints every `LT_STATS_INTERVAL_SEC`:

```
[stats] clients=100/100 tx=1000 pps 192.4 KB/s | rx=8940 dps 1571.2 KB/s notif=52344/s | lat~12.3ms | errs=0 reconnects=0 hmac_fail=0 send_fail=0
```

and a final summary (latency histogram, error-code breakdown) prints on exit.
Add `--csv-out stats.csv` for a machine-readable per-interval log.

## Progressive fleet

One process can stay up for the whole test: load some clients, measure, write
a rung summary, load more, never restart. Each VM owns a **global index
range** so two generators never share accounts (Buddy sessions are per token;
colliding emails steal sessions).

Install recipe for VM `i` of `N`, capacity `C` each:

```bash
export LT_INDEX_BASE=$((i * C)) LT_INDEX_LIMIT=$C LT_CLIENTS=0
export LT_INDEX_WIDTH=8
export LT_CONTROL_BIND="$PRIVATE_IP:9109"   # the private address ITSELF, not 0.0.0.0
export LT_CONTROL_TOKEN='...'          # required off-loopback; there is no TLS
export LT_STATS_DIR=/var/lib/cks-loadtest
export LT_ROSTER_FILE=roster.json LT_ROSTER_REQUIRED=1
./cks-loadtest
```

**Bind the private address, not `0.0.0.0`.** A cloud VM usually has a public
interface too, and `0.0.0.0` puts a token-gated, TLS-free control port on it —
one leaked token then adds clients to your fleet from anywhere. Naming the
private address is one string and cannot be got wrong by a firewall edit.

With a roster covering every client (`LT_ROSTER_FILE` + `LT_ROSTER_REQUIRED=1`)
no password is required and **none should be given**: the generators then hold
session tokens for their own slice and no credential that could mint more.

**Stop the machine's package manager from restarting the generator.** The point
of this mode is a process that lives for the whole test, which makes it the
first thing an unattended upgrade will interrupt. On Ubuntu, a run was cut at
176 s of a 180 s window by `apt-daily-upgrade` stopping and starting the
service, and the rung was lost:

```bash
sudo systemctl disable --now apt-daily.timer apt-daily-upgrade.timer
sudo systemctl mask unattended-upgrades.service apt-daily-upgrade.service apt-daily.service
```

`Restart=no` in a unit file does not help. It governs what the supervisor does
when the process exits by itself; a stop requested by *another* unit is not that,
so the setting never applies. Keep it anyway — a crash should stay visible rather
than silently restarting empty mid-rung — but do not mistake it for protection.

**Sync the generators' clocks to the same source as the servers**, or read
latency per host. See *Interpreting results* below: the one-way estimate is
offset by each generator's own clock error, and merging several hosts' samples
produces a fleet figure that is a mixture of their offsets.

Mint the whole population **once** (indices `0 .. N*C-1`), copy the same
roster to every VM:

```bash
LT_INDEX_BASE=0 LT_INDEX_WIDTH=8 LT_CLIENTS=$((N * C)) \
  scripts/provision-roster.sh
```

The orchestrating agent talks HTTP (`Authorization: Bearer $LT_CONTROL_TOKEN`)
or, easier, runs `cks-loadtest-ctl` against a hosts file (one `http://host:9109`
per line):

```bash
cks-loadtest-ctl --hosts hosts.txt status
cks-loadtest-ctl --hosts hosts.txt add --count 200
cks-loadtest-ctl --hosts hosts.txt wait --active-delta 200 --timeout-sec 120
# wait returns when each host has provisioned AND ramped the new clients
cks-loadtest-ctl --hosts hosts.txt rung-open --id r3
cks-loadtest-ctl --hosts hosts.txt wait --stable-sec 30
cks-loadtest-ctl --hosts hosts.txt rung-close --id r3 --out r3.fleet.json
# ... add again, never shutting the generators down ...
```

`rung-close` writes per-host JSON (and `{LT_STATS_DIR}/rung-<id>.json` on each
generator) plus one **fleet** summary. Merge rule: **sum** counts and rates,
**max** of max latency, **merge histograms then read p50/p95/p99**. Averages
of averages are refused. The fleet window carries the **earliest** host's
`open_epoch_sec` and the **longest** `duration_sec`, so it bounds the interval
every host was inside — which is what lets a rung be read back out of a
monitoring system over exactly its own window instead of a guess.

**Ramp before `rung-open`, as above.** The window then contains steady state
only, which is why a healthy rung shows *zero* first-contact `UNAUTHORIZED` in
its window while the lifetime count stays at roughly one per client. Both are
worth reading; either alone misleads.

**`aggregate` merges hosts within ONE rung.** It exists to re-merge per-host
blobs offline — the files under `{LT_STATS_DIR}` — not to summarise a ladder.
Handed several rungs it sums them, which is meaningless: five rungs of 50, 100,
200, 300 and 400 clients merge into a confident *1050 clients* labelled with the
first rung's id. It refuses that now (it reports the `rung_id` disagreement and
exits non-zero), but a ladder summary is a table of rungs and this tool does not
build one.

Control routes (all except `GET /health` require the bearer token):

| Method | Role |
|---|---|
| `GET /health` | liveness |
| `GET /v1/status` | instance id, index base/used/limit, active, busy, current rung |
| `GET /v1/stats` | lifetime + open-window counters + last-interval rates |
| `POST /v1/clients/add` | `{"count": N}` next unused indices (202; poll `status.busy`) |
| `POST /v1/rung/open` | `{"id": "r3"}` mark a stats window (does not add clients) |
| `POST /v1/rung/close` | persist the window summary |
| `POST /v1/shutdown` | graceful stop |

Put the control port on a private network. Binding anything other than
loopback without `LT_CONTROL_TOKEN` is a refusal at startup.

## Key options

| Option / env | Default | Meaning |
|---|---|---|
| `--email` / `LT_EMAIL` | — | Base account email (required) |
| `--password` / `LT_PASSWORD` | — | Password for base + derived accounts (min 8 chars). Prefer `LT_PASSWORD`: `--password` is visible in `ps(1)`. |
| `--management-api-url` / `LT_MANAGEMENT_API_URL` | — | CK GraphQL origin (required) |
| `--app-id` / `LT_APP_ID` | — | App to load test (required; a per-deployment snowflake, no default) |
| `--clients` / `LT_CLIENTS` | 10 | Clients provisioned at start (0 = wait for HTTP add) |
| `--index-base` / `LT_INDEX_BASE` | 0 | First global client index this process owns |
| `--index-limit` / `LT_INDEX_LIMIT` | = clients | Max clients this process will ever hold |
| `--index-width` / `LT_INDEX_WIDTH` | 4 | Zero-pad width for `{index}` (use 8 for a large fleet) |
| `--instance-id` / `LT_INSTANCE_ID` | hostname | Id stamped on every stats blob |
| `--control-bind` / `LT_CONTROL_BIND` | `127.0.0.1:9109` | HTTP control bind (`off` disables) |
| `--control-token` / `LT_CONTROL_TOKEN` | — | Bearer token; required off-loopback |
| `--stats-dir` / `LT_STATS_DIR` | — | Rung JSON + interval JSONL directory |
| `--threads` / `LT_THREADS` | 1 | Worker threads |
| `--update-hz` / `LT_UPDATE_HZ` | 10 | Actor updates per second per client |
| `--walk-speed` / `LT_WALK_SPEED` | 150 (`ue5`) / 4 (`bwf`) | Movement speed in the pose's position units per second |
| `--spawn-radius-chunks` / `LT_SPAWN_RADIUS_CHUNKS` | 8 | Spawn/bounce radius around origin (2D walk only) |
| `--pose-format` / `LT_POSE_FORMAT` | `ue5` | Actor-state payload the clients write: `ue5` (88-byte float64 state v2, chunk-local Unreal units, Z up) or `bwf` (48-byte float32 pose Blocks With Friends decodes: world blocks, Y up). See *Pose profiles*. |
| `--chunk-size-units` / `LT_CHUNK_SIZE_UNITS` | 1600 (`ue5`) / 16 (`bwf`) | Edge of one chunk in the pose's position units |
| `--volume-chunks` / `LT_VOLUME_CHUNKS` | 0 | 0 = the 2D walk; N = an N x N x N chunk cube centred on the origin chunk horizontally, filled uniformly by global client index, every client drifting in 3D and bouncing off the faces. 8 is the 512-chunk geometry that exercises per-ring decay. |
| `--volume-base-up` / `LT_VOLUME_BASE_UP` | 0 | Lowest vertical chunk of the cube |
| `--ramp-batch-size` / `LT_RAMP_BATCH_SIZE` | 10 | Clients activated per ramp batch |
| `--ramp-interval-ms` / `LT_RAMP_INTERVAL_MS` | 1000 | Delay between ramp batches |
| `--rx-silent-reassign-sec` / `LT_RX_SILENT_REASSIGN_SEC` | 0 | Re-assign a client that has received nothing at all for this many seconds on its current assignment (a Buddy that restarted or dropped the session answers nothing; no other trigger sees it). 0 = off; a lone client in an empty chunk legitimately hears nothing, so set it on fleet runs (30 on the ladder). Counted as `rx_silent_reassigns`. |
| `--provision-concurrency` / `LT_PROVISION_CONCURRENCY` | 4 | Parallel GraphQL provisioning |
| `--duration-sec` / `LT_DURATION_SEC` | 0 | Run time (0 = until Ctrl-C) |
| `--csv-out` / `LT_CSV_OUT` | — | Per-interval CSV stats file |
| `--email-pattern` / `LT_EMAIL_PATTERN` | `{local}+lt-{index}@{domain}` | Derived email pattern |
| `--game-api-url` / `LT_GAME_API_URL` | from mint | Game API override |
| `--verify-server-hmac` / `LT_VERIFY_SERVER_HMAC` | off | Verify signed server notifications |
| `--tls-insecure` / `LT_TLS_INSECURE` | off | Skip TLS verification (dev only) |
| `--duration-sec 0` + `SIGINT`/`SIGTERM` | — | Graceful shutdown with final summary |

## Interpreting results

- **tx pps** should equal `clients x update-hz` once the ramp completes. If it
  lags, the load generator host is saturated — add threads, or add hosts and
  partition `LT_INDEX_BASE` (see Progressive fleet).
- **notif/s** measures replication fan-out: with all clients co-located it
  approaches `clients^2 x update-hz` (bounded by the server's interest
  management and your `--distance`).
- **lat~** is a one-way estimate from the server's epoch-millis stamp vs the
  local clock; it includes clock skew between the hosts, so watch its *trend*
  under load rather than its absolute value.

  **On a fleet, do not merge it into one number.** Skew is per generator and it
  is a constant offset on every sample that host takes, so a merged percentile
  is a mixture of the generators' clock errors weighted by how many clients each
  holds — a figure that moves when you add a host and not when the servers
  change. Measured on two VMs of the same image running the same rung against
  the same servers, with their windows opened in the same second:

  | | generator A | generator B |
  |---|---|---|
  | p50 | 8.0 ms | **0.8 ms** |
  | p95 | 14.9 ms | 6.3 ms |
  | p99 | 18.7 ms | 7.0 ms |

  An order of magnitude at p50, from clocks alone. Report per-host percentiles
  side by side, or sync every generator to the same time source the servers use.
  Trends survive skew — p95 rose on *both* hosts as fan-out grew — but levels do
  not, and neither do comparisons between hosts.
- **errs / error codes** in the final summary map to the wire protocol error
  codes (e.g. `TOKEN_EXPIRED`, `UNAUTHORIZED`). Occasional `TOKEN_EXPIRED`
  around the ~30 min mark is normal — clients rotate tokens and resume.
- **One `UNAUTHORIZED` per client at startup is expected and is not a fault.**
  Buddy loads a session's permission window lazily and the client's own first
  spatial packet is what triggers the load. The summary separates those from the
  rest, by whether they arrived within 2 s of an assignment:

  ```
  code 7 (UNAUTHORIZED): 137
    of which EXPECTED: 100 on first contact with a Buddy, within 2s ...
    UNEXPLAINED: 37 UNAUTHORIZED arrived LATER than that. ...
  ```

  **Waiting longer does not avoid the startup one**, which is worth knowing
  before you go looking for a race: measured on a live tier,
  `LT_SESSION_SETTLE_MS` of 1500 and of 8000 produce the identical count,
  because the trigger is the packet and not the clock. The window re-arms per
  *assignment*, so a reassignment mid-run legitimately adds one.

  The count of expected refusals should be close to your client count — 100
  clients, 100 refusals. Substantially more, or a steady stream, usually means
  the accounts' granted tier carries no runtime permissions: tokens mint fine
  and every spatial message is refused.

- **A second, later batch of `UNAUTHORIZED` is the SAME lazy load, and it is
  geometry rather than a timer.** These used to be reported as `UNEXPLAINED`,
  which read as a platform fault and is not one. The server caches grid
  permissions as a **box of radius 8 chunks centred where the last lookup ran**,
  so a client that walks far enough leaves the box, and the packet that crosses
  the edge is refused while the re-query is in flight — exactly as at first
  contact, just later. The summary counts them separately:

  ```
  code 7 (UNAUTHORIZED): 144
    of which EXPECTED: 100 on first contact with a Buddy, within 2s ...
    of which EXPECTED: 44 on crossing out of the server's cached ...
  ```

  **The timing is a distance divided by a speed, not a timeout**, which is worth
  understanding because the first guess is always a TTL. A chunk is 1600 uu and
  the box radius is 8 chunks, so the edge is 12800 uu away: at the default
  `LT_WALK_SPEED=150` that is **85 s** if a client walks along an axis and
  **121 s** if it walks at 45°, since the box is square. Nothing can arrive
  before 85 s, and nothing did.

  Measured on dev, 100 clients at 10 Hz, same roster and app, varying one input
  at a time:

  | run | walk speed | spawn radius | first late refusal | late total |
  |---|---|---|---|---|
  | baseline, 180 s | 150 | 8 | **t+101 s** | 44 |
  | 120 s | **0** | 8 | never | **0** |
  | 120 s | **600** (4×) | 8 | **t+21 s** (4× earlier) | 79 |
  | 150 s | 150 | **0** | never | **0** |

  Those last two rows are the ones that settle it. Onset scales as **1/speed**,
  and a run at full walk speed with `LT_SPAWN_RADIUS_CHUNKS=0` produces none at
  all over 150 s — so no clock-driven mechanism is involved, because a token
  TTL, an idle disconnect or a released session slot would all still fire in a
  run where the clients are moving as fast as ever.

  **Why the spawn radius matters, and why this is the harness's own doing.** The
  walk is confined to `LT_SPAWN_RADIUS_CHUNKS` of the **world origin** (it
  reverses direction at the edge), while the server's box is centred on each
  client's **own spawn chunk**. A client that spawns at the origin has a box
  containing the whole confinement area and can never leave it; one that spawns
  at the edge is guaranteed to walk out of its box on the way to the far side.
  That is why roughly a third to a half of clients see one and the rest never
  do, and why setting the spawn radius to 0 removes them entirely. The
  confinement is deliberate — it keeps clients close enough to replicate to each
  other, which is the point of the load — so the harness reports the refusals
  rather than tuning them away.

  `LT_PERMISSION_WINDOW_RADIUS_CHUNKS` (default 8) is the radius the harness
  *assumes* when classifying. It is never sent on the wire, and the server's
  value is not discoverable from a client, so it is a declared assumption: set
  it too small and ordinary refusals get filed as window reloads, set it too
  large and they land in `UNEXPLAINED`.

- **`UNEXPLAINED` is what remains, and it is the line worth chasing.** A refusal
  that arrives while a client is neither newly assigned nor outside the modelled
  box is not accounted for by either lazy load. Check that the accounts' granted
  tier carries runtime permissions, and treat a steady stream as wedged sessions.
  The bucket is kept deliberately reachable rather than widened until the number
  reads zero — verified by running with the radius set absurdly high, which
  correctly moves every window-reload refusal into it.

  **Expect a residual of a few percent here even on a healthy tier**, so read the
  trend rather than demanding zero: a measured baseline run was 100 first-contact,
  51 window-reload and **9 unexplained out of 160**. The harness models a single
  box while the server keeps several, and the two disagree about the centre by a
  chunk or so under flight time, so a handful of genuine crossings get filed as
  internal. That gap was deliberately not tuned away — anything that closed it
  would be widening a model of the server's cache until the number looked clean,
  and a real permission fault would then land inside the widened grace.
- Exit code 3 means the RX health check tripped: traffic was sent but nothing
  was received (bad address, UDP blocked, or sessions never installed).

## Scope

The tool exercises the public GraphQL APIs and the public UDP wire protocol —
exactly what your game clients use — so its results reflect real player
traffic. Entitlement management stays with you (see Prerequisites).

## Repo layout

```
src/
  main.cpp            wiring: harness + HTTP control + shutdown
  Config.*            CLI + env + config-file settings
  GraphQLClient.*     minimal libcurl GraphQL POST client
  Provisioner.*       login/register, mintAppToken, serverWithLeastClients
  Wire.hpp            long-spatial wire format build/parse
  Hmac.hpp            HMAC-SHA256 sign/verify (OpenSSL)
  SimClient.hpp       per-client walk simulation + message template
  Worker.*            worker threads: epoll RX + tick TX
  Stats.*             counters, windowed snapshots, fleet merge, reporter
  Harness.*           long-lived generator: hot-add, rungs, workers
  ControlServer.*     HTTP JSON control port
  ctl.cpp             cks-loadtest-ctl (status/add/wait/rung/aggregate)
tests/                wire, config, stats merge, hot-add, HTTP control
```

## Versioning

**There is none, and there is deliberately no changelog.** This repository ships no
versioned artifact: `project(cks-loadtest CXX)` declares no `VERSION`, there are no
git tags, there is no release workflow, and nothing publishes a binary anywhere. A
run is built from a checkout, so **the commit is the version** — record the SHA
beside a measurement, because that is the only thing that identifies what produced
it.

That is worth stating rather than leaving to inference: an absent changelog in a
repo that ships a versioned artifact is a gap, and an auditor sweeping for one
should be able to tell the two cases apart without re-deriving this. If a released
binary or image ever starts being published from here, that is the moment this
section becomes wrong and a `CHANGELOG.md` becomes owed.

## License

Apache-2.0. See [LICENSE](LICENSE).
