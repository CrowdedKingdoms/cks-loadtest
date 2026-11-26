# ck-fleet-load: a load client for fleet sessions

`ck-fleet-load` simulates an organization's game servers. Each simulated server opens
one fleet session, the servers carry a population of players between them, and the
players wander and cross from one server to the next. The client measures what happens
at each crossing and prints one JSON summary.

**Fleet sessions are in development.** They run on the dev tier only. This page follows
the wire as the realtime servers v0.50.0 built it.

The client has three modes:

| Mode | What it does |
|---|---|
| The load run (default) | 100 sessions carry 3,000 players who cross about 75 times a second for 30 minutes |
| `--rules-check` | Two sessions carry six players and check each per-player rule on a carried player |
| `--delete-players` | Deletes the players the other two modes made |

## A fleet session carries many players in one realtime session

A game server used to open one realtime session for each of its players. With a fleet
session the server opens one session for itself and *carries* its players in it:

- The server opens the session with an org token (`openFleetSession`) and receives a
  fleet token, which is the session's signing key. The same org token refreshes and
  closes the session. Another org token of the same organization cannot.
- A player signs in once, when they join the game (`mintManagedPlayerToken`).
- The server tells the realtime servers that it carries the player (`FLEET_CARRY`).
  From then on it sends that player's messages itself: the message's tail names the
  player's `gameTokenId`, and the HMAC is keyed with the fleet token.
- Under its own `gameTokenId` the session sends four messages only: its capabilities
  (29), `FLEET_CARRY` (49), `FLEET_RELEASE` (50), and a keepalive (26) that registers no
  actor.
- When the player moves to another of the organization's servers, that server sends
  `FLEET_CARRY` for the player. The server the player left receives `FLEET_RELEASED`
  with reason 1. The crossing makes no call to the Application API.

## The load run proves that a crossing costs no API call

A passing run shows six things about the population it carried:

1. Every player ended the run carried by the server whose region they stood in.
2. No crossing lost a player.
3. The Application API received no call because of a crossing.
4. During the measured phase the API received no call at all, apart from refreshes
   that fell due (a player's app token, a session's 24 hours).
5. No server was sent state that one of its own carried players sent, while the
   `FLEET_OWN_STATE` capability was off.
6. Every communication arrived inside a `FLEET_DELIVERY`.

The summary also reports what the proof asks to see: carries by status, releases by
reason, refusals, HMAC failures, the latencies of a carry and a crossing, and the
bytes each session sent and received in each minute, for comparison with the bill.

## One run opens the sessions, signs the players in, and lets them walk

1. **Sessions.** The client calls `openFleetSession` once for each simulated server and
   connects one UDP socket to the realtime server each answer names. A session's first
   datagram is its capabilities, which bind the session to its address. It then sends a
   keepalive every 2 seconds.
2. **Players.** The client calls `mintManagedPlayerToken` once for each player, at
   `--signin-rate` calls a second. A player's `externalId` is `fleet-load-<n>`, so a
   later run signs the same players in again and creates none. An answer of
   `PLATFORM_BUSY` or `RATE_LIMITED` is retried after the wait the answer names
   (`extensions.retryAfterMs`), or after 1.5 seconds when it names none.
3. **The world.** The servers form a grid of regions: 10 by 10 for 100 servers. Each
   region is `--region-chunks` chunks on a side. A player is placed at a random point
   and belongs to the server whose region the point is in.
4. **Carrying.** That server sends `FLEET_CARRY` for the player. A realtime server can
   leave a `FLEET_CARRY` unanswered, and it drops carries past 64 a second for one
   session. So the client sends at most `--carry-rate` carries a second for a session
   (60 by default), and sends an unanswered carry again with a later time and a new
   proof: after 250 ms, then after twice as long each time, up to 2 seconds. After 5
   seconds with no result it gives up, counts the carry, and starts again one second
   later.
5. **Walking.** A carried player sends an actor update `--update-hz` times a second and
   walks in a straight line that turns every 4 to 12 seconds. The walking speed is set
   so that the whole population crosses `--cross-rate` times a second, and a controller
   corrects the speed from the crossings it counts.
6. **Crossing.** When a player steps over a border, the old server stops sending for
   the player at once. The new server sends `FLEET_CARRY` and takes over the player's
   updates when the result says carried. The old server expects `FLEET_RELEASED` with
   reason 1. The two servers exchange nothing else.
7. **The measured phase.** `--duration-s` starts when every player has joined. A
   player's app token lasts 30 minutes, so in a longer run the client refreshes each
   token before it expires (`refreshAppToken`, with the player's own token as the
   bearer) and carries the player again with the new token.
8. **The end.** The players stop walking and the client waits up to `--drain-s`
   seconds for carries and releases still on their way. It then sends `FLEET_RELEASE`
   for each player, calls `closeFleetSession` for each session, and prints the summary.
   The client does the same when you interrupt it with Ctrl-C.

**The other crossing plan.** With `--crossing-plan release-first` the old server sends
`FLEET_RELEASE` for the player before the new server carries them. That plan needs the
two servers to agree on the order, and it leaves a moment with no carrier. The default
plan needs neither, which is why it is the default.

**A session that comes back.** A realtime server can tell a session to move
(`COMMAND_RECONNECT`, 22 or 44, or `COMMAND_SESSION_RELEASED`, 28 or 45, with reason 3 or
4, each verified with the fleet token). The client then calls `refreshFleetSession`
with no current server, so the API names a new one. On the new server the session
sends its capabilities and a new `FLEET_CARRY` for every player, at the carry rate. A
session whose own messages are refused calls `refreshFleetSession` naming the server it
is on. When the answer names that same server the carries stand, and the session sends
its capabilities only.

