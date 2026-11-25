# ck-fleet-load: a load client for fleet sessions

`ck-fleet-load` simulates an organization's game servers. Each simulated server opens
one fleet session, the servers carry a population of players between them, and the
players wander and cross from one server to the next. The client measures what happens
at each crossing and prints a JSON summary.

**Fleet sessions are in development.** They run on the dev tier only, and the wire
formats on this page can still change.

## A fleet session carries many players in one realtime session

A game server used to open one realtime session for each of its players. With a fleet
session the server opens one session for itself and *carries* its players in it:

- The server opens the session with an org token (`openFleetSession`) and receives a
  fleet token, which is the session's signing key.
- A player signs in once, when they join the game (`mintManagedPlayerToken`).
- The server tells the realtime servers that it carries the player (`FLEET_CARRY`).
  From then on it sends that player's messages itself: the message's tail names the
  player's `gameTokenId`, and the HMAC is keyed with the fleet token.
- When the player moves to another of the organization's servers, that server sends
  `FLEET_CARRY` for the player. The server the player left receives `FLEET_RELEASED`
  with reason 1. The crossing makes no call to the Application API.

## The run proves that a crossing costs no API call

A passing run shows five things about the population it carried:

1. Every player ended the run carried by the server whose region they stood in.
2. No crossing lost a player.
3. The Application API received no call because of a crossing. The summary lists
   every call by kind and by cause, so you can read the count yourself.
4. No server was sent state that one of its own carried players sent, while the
   `FLEET_OWN_STATE` capability was off.
5. The client itself kept up. The summary reports how late the client sent each
   update, so a slow generator is visible.

The summary also reports the latencies the proof asks for: how long a carry takes, how
long a crossing takes, how soon the old server is released, and how long an update
takes to reach a neighbouring server.

## One run opens the sessions, signs the players in, and lets them walk

1. **Sessions.** The client calls `openFleetSession` once for each simulated server and
   connects one UDP socket to the realtime server each answer names. Each session sends
   its capabilities first, then a heartbeat every 2 seconds.
2. **Players.** The client calls `mintManagedPlayerToken` once for each player, at
   `--signin-rate` calls a second. A player's `externalId` is `fleet-load-<n>`. An answer
   of `PLATFORM_BUSY` or `RATE_LIMITED` is retried after the wait the answer names
   (`extensions.retryAfterMs`), or after 1.5 seconds when it names none.
3. **The world.** The servers form a grid of regions: 10 by 10 for 100 servers. Each
   region is `--region-chunks` chunks on a side. A player is placed at a random point
   and belongs to the server whose region the point is in.
4. **Carrying.** That server sends `FLEET_CARRY` for the player and repeats it every
   250 ms, each time with a fresh time and proof, until `FLEET_CARRY_RESULT` arrives.
   After 5 seconds with no result it gives up, counts the carry, and starts again one
   second later.
5. **Walking.** A carried player sends an actor update `--update-hz` times a second and
   walks in a straight line that turns every 4 to 12 seconds. The walking speed is set
   so that the whole population crosses `--cross-rate` times a second, and a controller
   corrects the speed from the crossings it counts.
6. **Crossing.** When a player steps over a border, the old server stops sending for
   the player at once. The new server sends `FLEET_CARRY` and takes over the player's
   updates when the result says carried. The old server expects `FLEET_RELEASED` with
   reason 1.
7. **The measured window.** `--duration-s` starts when every player has joined. A
   player's app token lasts 30 minutes, so in a longer run the client refreshes each
   token before it expires (`refreshAppToken`, with the player's own token as the
   bearer) and carries the player again with the new token.
8. **The end.** The players stop walking and the client waits up to `--drain-s`
   seconds for carries and releases still on their way. It then sends `FLEET_RELEASE`
   for each player, calls `closeFleetSession` for each session, and prints the summary.
   The client does the same when you interrupt it with Ctrl-C.

