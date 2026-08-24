# cks-loadtest

An open-source UDP load tester for **Crowded Kingdoms** game servers (Buddy).
It simulates N game clients that walk around the world and exchange actor
updates, using the same public sign-in, token-minting, and server-assignment
flow a real native game client uses.

- Lightweight C++20: plain UDP sockets and a few worker threads drive
  thousands of simulated clients from a single host.
- Runs natively on Ubuntu or anywhere Docker runs — no special host setup.
- Provisions its own player accounts deterministically and idempotently from
  one email + password.

> **AUTHENTICATION IS PART OF THE MEASUREMENT, and this harness does it in-band.**
> bcrypt at 10 rounds costs 0.4–1.4s per sign-in, so ten thousand bots
> authenticating at run start burns API CPU in exactly the first seconds where
> the numbers matter. Provisioning runs before the ramp (`provisionAll`, then
> the ramp), which keeps it out of the steady state — but it is inside the
> process you are timing, and **the harness does not currently report whether
> any session was minted MID-RUN**. Until it does, a run whose latency tail
> looks wrong in the first minute should be suspected of measuring bcrypt.
> Field note 59: provision out of band, cache the session, and have the harness
> say `0 sessions minted during the run` rather than leaving it to be inferred.

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

> **ONE API, ONE ORIGIN — and "galaxy" is not what this is.** The management and
> game GraphQL surfaces are two surfaces of the unified CK API, which runs on
> PostgreSQL + Citus. Galaxy is a
> different product and is not the CK data plane; this note said "galaxy
> environments" for months and there is no such thing to point at. There is also
> no split deployment left: `cks-management-api` is not a running service.
>
> Point `LT_MANAGEMENT_API_URL` at that one origin and everything else follows —
> `mintAppToken` still reveals `gameApiUrl` (the app's OWN datacenter, which is
> where its shards live), and `serverWithLeastClients` still returns the Buddy
> fleet. App ids are 64-bit snowflakes (e.g. `73877390897664`).
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
4. **Traffic.** Each client opens one UDP socket, walks a random 2D path
   around the origin, and sends signed `ACTOR_UPDATE_REQUEST_2` messages at
   the configured rate. Inbound notifications, bundles, and errors are parsed
   and counted; the server epoch in notification tails yields a one-way
   latency estimate.
5. **Lifecycle.** App tokens are rotated before expiry (`refreshAppToken` +
   re-assign), `COMMAND_RECONNECT` triggers reassignment to another Buddy,
   and the run fails fast if traffic goes out but nothing ever comes back.

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
./build/cks-loadtest \
  --email you@studio.com \
  --password 'your-password' \
  --management-api-url "$MANAGEMENT_API_URL" \
  --app-id 42 \
  --clients 100 --threads 4 --update-hz 10 --duration-sec 300
```

No hostname appears in this repository, deliberately — see
[.env.example](.env.example) for how to derive one. In short, from the wrapper
checkout it is `tier_public_client_origin <tier>`, which looks the **published**
client origin up in the hostname table. Its neighbour `tier_client_origin`
derives the tier's internal **fleet** origin instead; both resolve and both
serve, so choosing the wrong one produces a run that passes while measuring a
host no customer uses.

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

## Key options

| Option / env | Default | Meaning |
|---|---|---|
| `--email` / `LT_EMAIL` | — | Base account email (required) |
| `--password` / `LT_PASSWORD` | — | Password for base + derived accounts (min 8 chars) |
| `--management-api-url` / `LT_MANAGEMENT_API_URL` | — | Management API base URL (required) |
| `--app-id` / `LT_APP_ID` | — | App to load test (required) |
| `--clients` / `LT_CLIENTS` | 10 | Simulated clients |
| `--threads` / `LT_THREADS` | 1 | Worker threads |
| `--update-hz` / `LT_UPDATE_HZ` | 10 | Actor updates per second per client |
| `--walk-speed` / `LT_WALK_SPEED` | 150 | Walk speed (Unreal units/s) |
| `--spawn-radius-chunks` / `LT_SPAWN_RADIUS_CHUNKS` | 8 | Spawn/bounce radius around origin |
| `--ramp-batch-size` / `LT_RAMP_BATCH_SIZE` | 10 | Clients activated per ramp batch |
| `--ramp-interval-ms` / `LT_RAMP_INTERVAL_MS` | 1000 | Delay between ramp batches |
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
  lags, the load generator host is saturated — add threads or hosts.
- **notif/s** measures replication fan-out: with all clients co-located it
  approaches `clients^2 x update-hz` (bounded by the server's interest
  management and your `--distance`).
- **lat~** is a one-way estimate from the server's epoch-millis stamp vs the
  local clock; it includes clock skew between the hosts, so watch its *trend*
  under load rather than its absolute value.
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

  The `UNEXPLAINED` line is deliberately not folded into the expected total, and
  on current tiers it is usually non-zero on a longer run. Measured with 100
  clients at 10 Hz, same roster, same app:

  | run length | expected | unexplained |
  |---|---|---|
  | 60 s | 100 | **0** |
  | 120 s (dev) | 100 | 37 |
  | 120 s (test) | 100 | 34 |

  So they are not a startup artefact and not tier-specific: **they begin
  somewhere between 60 s and 120 s into a run**, at roughly a third of clients,
  on both tiers. That bracket is the useful part — a run at 90 s and one at
  150 s would narrow it in two attempts. The shape is consistent with a
  server-side permission window or session entry being re-validated on a timer,
  but nothing here has established that, which is exactly why the harness labels
  the count unexplained instead of widening its window until the number looks
  clean.
- Exit code 3 means the RX health check tripped: traffic was sent but nothing
  was received (bad address, UDP blocked, or sessions never installed).

## Scope

The tool exercises the public GraphQL APIs and the public UDP wire protocol —
exactly what your game clients use — so its results reflect real player
traffic. Entitlement management stays with you (see Prerequisites).

## Repo layout

```
src/
  main.cpp            wiring: provision -> ramp -> simulate -> report
  Config.*            CLI + env + config-file settings
  GraphQLClient.*     minimal libcurl GraphQL POST client
  Provisioner.*       login/register, mintAppToken, serverWithLeastClients
  Wire.hpp            long-spatial wire format build/parse
  Hmac.hpp            HMAC-SHA256 sign/verify (OpenSSL)
  SimClient.hpp       per-client walk simulation + message template
  Worker.*            worker threads: epoll RX + tick TX
  Stats.*             counters, latency histogram, console/CSV reporter
tests/wire_test.cpp   wire vectors cross-checked against the reference impl
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