## Before you run it against a tier

- **An org token in the environment.** Put the token in `CK_ORG_TOKEN`. The client
  reads the token from the environment only, sends it as `Authorization: Bearer`, and
  never prints it. The token needs the scopes `players:mint` and `fleet:session`, and
  `players:manage` for `--delete-players`. Use one token for a run: the token that
  opens a session is the only one that can refresh or close it.
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
  more than 30 seconds from theirs (status 3). Keep the host on NTP.
- **UDP to the realtime servers.** The host must be able to send UDP to the address
  and port that `openFleetSession` answers, and receive datagrams of up to 1,242 bytes.

## Build it and check it without a network

```bash
sudo apt-get install build-essential cmake libcurl4-openssl-dev libssl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build
```

```bash
./build/ck-fleet-load --self-test
./build/ck-fleet-load --dry-run
./build/ck-fleet-load --rules-check --dry-run
```

`--self-test` checks the wire formats against known answers and runs short populations
against a stand-in inside the process. `--dry-run` runs the full default population
against the same stand-in for `--dry-run-s` seconds (5 by default), builds every message
a live run would send, and prints the rates it reached.

## Run the 30-minute proof

```bash
export CK_ORG_TOKEN='...'   # the org token; never put it on the command line
./build/ck-fleet-load \
  --api "$CK_API_URL" \
  --app "$APP_ID" \
  --servers 100 --players 3000 --duration-s 1800 \
  --update-hz 5 --cross-rate 75 --signin-rate 20 \
  --roster-file fleet-roster.json \
  --rx-silent-s 30 \
  --summary-file fleet-summary.json
```

`CK_API_URL` is the GraphQL endpoint of the tier's Application API in the app's
datacenter, the one the organization's game servers call (it ends in `/graphql`). This
repository checks in no tier hostname for a run, so the command reads the endpoint from
your shell.

Signing 3,000 players in at 20 a second takes 150 seconds before the 30 minutes start.
That is the sign-in each player makes once, and it is outside the measured phase.
`--roster-file` saves the player tokens (mode 0600), and a second run inside their 30
minutes starts without signing anyone in. Roster files hold live tokens; `.gitignore`
covers names that end in `-roster.json`.

The progress line goes to stderr every 5 seconds, and the summary goes to stdout. This
line is from a dry run:

```
[fleet] t=45s sessions=100 players=3000/3000 | upd=14998/s notif=35016/s | cross=76.2/s (target 75.0) speed=0.353 | carry p50=0.1ms p99=0.3ms | released ok=3454 missing=0 r2=0 r5=0 | refused=0 own_state=0 | api busy=0 limited=0 crossing=0 | lag p99=1.12ms
```

### What a run leaves on the tier, and how to remove it

A run closes its sessions and releases its players before it exits, also after Ctrl-C.
It leaves these behind:

- **The players.** 3,000 managed players named `fleet-load-0` to `fleet-load-2999`
  stay in the organization, so the next run reuses them. Remove them with
  `--delete-players` (below).
- **Usage and billing records** for the bytes the run sent and received. They stay,
  as they do for any player.
- **Recorded inputs**, if the app has its input log on. They age out with the log's
  retention.
- **The roster file** on this host, if you passed `--roster-file`.
- **Sessions, if the process was killed.** A session that was neither closed nor
  refreshed ends by itself within 24 hours and counts toward the organization's 512
  until then. Revoking the org token ends its sessions at once.

The run makes no world edits, and its actors leave five seconds after their last
update.

```bash
./build/ck-fleet-load --delete-players \
  --api "$CK_API_URL" --app "$APP_ID" --org "$ORG_ID" --players 3000 \
  --roster-file fleet-roster.json
```

`--delete-players` calls `deleteManagedPlayer` for `fleet-load-0` to
`fleet-load-<players - 1>` and for the six players of the rule check, at
`--signin-rate` a second. `--org` is the organization's id, which the call needs. A
player that does not exist counts as `not_found` and is no failure. With
`--roster-file` the roster is removed too, because its tokens no longer work. A
deleted player's content is erased within minutes; signing the same `externalId` in
again creates a new player.

## Check the per-player rules on carried players

Everything the realtime servers decide by who a session's player is, they decide by
the carried player a message names. `--rules-check` checks that, one rule at a time:

```bash
./build/ck-fleet-load --rules-check \
  --api "$CK_API_URL" --app "$APP_ID" --rules-idle-timeout-s 60
```