A session that a realtime server tells to move (`COMMAND_RECONNECT` or
`COMMAND_SESSION_RELEASED`, each verified with the fleet token) calls
`refreshFleetSession` with the server it was on, connects to the server the answer
names, and carries its players again.

## Before you run it against dev

- **An org token in the environment.** Put the token in `CK_ORG_TOKEN`. The client
  reads the token from the environment only, sends it as `Authorization: Bearer`, and
  never prints it. The token needs the scopes `players:mint` and `fleet:session`.
- **Fleet sessions switched on for the organization.** A Crowded Kingdoms operator
  switches fleet sessions on for an organization, together with org-managed players.
  An admin of the organization must have accepted the current Org-Managed Players
  Addendum.
- **An app of the organization's own.** Pass its id as `--app`. Use an app that holds
  nothing else, because the client fills the app's world around chunk (0, 0) with
  walking players.
- **Room in the organization's limits.** One session carries at most 256 players and
  an organization holds at most 512 sessions, unless an operator set other numbers.
  The default run needs 100 sessions and about 30 players in each.
- **Access to the tier.** Dev is open only to accounts and organizations that have
  been granted access to it. Ask an operator if the organization has none.
- **A clock that is right.** The realtime servers refuse a `FLEET_CARRY` whose time is
  more than 30 seconds from theirs. Keep the host on NTP.
- **UDP to the realtime servers.** The host must be able to send UDP to the address
  and port that `openFleetSession` answers.

## Build and run it

```bash
sudo apt-get install build-essential cmake libcurl4-openssl-dev libssl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build
```

Check the build before you point it at a tier. Neither command uses the network:

```bash
./build/ck-fleet-load --self-test
./build/ck-fleet-load --dry-run
```

`--self-test` checks the wire formats against known answers and runs three short
populations against a stand-in inside the process. `--dry-run` runs the full default
population against the same stand-in for `--dry-run-s` seconds (5 by default), builds
every message a live run would send, and prints the rates it reached.

The run the proof asks for is 100 sessions carrying 3,000 players for 30 minutes on dev:

```bash
export CK_ORG_TOKEN='...'   # the org token; never put it on the command line
./build/ck-fleet-load \
  --api "$CK_API_URL" \
  --app "$APP_ID" \
  --servers 100 --players 3000 --duration-s 1800 \
  --update-hz 5 --cross-rate 75 --signin-rate 20 \
  --roster-file dev-fleet-roster.json \
  --rx-silent-s 30 \
  --summary-file fleet-summary.json
```

`CK_API_URL` is the GraphQL endpoint of the dev tier's Application API, the one the
organization's game servers call (it ends in `/graphql`). This repository checks in no
tier hostname for a run, so the command reads the endpoint from your shell.

Signing 3,000 players in at 20 a second takes 150 seconds before the 30 minutes start.
`--roster-file` saves the player tokens (mode 0600), and a second run inside their 30
minutes starts without signing anyone in. Roster files hold live tokens; `.gitignore`
covers names that end in `-roster.json`.

The progress line goes to stderr every 5 seconds, and the summary goes to stdout:

```
[fleet] t=45s sessions=100 players=3000/3000 | upd=14998/s notif=35016/s | cross=76.2/s (target 75.0) speed=0.353 | carry p50=0.1ms p99=0.3ms | released ok=3454 missing=0 | refused=0 own_state=0 | api busy=0 limited=0 crossing=0 | lag p99=1.12ms
```

## The options and their defaults

| Option | Default | Meaning |
|---|---|---|
| `--api` | required | The Application API's GraphQL endpoint |
| `--app` | required | The app id |
| `--servers` | 100 | Simulated game servers, one fleet session each |
| `--players` | 3000 | Players carried in total |
| `--duration-s` | 1800 | Measured seconds, counted from when every player has joined. 0 runs until Ctrl-C |
| `--update-hz` | 5 | Actor updates a second for each carried player |
| `--cross-rate` | 75 | Target crossings a second over all players. 0 keeps each player inside their region |
| `--signin-rate` | 20 | `mintManagedPlayerToken` calls a second |
| `--region-chunks` | 16 | Side of a region in chunks |
| `--seed` | 1 | Seed for where players start and how they walk |
| `--own-state` | off | Set the `FLEET_OWN_STATE` capability bit, to measure the difference |
| `--roster-file` | none | Save the minted player tokens here and reuse them in the next run |
| `--dry-run` | off | No network: a stand-in inside the process answers the API and the realtime wire |
| `--self-test` | off | Run the built-in checks and exit |
| `--distance` | 8 | Replication distance of an actor update, in chunks |
| `--latency-sample` | 100 | Players whose updates carry a send time |
| `--heartbeat-s` | 2 | Seconds between a session's own heartbeats. 0 sends none |
| `--session-settle-ms` | 1500 | Wait after the API authorizes a session before its first datagram |
| `--rx-silent-s` | 0 | Move a session that has received nothing for this long. 0 never does |
| `--threads` | 4 | Realtime worker threads |
| `--api-threads` | 8 | Application API threads |
| `--token-refresh-lead-s` | 120 | Refresh a player's app token this many seconds before it expires |
| `--drain-s` | 6 | Seconds to wait at the end for carries and releases on their way |
| `--summary-file` | none | Also write the summary JSON to this file |

`--help` lists the rest.

With the default replication distance of 8 chunks, a 16-chunk region puts every player
within 8 chunks of a border. A player that close to a border is in range of players
standing just across it, so each server keeps receiving updates from its neighbours'
players. A larger region lowers that traffic, and a smaller one raises it.

## A run passes on four conditions

The exit code is 0 when all four hold:

1. Every carry that was attempted ended carried, or was retried until it was carried.
   The summary shows this as `players.not_carried_at_end` and
   `players.refused_for_good`, which must both be 0.
2. No crossing lost a player: `crossings.lost` and `crossings.unaccounted` are 0.
3. No API call was made because of a crossing: `api.calls_caused_by_crossing` is 0.
4. No own state came back: `own_state.received` is 0, unless you ran with `--own-state`.

| Exit code | Meaning |
|---|---|
| 0 | The run passed |
| 1 | A pass condition failed. `fail_reasons` in the summary says which |
| 2 | An option was wrong, or the setup failed (a session could not be opened, a player could not be signed in, the roster was refused) |
| 130 | The run was interrupted before its duration ended. The sessions were closed and the summary covers what ran. A run started with `--duration-s 0` ends on Ctrl-C and is judged like any other |

### How the client knows that no API call came from a crossing

Three things hold the count at zero, and the summary lets you check each one.

- **The threads are separate.** The threads that handle crossings own the sockets and
  hold no handle to the Application API. They ask the API threads for the two things
  they can need: a session that was told to move, and a player whose sign-in ended.
- **Every call has a cause.** `api.calls_by_cause` splits the calls into setup,
  teardown, session moves, session keeps, token refreshes, and sign-ins again. A
  request raised while a crossing is being handled is counted under `crossing`, and
  that count fails the run.
- **The window is counted on its own.** `api.calls_in_measured_window_by_kind` lists the
  calls made while the players crossed. In a 30-minute run that list holds about 3,000
  `refreshAppToken` calls, one for each player's token, and nothing else unless a
  session moved or a sign-in ended.

## The summary, number by number

Latencies are objects with `samples`, `p50_ms`, `p95_ms`, `p99_ms`, `max_ms`, and
`mean_ms`. Percentiles come from a histogram whose buckets are each a quarter wider
than the last, so a percentile is exact to within its bucket; `max_ms` is exact.

### `pass`, `exit_code`, `fail_reasons`, `interrupted`, `mode`

`mode` is `live` or `dry-run`. `fail_reasons` holds one sentence for each pass
condition that failed.

### `config` and `timing`