Two sessions, X and Y, carry six players who stand in one chunk (chunk 5000, 5000,
away from the load run's world). The client signs the players in itself:

| Player (`externalId`) | Session | Role |
|---|---|---|
| `fleet-rules-a` | X | Blocks `b` |
| `fleet-rules-b` | Y | Blocked by `a` |
| `fleet-rules-c` | X | Stands beside `a` |
| `fleet-rules-d` | Y | Stands beside `b` |
| `fleet-rules-minor` | Y | 16 years old |
| `fleet-rules-idle` | X | Sends one update, then only heartbeats |

### What an operator sets up first

The client sets the block itself, with player `a`'s own app token (`blockPlayer`). The
rest needs someone with rights the org token does not have:

1. **The app's idle timeout, set to 60 seconds.** A member with `manage_apps` on the
   app runs `updateApp(appId: ..., input: { idleTimeoutSeconds: 60 })`. Pass the same
   number as `--rules-idle-timeout-s`. Set it back afterwards (`null` restores the
   platform's 15 minutes). `--rules-idle-timeout-s 0` skips the idle check.
2. **The app admits a 16-year-old.** Three things must hold, or the player's sign-in is
   refused:
   - the app's minimum age is 16 or lower (`minPlayerAge`; `null` is the platform's 13);
   - the app's safety log is on (`updateApp(..., input: { replayLoggingEnabled: true })`),
     which needs a funded organization wallet;
   - the organization has a safety moderator: a member with a role that grants
     `moderate_safety`.

   A sign-in refused for age (`AGE_REQUIREMENT_NOT_MET`) locks that player out for 24
   hours, so set the app up before the first run.
3. **Text and voice chat for the app's players.** The access tier the app gives its
   players must grant `use_text_chat` and `use_voice_chat`. Without them nothing is
   delivered and the block and voice checks fail.
4. **The same organization setup as the load run**: fleet sessions on, the addendum
   accepted, the org token's scopes.

If you prefer to make the block yourself, block `fleet-rules-b` as `fleet-rules-a`
before the run. The client's own `blockPlayer` call then changes nothing.

### What each check sends and expects

Each check prints `PASS` or `FAIL` and what it observed, on stderr, and the summary
lists the same under `checks`:

```
[rules] PASS  block: the blocked player's text: b's text reached session X in 1 FLEET_DELIVERY listing c (c must be listed; a, who blocked b, must not). On b's own session it listed b, d, minor
```

| Check | What is sent | What must come back |
|---|---|---|
| carry | `FLEET_CARRY` for each player | Status 0 for each |
| block: the blocked player's text | `b` sends a text | Session X receives it in a `FLEET_DELIVERY` that lists `c` and does not list `a` |
| block: the blocking player's text | `a` sends a text | Session Y receives it in a `FLEET_DELIVERY` that lists `d` and does not list `b` |
| block: state still passes | Actor updates | Session X still receives plain actor updates from session Y's players |
| communications arrive only in FLEET_DELIVERY | Everything above | No text or voice outside a `FLEET_DELIVERY`, and no frame whose HMAC fails |
| a player under 18's voice | The minor sends voice | `UNAUTHORIZED`, and no session receives it. An adult's voice from the same place is delivered, to show voice works there |
| another player's actor | `c` and `b` each send an update under `a`'s actor | `UNAUTHORIZED` for both |
| a player the session does not carry | Session X sends an update for `b` | `UNAUTHORIZED` |
| what a session may send under its own id | An actor update under the session's id; capabilities under a player's id | `UNAUTHORIZED`; `INVALID_REQUEST` |
| the largest FLEET_DELIVERY | `d` sends a 1,123-byte text | Session X receives it alone with its own HMAC, in datagrams of at most 1,242 bytes |
| the idle release | `idle` sends heartbeats under its own id and no update | `FLEET_RELEASED` reason 5 for `idle` after the timeout, and for nobody else |

The checks before the idle release take about 25 seconds. The idle release then
waits for the timeout, and for up to 45 seconds more before it gives up. Exit code 0
means every check printed `PASS`.

The rule check leaves its six players and the block between `a` and `b` on the tier.
`--delete-players` removes both.

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
| `--signin-rate` | 20 | `mintManagedPlayerToken` (or `deleteManagedPlayer`) calls a second |
| `--region-chunks` | 16 | Side of a region in chunks |
| `--seed` | 1 | Seed for where players start and how they walk |
| `--crossing-plan` | `carry` | `carry`: the new server carries and the old one is released. `release-first`: the old server sends `FLEET_RELEASE` first |
| `--carry-rate` | 60 | `FLEET_CARRY` a second a session, resends included. At most 64 |
| `--carry-resend-ms` | 250 | Send an unanswered `FLEET_CARRY` again after this long, then after twice as long each time, up to eight times as long |
| `--own-state` | off | Set the `FLEET_OWN_STATE` capability bit, to measure the difference |
| `--signed-bundles` | true | Advertise `BUNDLE_SIGNED`. With `--signed-bundles=false` every frame arrives with its own HMAC |
| `--bound-control` | true | Advertise `BOUND_CONTROL` with a nonce, so move commands are 44 and 45 |
| `--roster-file` | none | Save the minted player tokens here and reuse them in the next run |
| `--dry-run` | off | No network: a stand-in inside the process answers the API and the realtime wire |
| `--self-test` | off | Run the built-in checks and exit |
| `--rules-check` | off | Run the per-player rule checks and exit |
| `--rules-idle-timeout-s` | 60 | The app's idle timeout as the operator set it. 0 skips the idle check |
| `--delete-players` | off | Delete the players this client made and exit. Needs `--org` |
| `--org` | none | The organization's id |
| `--distance` | 8 | Replication distance of an actor update, in chunks |
| `--latency-sample` | 100 | Players whose updates carry a send time |
| `--heartbeat-s` | 2 | Seconds between a session's own keepalives. 0 sends none |
| `--session-settle-ms` | 1500 | Wait after the API authorizes a session before its first datagram |
| `--rx-silent-s` | 0 | Ask the API to authorize again a session that has received nothing for this long. 0 never does |
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

## A load run passes on seven conditions

The exit code is 0 when all seven hold:

1. Every carry that was attempted ended carried, or was retried until it was carried:
   `players.not_carried_at_end` and `players.refused_for_good` are 0.
2. No crossing lost a player: `crossings.lost` and `crossings.unaccounted` are 0.
3. No API call was made because of a crossing: `api.calls_caused_by_crossing` is 0.
4. No API call in the measured phase was anything but a refresh that fell due:
   `api.calls_in_measured_phase_other` is 0.
5. No session received an update it sent itself:
   `own_state.sent_by_the_receiving_session.total` is 0, unless you ran with
   `--own-state`. An update another session sent for a player the receiver carries
   now is counted apart and does not fail a run.
6. No communication arrived outside a `FLEET_DELIVERY`: `traffic.plain_communications`
   is 0.
7. No refusal was left unexplained: `refused.unauthorized_unexplained` and
   `refused.unauthorized_session_messages` are 0.

The other classes of refusal are expected and do not decide the exit code (the table
under `refused` below). `proof.nothing_refused` says whether the run saw any refusal.

| Exit code | Meaning |
|---|---|
| 0 | The run passed |
| 1 | A pass condition failed. `fail_reasons` in the summary says which |
| 2 | An option was wrong, or the setup failed (a session could not be opened, a player could not be signed in, the roster was refused) |
| 130 | The run was interrupted before its duration ended. The sessions were closed and the summary covers what ran. A run started with `--duration-s 0` ends on Ctrl-C and is judged like any other |

### How the client knows that no API call came from a crossing

- **The threads are separate.** The threads that handle crossings own the sockets and
  hold no handle to the Application API. They ask the API threads for the two things
  they can need: a session that has to be authorized again, and a player whose sign-in
  ended.
- **Every call has a cause.** `api.calls_by_cause` splits the calls into setup,
  teardown, session moves, session keeps, token refreshes, and sign-ins again. A
  request raised while a crossing is being handled is counted under `crossing`, and
  that count fails the run.
- **The measured phase is counted on its own.** `api.calls_in_measured_phase` is every
  call made while the players crossed. `api.calls_in_measured_phase_refreshes_due` is
  the part that refreshed a player's app token or kept a session's 24 hours. In a
  30-minute run that part is about 3,000 `refreshAppToken` calls, one for each
  player's token. `api.calls_in_measured_phase_other` is the rest and must be 0. A
  session that a realtime server told to move, and a player who had to sign in again,
  each make a call that lands there: the run then fails, and `fail_reasons` names the
  cause.

## The summary, number by number

Latencies are objects with `samples`, `p50_ms`, `p95_ms`, `p99_ms`, `max_ms`, and
`mean_ms`. Percentiles come from a histogram whose buckets are each a quarter wider
than the last, so a percentile is exact to within its bucket; `max_ms` is exact.

### `pass`, `exit_code`, `fail_reasons`, `interrupted`, `mode`

`mode` is `live` or `dry-run`. `fail_reasons` holds one sentence for each pass
condition that failed.

### `proof`

The headline numbers in one object:

| Field | Meaning |
|---|---|
| `sessions_opened` | Fleet sessions opened |
| `players_carried`, `players_carried_at_end` | The most players carried at once in the measured phase, and the number at the end |
| `carries_by_status` | `FLEET_CARRY_RESULT` by status: `0_carried`, `1_no_live_token`, `2_other_organization`, `3_proof_time_or_replay`, `4_session_full`, `other` |
| `releases_by_reason` | `FLEET_RELEASED` by reason: `1_carried_by_another_session`, `2_sign_in_ended`, `5_idle`, `other` |
| `crossings_attempted`, `crossings_completed`, `crossings_lost`, `crossings_per_s` | Crossings, and the rate achieved over the measured phase |
| `crossing_plan` | `carry` or `release-first` |
| `carry_retries` | `FLEET_CARRY` sent again, plus carries started again after giving up |
| `api_calls_in_measured_phase`, `api_calls_in_measured_phase_refreshes_due`, `api_calls_in_measured_phase_other`, `api_calls_caused_by_crossing` | The API calls made while the players crossed |
| `hmac_failures` | Datagrams, notifications, and fleet frames whose HMAC failed |
| `unauthorized_answers` | `UNAUTHORIZED` error messages received |
| `refused`, `nothing_refused` | Carries refused, `UNAUTHORIZED` answers, other errors, their total, and whether the total is 0 |
| `own_state_received`, `own_state_sent_by_the_receiving_session`, `plain_communications` | Updates that came back to the session that sent them (two names for one number), and communications outside a `FLEET_DELIVERY`. All must be 0 |
| `own_state_sent_by_another_session_before_the_handoff` | Updates a session received for a player it carries now, sent by the session that carried the player before. Expected |
| `datagrams_sent`, `datagrams_received`, `bytes_sent`, `bytes_received` | Totals over the run |

### `config` and `timing`

`config` repeats the options the run used, with the grid it built (`grid_cols`,
`grid_rows`). `timing.setup_s` is the time to open the sessions and sign the players
in, `timing.measured_s` is the measured phase, and `timing.total_s` includes the end.

### `sessions`

| Field | Meaning |
|---|---|
| `opened`, `closed`, `close_failures` | `openFleetSession` and `closeFleetSession` answers |
| `max_players` | Players one session may carry, as `openFleetSession` answered |
| `moves`, `moves_by_trigger` | Sessions that went back to the API, and why: a reconnect command, a session-released command, send errors, a run of errors that refuse the session, or silence |
| `moves_completed`, `move_failures` | Refreshes that ended connected, and failed attempts |
| `came_back_to_the_same_server` | The carries stood: capabilities only |
| `came_back_to_another_server` | Every player was carried again |
| `bound_control_acknowledged` | Acknowledgements (46) of a session's control nonce: one for each capabilities message a server accepted |
| `legacy_move_commands_dropped` | A 22 or 28 that arrived after the acknowledgement: a replay |
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
| `attempts`, `attempts_by_cause` | Carries started, and why: a join, a crossing, a refreshed token, a sign-in again, a carry again after a release, or a session that came back on another server |
| `sent` | `FLEET_CARRY` datagrams |
| `carried`, `carried_by_cause` | Results with status 0 |
| `by_status` | Every result by its status. 1: no such live token, or a token of another app. 2: another organization's player. 3: the proof or its time failed, or a replay. 4: the session carries as many as it may, or there is no place for it |
| `resent` | Carries sent again because no result came, each with a later time and a new proof |
| `waited_for_the_carry_rate` | Carries that waited their turn under `--carry-rate` |
| `gave_up_after_5s`, `started_again_after_giving_up` | Carries with no result in 5 seconds, and carries started again after that |
| `abandoned_player_moved_on` | A carry whose player crossed to another region before its result came, so the session stopped waiting for it: a join that had not been answered, or a carry made for a refreshed token |
| `results_duplicate` | A result that answered a carry the session had sent twice |
| `results_late` | The result of an abandoned carry, which arrived after the player had left the session; a status 0 among them means that carry landed, and its `FLEET_RELEASED` follows when the next session carries the player |
| `took_a_newer_token_before_sending` | A carry that waited its turn and went out with a token stored meanwhile |
| `retried_after_refresh` | Results with status 1 around a token refresh, after which the carry was sent again (see below). `..._at_once_with_the_newer_token` and `..._after_a_wait` split them |
| `status_1_not_explained_by_a_refresh` | Results with status 1 and no refresh near them. The player signs in again, and that API call fails the run |
| `token_carry_sent_after_the_refresh_answer` | How long after `refreshAppToken` answered the carry with the new token went out |
| `unresolved_at_end` | Players without a carry at the end. Must be 0 |
| `latency` | From the first `FLEET_CARRY` sent to the `FLEET_CARRY_RESULT` received |

#### What happens to a carry around a token refresh

A player's app token is refreshed before it expires (`--token-refresh-lead-s`). The
API answers with a new token and a new `gameTokenId`, and the session that carries the
player sends a new `FLEET_CARRY` with it at once.

- **The carry under the old token stands while the new one is unanswered.** The
  session goes on sending the player's updates under the old `gameTokenId` until the
  new carry's result arrives, and under the new one from then on. It sends no
  `FLEET_RELEASE` for the old one: the new carry replaces it.
- **The realtime server moves the player to the new id when it accepts the carry.**
  An update under the old id that is sent after that moment and before the result
  reaches the session is answered `UNAUTHORIZED`. Those are counted in
  `refused.unauthorized_during_token_refresh`.
- **Status 1 around a refresh is sent again.** A carry that names the token the
  refresh replaced is answered status 1 (no such live token). That happens to a
  crossing whose carry went out while `refreshAppToken` was being answered. The
  client sends the carry again at once with the new token, with a new time and proof.
  If the carry named the newest token, and that token was stored in the last
  5 seconds or a refresh is in flight, the client waits 200 ms and sends it again,
  up to 10 times. Both are counted in `carries.retried_after_refresh`.
- **Any other status 1 is a refusal.** The player signs in again, and the run fails
  on that API call.
- **A player can cross while the new carry is unanswered.** The session it leaves
  drops that carry (`abandoned_player_moved_on`). If the carry landed all the same,
  the `FLEET_RELEASED` the old session receives names the new `gameTokenId`. The
  client pairs it with the crossing by either id
  (`released.old_session.named_the_token_id_of_a_carry_left_unanswered`).

### `crossings`

| Field | Meaning |
|---|---|
| `attempted` | Steps over a border |
| `completed` | Crossings whose new server received status 0 |
| `superseded` | Crossings whose player crossed again before the new server's result |
| `lost` | Crossings still open at the end: the new server never carried the player. Must be 0 |
| `unaccounted` | `attempted` minus the three above. Must be 0 |
| `latency` | From the step over the border to the new server's result |
| `target_per_s`, `achieved_per_s` | The crossing rate asked for, and the rate measured over the measured phase |
| `start_speed_chunks_per_s`, `speed_chunks_per_s` | The walking speed the run started with, and the speed the controller ended on |
| `api_calls` | API calls made because of a crossing. Must be 0 |

### `released`

`by_reason` counts every `FLEET_RELEASED` by its reason, under the names above.
`old_session` covers the release each crossing expects:

| Field | Meaning |
|---|---|
| `expected` | Crossings that left a server which carried the player. 0 under the release-first plan |
| `received` | Of those, the old server received `FLEET_RELEASED` with reason 1 |
| `delay_after_new_result` | How long after the new server's result the release arrived. A release that arrived first counts as 0 |
| `before_new_result` | Releases that arrived before the new server's result |
| `named_the_token_id_of_a_carry_left_unanswered` | Releases that named the `gameTokenId` of a carry the player left behind unanswered (a refreshed token's). They are counted in `received` |
| `never_arrived_within_5s` | Releases still missing 5 seconds after the new server's result. The client does nothing more about one: the new server carries the player, and the old one stopped sending for the player when the player left. Each is listed under `events.released_never_arrived` |
| `no_new_result_to_pair`, `pending_at_end` | Expectations that ended with no result to time against, and ones still waiting at the end |

The remaining fields count releases the run did not expect, and what the client did:

| Field | Meaning |
|---|---|
| `reason_1_for_a_held_player` | Another session's carry replaced one the world says is current. The server carried the player again |
| `reason_1_after_a_late_carry` | The release for a carry the player had left behind: the abandoned carry landed after the player crossed (`carries.results_late`), and the next session's carry replaced it |
| `reason_1_after_the_sessions_own_release` | Under the release-first plan: a reason 1 for a player the session had already released |
| `reason_2_for_a_held_player` | The player's sign-in ended. The player was signed in again and carried again |
| `reason_5_for_a_held_player` | The player was released as idle. The server carried the player again |
| `carried_again`, `sign_in_again_requests`, `signed_in_again` | What the client did about them |
| `for_a_player_not_held` | Releases naming a player the session did not hold |

### `refused`

`errors_by_code` counts every error message the realtime servers sent, by code.
`unauthorized` is the count for `UNAUTHORIZED`, and `player_messages_refused` is the
part of it that answered a message sent for a player. An error names no player: it
echoes a one-byte sequence number that the whole session shares, and the client looks
up what it sent under that number in the last two seconds. For each message it sent,
the client remembers the type, the player, the token id it named, whether a carry
with a new token was unanswered, and whether a `refreshAppToken` was in flight.

Every `UNAUTHORIZED` falls into one class. The client tests them in this order:

| Field | The refused message | Fails a run |
|---|---|---|
| `unauthorized_session_messages` | Was one of the session's own (a carry, a release, its capabilities, a keepalive) | Yes |
| `unauthorized_during_token_refresh` | Named a token id that a refresh was replacing or had replaced. It was sent after the realtime server accepted the carry with the new token and before that carry's result reached the session | No |
| `unauthorized_after_handoff` | Was for a player who has left the session since. The player's last update there met the new session's carry on the way | No |
| `unauthorized_after_the_carry_ended` | Was sent before a `FLEET_RELEASED` for a player the session held, or before the session moved. The carry it relied on ended while the message was on the way | No |
| `unauthorized_first_contact` | Was sent within 2 seconds of the carry's result. The first message makes the realtime server load the player's permissions | No |
| `unauthorized_window_reload` | Was sent after the player walked more than 8 chunks from where their permissions were loaded | No |
| `unauthorized_unexplained` | None of the above: the session carried the player under that token id when it sent the message, and still does. The sequence number of an error that matches nothing sent in the last two seconds lands here too | Yes |

`unauthorized_during_token_refresh_before_the_new_carry_result` and
`..._after_the_new_carry_result` say whether the refusal reached the session before or
after the new carry's result. A steady run of unexplained refusals for one player
makes the session carry the player again (`carried_again_after_unauthorized`).

The first 200 refusals of each class are listed under `events`, each with the message
type, the player, how long after the player's last carry result on that session the
message was sent, whether a carry with a new token was unanswered, and whether a
token refresh was in flight.

The numbers 2 seconds and 8 chunks follow the model the per-player harness uses. They
are assumptions about the realtime servers, so read a large
`unauthorized_window_reload` as a question to ask and treat it as unproven.

### `traffic`

| Field | Meaning |
|---|---|
| `updates_sent`, `updates_per_s`, `updates_per_s_expected` | Actor updates sent, the rate over the measured phase, and players times `--update-hz` |
| `capabilities_sent`, `heartbeats_sent`, `releases_sent` | The session's own messages |
| `send_errors` | Sends the socket refused |
| `received_by_type` | Messages received, by type and name |
| `notifications_received`, `notifications_per_s` | Spatial notifications received |
| `plain_state_notifications` | State received plain: once a session, however many of its players may receive it |
| `deliveries` | `FLEET_DELIVERY` frames, the players they listed, the notification types inside, and any player listed that the session did not carry |
| `plain_communications` | Texts, voice, video, single-actor messages, or channel messages that arrived outside a `FLEET_DELIVERY`. Must be 0: such a message does not say who may receive it |
| `signed_bundles`, `plain_bundles` | Datagrams of each bundle kind |
| `by_type_and_arrival` | For each message type: how many arrived as members of a signed bundle, as members of a plain bundle, and alone in a datagram. For those outside a signed bundle: how many carried an HMAC of their own that verified, one that failed, or none. An error message (3) has no HMAC in any form, and 22, 28, 44, 45, and 46 always arrive alone with one |
| `plain_bundle_members`, `plain_bundle_members_by_type` | What the plain bundles held. Each plain bundle is listed under `events.plain_bundles` with its members and each member's own HMAC |
| `fleet_frames_in_signed_bundles`, `fleet_frames_with_own_hmac` | 51, 52, and 53 frames in each form |
| `fleet_frame_hmac_failures` | 51, 52, and 53 frames whose own HMAC was missing or wrong. They are dropped |
| `hmac_failures`, `malformed` | Datagrams and notifications that failed verification or parsing. Both should be 0 |
| `unsigned_notifications` | Spatial notifications outside a signed bundle whose header says they carry no HMAC. Nothing proves the realtime server sent one. Error messages are not among them |
| `unsigned_notifications_by_type` | The same, by message type |
| `unsigned_notifications_were` | What the unsigned actor updates were: sent by the receiving session, sent by another session of this run, the sender's first update after a carry result, without a marker of this run, and from beyond the replication distance of the receiving session's whole region |
| `updates_from_beyond_the_range_of_the_receivers_region` | Updates of this run, in any form, that came from farther than `--distance` plus one chunk from the receiving session's region. Every player of a session stands inside its region, so none of them is in range of such an update |
| `first_updates_after_a_carry_result` | The first update a session sends for a player after a carry result: how many were sent, how many notifications of them were received, and how many of those had no HMAC |
| `actor_left` | `ACTOR_LEFT` (145) by its reason byte (0: the actor went stale, 1: its session was released), whether the actor's player was carried by a session at that moment, and how many came from out of range |
| `actor_updates_not_of_this_run` | Actor updates with another payload, and ones with the marker of another run of this tool. The second kind means another run is using the same app at the same time |

### `replication`

`sample_players` players have a send time written into each update. `latency` is the
time from one session sending such an update to another session receiving it, on this
process's clock. `marked_updates_received` counts every update of this run that a
session received.

### `own_state`

Every update carries a marker that names the session that sent it and the player it
is for. The client sorts each update a session receives by who sent it:

| Field | Meaning |
|---|---|
| `sent_by_the_receiving_session.total` | Updates that came back to the session that sent them. Must be 0 with the capability off. With `--own-state` it is the traffic the capability adds. `received` is the same number |
| `...while_it_still_carried_the_player`, `...after_the_player_had_left_it` | Whether the session still carried the player when its own update came back |
| `...its_first_update_after_a_carry_result`, `...with_no_hmac_outside_a_signed_bundle` | How many of them were the session's first update for the player after a carry result, and how many arrived outside a signed bundle with no HMAC |
| `...age_when_received` | How old those updates were when they came back |
| `sent_by_another_session_before_the_handoff.total` | Updates for a player the receiver carries now, sent by another session before the receiver's carry result arrived. The session that carried the player before sent them, and they were still on the way at the handoff. Expected |
| `...sent_before_the_receivers_carry_result` | How long before the receiver's carry result those updates were sent (`max_ms`, `p99_ms`) |
| `sent_by_another_session_after_the_receivers_carry_result` | Updates for a player the receiver carries, sent by another session after the receiver's carry result. This client stops sending for a player the moment the player leaves a session, so it expects 0 |

The first 200 updates that came back to their sender are listed under
`events.own_state_sent_by_the_receiving_session`.

### `events`

A counter says how often something happened. `events` says what happened: it holds the
first 200 occurrences of each kind, with `recorded` and `not_recorded` counts. Each
event has `since_start_s` (seconds since the run began), `t_s` (seconds since the
measured phase opened, null during the setup), and the session.

| Kind | One event is |
|---|---|
| `own_state_sent_by_the_receiving_session` | An update that came back to its sender: the player, its age, how it arrived, and how long after the session's last carry result for that player it was sent |
| `state_for_a_carried_player_sent_by_another_session_after_the_carry` | An update for a player the receiver carries, sent by another session after the receiver's carry result |
| `notifications_outside_a_signed_bundle_without_a_verified_hmac` | A notification with no HMAC, or one that failed: its type and size, who sent the update, how far its chunk is from the receiving session's region, and whether the session carried that player before |
| `plain_bundles` | A plain bundle: its size, and each member's type, size, and own HMAC (`verified`, `failed`, or `none`) |
| `carry_status_1` | A carry answered status 1: its cause, how long after the newest token was stored it was sent, whether a refresh was in flight, and what the client did next |
| `unauthorized_...` | A refusal of that class (see `refused`) |
| `released_never_arrived` | A crossing whose old session received no `FLEET_RELEASED` within 5 seconds |
| `actor_left` | An `ACTOR_LEFT`: its reason, the actor's player, which session carried that player then, and how long after the player's last carry result it came |

### `bytes`

`tx` and `rx` are UDP payload bytes, in total. `per_session` lists each session's
`game_token_id`, server, bytes, datagrams, plain state notifications, `FLEET_DELIVERY`
frames with the players they listed, plain communications, and the players it held at
the end.

### `usage`: the bytes to lay beside the bill

`per_session_minute` has one row for each session and each minute of the wall clock
(UTC) in which the session sent or received anything. The tier's usage rows are by
session game token id and minute, so the two can be joined on `game_token_id` and
`minute`.

| Field | Meaning |
|---|---|
| `game_token_id`, `session`, `minute` | The session's `gameTokenId`, its number in this run, and the minute as `2026-11-25T21:05:00Z` |
| `received_datagrams`, `received_bytes` | What the realtime server sent the session: the tier's data out. UDP payload bytes |
| `sent_datagrams`, `sent_bytes` | What the session sent: the data in |
| `received_wire_bytes`, `sent_wire_bytes` | The same plus 42 bytes a datagram for the Ethernet, IPv4, and UDP headers |
| `carried_player_seconds` | The seconds the session's players were carried in that minute, summed over the players. 40 players carried for the whole minute are 2,400 |

The tier's usage rows for a fleet session hold its bytes and the seconds its players
were carried, and one session-minute can be several rows. To check a bill, sum the
tier's rows for a session and minute and compare the bytes and the carried seconds
with this row.

The client counts a datagram in the minute its own clock shows when it sends or reads
the datagram, so a datagram near a minute's edge can fall in the next row on one side.
Compare sums over a few minutes as well as single rows. The client counts every
datagram it receives; a datagram the network lost was counted by the server and is
missing here.

### `api`

| Field | Meaning |
|---|---|
| `calls`, `calls_by_kind`, `calls_by_cause` | Every HTTP request to the Application API, including retries |
| `calls_in_measured_phase`, `calls_in_measured_phase_by_kind`, `calls_in_measured_phase_by_cause` | The calls made while the players crossed |
| `calls_in_measured_phase_refreshes_due` | Of those, token refreshes and session keeps |
| `calls_in_measured_phase_other` | The rest. Must be 0 |
| `platform_busy_by_kind`, `rate_limited_by_kind`, `failures_by_kind` | Answers that were retried, and calls that failed |
| `calls_caused_by_crossing` | Must be 0 |

### `generator`

`send_lag` is how late the client sent each update against its schedule.
`updates_skipped` counts updates the client dropped because it had fallen a whole
interval behind. If the lag's 99th percentile nears the update interval (200 ms at
5 Hz), the host is the limit: add `--threads` or use a larger host before you read the
other numbers.

## The messages on the wire

**Client to server**, each in the long-form spatial layout. The HMAC in the tail is
always keyed with the fleet token.

| Message | Type | Tail's `gameTokenId` | App payload |
|---|---|---|---|
| `CLIENT_CAPABILITIES` | 29 | The session's | 4-byte flags (bit 0 signed bundles, bit 1 bound control, bit 3 `FLEET_OWN_STATE`), then the 8-byte control nonce when bit 1 is set |
| `CLIENT_ACTOR_HEARTBEAT` | 26 | The session's | None, and no actor: a keepalive |
| `FLEET_CARRY` | 49 | The session's | 8-byte player `gameTokenId`, 8-byte epoch milliseconds, 32-byte proof |
| `FLEET_RELEASE` | 50 | The session's | 8-byte player `gameTokenId`. Not answered |
| Anything a player does | 128 and others | The player's | What the player's own session would send |
| `CLIENT_ACTOR_HEARTBEAT` for a player's actor | 26 | The player's | None, with the actor's chunk and UUID |

The proof is `HMAC-SHA256(key = the player's 64-octet token, message = "ck-fleet-carry"
+ fleet gameTokenId + player gameTokenId + epoch milliseconds)`, each number 8 bytes,
little endian. The load run sends only the first five rows; the rule check sends the
sixth for its idle player.

**Server to client:**

| Message | Type | Frame |
|---|---|---|
| `FLEET_CARRY_RESULT` | 51 | `[51][8B player gameTokenId][8B user id][1B status]`, 18 bytes |
| `FLEET_RELEASED` | 52 | `[52][8B player gameTokenId][1B reason]`, 10 bytes |
| `FLEET_DELIVERY` | 53 | `[53][1B count][count x 8B player gameTokenId][notification]` |

A session reads each frame in two forms:

- **In a signed bundle.** A session that advertised `BUNDLE_SIGNED` receives the frame
  as a member of a `MESSAGE_BUNDLE_SIGNED` (30), with no HMAC of its own. The bundle's
  one HMAC covers it.
- **With its own HMAC.** Otherwise the frame is followed by 32 bytes:
  `HMAC-SHA256(key = the fleet token, message = the frame from its type byte on,
  followed by the fleet token)`. A 51 is then 50 bytes and a 52 is 42. The signed frame
  is alone in its datagram or a member of a `MESSAGE_BUNDLE` (2). A `FLEET_DELIVERY`
  above 1,197 bytes always comes this way, alone, in a datagram of up to 1,242 bytes.

The client verifies the HMAC of the second form, drops a frame whose HMAC is missing
or wrong, and counts it in `fleet_frame_hmac_failures`. The notification inside a 53 is
the unsigned form: a long-spatial one keeps its 9-byte tail and has `containsAuth` 0,
and a channel one is byte for byte what any session receives.

### What the client assumes where the contract is open

- **The marker lives in the update's payload.** The client sends the 88-byte actor
  state the per-player harness sends, which leaves rotation pitch and yaw at 0. The
  client writes the sending session, the player, a 10-bit tag of the run, whether the
  update is the session's first for the player after a carry result, and a send time
  into those two fields as whole numbers. The platform relays the payload as it is.
  That is how a receiving session knows which session sent an update. Use an app of your own, because a
  game client that renders these actors would show the numbers as a rotation.
- **A session-released command decides between a new server and the same one by its
  reason.** Reasons 3 and 4 ask for a new server, as `COMMAND_RECONNECT` does. Any other
  reason asks the API to authorize the session again where it is.
- **A session returns to "the same server" when the refresh answer has the address and
  port it had.**
- **Wire bytes add 42 a datagram.** The public wire-format reference counts a 109-byte
  heartbeat as about 151 bytes on the wire. If the tier's usage rows count payload
  bytes, compare with `received_bytes`.
- **`FLEET_RELEASE` has no acknowledgement.** A lost one leaves the carry until the
  player is carried elsewhere, idles out, or the session closes. The client closes
  every session at the end, which ends its carries.

## The tests run without a network

```bash
ctest --test-dir build -R fleet --output-on-failure
```

| Test | Covers |
|---|---|
| `fleet_wire_test` | `FLEET_CARRY`, `FLEET_RELEASE`, the keepalive, a player's heartbeat, the capabilities with and without a nonce, and a carried player's update against bytes computed outside this code; the contract's golden vectors; the proof against an HMAC built from SHA-256 by hand; 51, 52, and 53 in both forms, a wrong HMAC, and the 1,242-byte datagram; the move commands 22, 28, 44, 45, and the acknowledgement 46; the marker |
| `fleet_world_test` | The grid, neighbours, a crossing detected exactly once, the speed formula, and the cross-rate controller |
| `fleet_api_test` | Each GraphQL document and input, the retries, the counts by kind and cause, the roster file, and the org token's handling |
| `fleet_run_test` | The whole client against the stand-in: a clean run with the stand-in's own count of what it was sent; sessions told to move; a session authorized again on the same server; an ended sign-in; an idle release; lost datagrams; token refresh; slow carry results, a late new token, and a slow refresh answer around a token refresh; state held back across a handoff; a stray copy with no HMAC; both frame forms and a frame whose HMAC fails; the carry rate; the release-first plan; a `FLEET_DELIVERY` and a communication outside one; own state; statuses 2, 3, and 4; an interrupt; roster reuse; the org token that owns a session; `--delete-players`; `--rules-check` |
| `fleet_self_test`, `fleet_dry_run`, `fleet_rules_dry_run`, `fleet_needs_org_token` | The binary itself |

The stand-in (`src/fleet/FakeRealtime.*`) is written from the public wire formats
above. It checks each datagram's HMAC and each carry's proof, keeps one carrier for a
player, drops carries past 64 a second, sends state plain and once to a session, and
sends text and voice inside a `FLEET_DELIVERY`. Around a token refresh it answers as
the dev tier was seen to answer: a carry that names a replaced token gets status 1,
and after a carry with the new token a message under the old id is refused. It is a test double and says nothing
about how the realtime servers are built. Only a live run on a tier tests the realtime
servers themselves.