`config` repeats the options the run used, with the grid it built (`grid_cols`,
`grid_rows`). `timing.setup_s` is the time to open the sessions and sign the players
in, `timing.measured_s` is the measured window, and `timing.total_s` includes the end.

### `sessions`

| Field | Meaning |
|---|---|
| `opened`, `closed`, `close_failures` | `openFleetSession` and `closeFleetSession` answers |
| `max_players` | Players one session may carry, as `openFleetSession` answered |
| `moves`, `moves_by_trigger` | Sessions that left their realtime server, and why: a reconnect command, a session-released command, send errors, a run of token errors, or silence |
| `moves_completed`, `move_failures` | Moves that ended connected to the server `refreshFleetSession` named, and failed attempts |
| `keep_refreshes` | `refreshFleetSession` calls made because a session's token neared its 24-hour expiry |
| `reconnect_commands`, `session_released_by_reason` | Verified commands received |

### `players`

| Field | Meaning |
|---|---|
| `total`, `joined` | Players asked for, and players handed to the world |
| `signed_in` | Players signed in with `mintManagedPlayerToken` during setup |
| `from_roster` | Players whose token came from the roster file |
| `platform_busy`, `rate_limited` | Sign-in answers of each kind; each one was retried |
| `sign_in_failures` | Sign-ins that failed for another reason. The first one stops the run |
| `token_refreshes`, `token_refresh_failures` | `refreshAppToken` answers |
| `signed_in_again`, `sign_in_again_failures` | Players signed in again after their sign-in ended |
| `carried_at_end`, `not_carried_at_end`, `refused_for_good` | Where the players stood when the run ended |

### `carries`

| Field | Meaning |
|---|---|
| `attempts`, `attempts_by_cause` | Carries started, and why: a join, a crossing, a refreshed token, a sign-in again, a carry again after a release, or a session move |
| `sent` | `FLEET_CARRY` datagrams, including the 250 ms repeats |
| `carried`, `carried_by_cause` | Results with status 0 |
| `refused` | Results with each refusal status: 1 no live token, 2 another organization's or app's player, 3 the proof or its time, 4 the session is full |
| `gave_up_after_5s`, `retried` | Carries with no result in 5 seconds, and carries started again after that |
| `abandoned_player_moved_on` | Carries dropped because the player crossed again before the result |
| `results_duplicate`, `results_late` | Results that answered a repeat, and results for a player the session no longer held |
| `unresolved_at_end` | Players without a carry at the end. Must be 0 |
| `latency` | From the first `FLEET_CARRY` sent to the `FLEET_CARRY_RESULT` received |

### `crossings`

| Field | Meaning |
|---|---|
| `attempted` | Steps over a border |
| `completed` | Crossings whose new server received status 0 |
| `superseded` | Crossings whose player crossed again before the new server's result |
| `lost` | Crossings still open at the end: the new server never carried the player. Must be 0 |
| `unaccounted` | `attempted` minus the three above. Must be 0 |
| `latency` | From the step over the border to the new server's result |
| `target_per_s`, `achieved_per_s` | The crossing rate asked for, and the rate measured over the window |
| `start_speed_chunks_per_s`, `speed_chunks_per_s` | The walking speed the run started with, and the speed the controller ended on |
| `api_calls` | API calls made because of a crossing. Must be 0 |

### `released`

`by_reason` counts every `FLEET_RELEASED` by its reason. `old_session` covers the
release each crossing expects:

| Field | Meaning |
|---|---|
| `expected` | Crossings that left a server which carried the player |
| `received` | Of those, the old server received `FLEET_RELEASED` with reason 1 |
| `delay_after_new_result` | How long after the new server's result the release arrived. A release that arrived first counts as 0 |
| `before_new_result` | Releases that arrived before the new server's result |
| `never_arrived_within_5s` | Releases still missing 5 seconds after the new server's result |
| `no_new_result_to_pair`, `pending_at_end` | Expectations that ended with no result to time against, and ones still waiting at the end |

The remaining fields count releases the run did not expect, and what the client did:

| Field | Meaning |
|---|---|
| `reason_1_for_a_held_player` | Another session's carry replaced one the world says is current. The server carried the player again |
| `reason_1_after_a_late_carry` | A carry that the player had already left behind landed late, and its release followed |
| `reason_2_for_a_held_player` | The player's sign-in ended. The player was signed in again and carried again |
| `reason_5_for_a_held_player` | The player was released as idle. The server carried the player again |
| `carried_again`, `sign_in_again_requests`, `signed_in_again` | What the client did about them |
| `for_a_player_not_held` | Releases naming a player the session did not hold |

### `refused`

`errors_by_code` counts every error message the realtime servers sent, by code.
`unauthorized` is the count for `UNAUTHORIZED`, and `player_messages_refused` is the
part of it that answered a message sent for a player. The client sorts those refusals
by the cause it can see:

| Field | Meaning |
|---|---|
| `unauthorized_first_contact` | Within 2 seconds of the carry. The first message makes the realtime server load the player's permissions |
| `unauthorized_window_reload` | The player walked more than 8 chunks from where their permissions were loaded |
| `unauthorized_after_handoff` | The player had left the session; its last update met the new server's carry on the way |
| `unauthorized_unexplained` | None of the above. A steady run of these for one player makes the server carry the player again (`carried_again_after_unauthorized`) |
| `unauthorized_session_messages` | A session's own message (a carry, a release, its capabilities, a heartbeat) was refused |

The first two causes follow the model the per-player harness uses. The numbers 2
seconds and 8 chunks are assumptions about the realtime servers, so read a large
`unauthorized_window_reload` as a question to ask and treat it as unproven.

### `traffic`

| Field | Meaning |
|---|---|
| `updates_sent`, `updates_per_s`, `updates_per_s_expected` | Actor updates sent, the rate over the window, and players times `--update-hz` |
| `capabilities_sent`, `heartbeats_sent`, `releases_sent` | The session's own messages |
| `send_errors` | Sends the socket refused |
| `received_by_type` | Messages received, by type and name |
| `notifications_received`, `notifications_per_s` | Spatial notifications received |
| `deliveries` | `FLEET_DELIVERY` frames, the players they listed, the notification types inside, and any player listed that the session did not carry |
| `signed_bundles`, `plain_bundles` | Datagrams of each bundle kind |
| `hmac_failures`, `malformed` | Datagrams that failed verification or parsing. Both should be 0 |
| `unsigned_notifications`, `fleet_frames_outside_signed_bundle`, `fleet_frames_long_form` | How frames arrived, for comparison with what the realtime servers intend |

### `replication`

`sample_players` players have a send time written into each update. `latency` is the
time from one session sending such an update to another session receiving it, on this
process's clock. `marked_updates_received` counts every update of this run that a
session received.

### `own_state`

`received` counts updates that came back to the session that sent them. It must be 0
with the capability off. With `--own-state` it is the traffic the capability adds.

### `bytes`

`tx` and `rx` are application bytes in UDP datagrams, in total. `per_session` lists
each session's server, bytes, datagrams, and the players it held at the end.

### `api`

| Field | Meaning |
|---|---|
| `calls`, `calls_by_kind`, `calls_by_cause` | Every HTTP request to the Application API, including retries |
| `calls_in_measured_window_by_kind` | The calls made while the players crossed |
| `platform_busy_by_kind`, `rate_limited_by_kind`, `failures_by_kind` | Answers that were retried, and calls that failed |
| `calls_caused_by_crossing` | Must be 0 |

### `generator`

`send_lag` is how late the client sent each update against its schedule.
`updates_skipped` counts updates the client dropped because it had fallen a whole
interval behind. If the lag's 99th percentile nears the update interval (200 ms at
5 Hz), the host is the limit: add `--threads` or use a larger host before you read the
other numbers.

## The messages on the wire

Client to server, each in the long-form spatial layout with chunk (0, 0, 0), distance
0, and no actor UUID. The tail carries the session's own `gameTokenId` and an HMAC
keyed with the fleet token.

| Message | Type | App payload |
|---|---|---|
| `CLIENT_CAPABILITIES` | 29 | 4-byte flags: bit 0 signed bundles, bit 3 `FLEET_OWN_STATE` |
| `CLIENT_ACTOR_HEARTBEAT` | 26 | none |
| `FLEET_CARRY` | 49 | 8-byte player `gameTokenId`, 8-byte epoch milliseconds, 32-byte proof |
| `FLEET_RELEASE` | 50 | 8-byte player `gameTokenId` |

The proof is `HMAC-SHA256(key = the player's 64-octet token, message = "ck-fleet-carry"
+ fleet gameTokenId + player gameTokenId + epoch milliseconds)`, each number 8 bytes,
little endian.

An actor update for a carried player is the update the player's own session would
send. Its tail names the player's `gameTokenId`, and its HMAC is keyed with the fleet
token.

Server to client:

| Message | Type | Bytes after the type |
|---|---|---|
| `FLEET_CARRY_RESULT` | 51 | 8-byte player `gameTokenId`, 8-byte user id, 1-byte status |
| `FLEET_RELEASED` | 52 | 8-byte player `gameTokenId`, 1-byte reason |
| `FLEET_DELIVERY` | 53 | 1-byte count, count 8-byte player `gameTokenId`s, one notification |

### What the client assumes where the contract is open

- **Frames 51, 52, and 53 are read in two framings.** The client expects the bytes in
  the table to follow the type byte directly, as a member of a signed bundle. It also
  reads the same bytes as the app payload of a long-form message, and the summary says
  which arrived (`fleet_frames_long_form`).
- **A frame outside a signed bundle is accepted and counted.** A session advertises
  signed bundles first, so its frames should arrive inside one, verified with the fleet
  token. A fleet frame that arrives another way is processed and counted in
  `fleet_frames_outside_signed_bundle`.
- **The session's heartbeat names no actor.** It is a `CLIENT_ACTOR_HEARTBEAT` with the
  session's own `gameTokenId`, sent every 2 seconds. Carried players send updates
  continuously and need none of their own. `--heartbeat-s 0` turns it off.
- **A session told to move always names its current server** in `refreshFleetSession`,
  and reconnects to whichever server the answer names.
- **The replication marker lives in the update's payload.** The client sends the
  88-byte actor state the per-player harness sends, which leaves rotation pitch and yaw
  at 0. The client writes the sending session and a send time into those two fields as
  whole numbers. The platform relays the payload as it is. Use an app of your own,
  because a game client that renders these actors would show the numbers as a rotation.

## The tests run without a network

```bash
ctest --test-dir build -R fleet --output-on-failure
```

| Test | Covers |
|---|---|
| `fleet_wire_test` | `FLEET_CARRY`, `FLEET_RELEASE`, the heartbeat, and a carried player's update against bytes computed outside this code; the proof against an HMAC built from SHA-256 by hand; parsing of the three server frames; the two move commands; the marker |
| `fleet_world_test` | The grid, neighbours, a crossing detected exactly once, the speed formula, and the cross-rate controller |
| `fleet_api_test` | Each GraphQL document and input, the retries, the counts by kind and cause, the roster file, and the org token's handling |
| `fleet_run_test` | The whole client against the stand-in: a clean run, sessions told to move, an ended sign-in, an idle release, lost datagrams, token refresh, own state, refusal statuses 2, 3, and 4, an interrupt, and roster reuse |
| `fleet_self_test`, `fleet_dry_run`, `fleet_needs_org_token` | The binary itself |

The stand-in (`src/fleet/FakeRealtime.*`) is written from the contract above. It checks
each datagram's HMAC and each carry's proof, keeps one carrier for a player, and sends
an update once to each session that carries a player in range. It is a test double and
says nothing about how the realtime servers are built. Only a live run on dev tests
the realtime servers themselves.
