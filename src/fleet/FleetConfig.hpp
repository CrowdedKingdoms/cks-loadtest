#pragma once

#include <cstdint>
#include <string>

namespace lt::fleet {

/// Options of ck-fleet-load. The org token is never an option: it is read from
/// the environment variable CK_ORG_TOKEN, and nothing prints it.
struct FleetConfig {
    // What to run against.
    std::string api;              // --api: the Application API's GraphQL endpoint
    int64_t appId = 0;            // --app: an app of the organization

    // Scale.
    int servers = 100;            // --servers: simulated game servers, one fleet session each
    int players = 3000;           // --players: players carried in total
    int durationS = 1800;         // --duration-s: the measured window, after every player has joined
    double updateHz = 5.0;        // --update-hz: actor updates a second for each carried player
    double crossRate = 75.0;      // --cross-rate: target crossings a second over all players
    double signinRate = 20.0;     // --signin-rate: mintManagedPlayerToken calls a second
    // Region side in chunks. A player within `distance` chunks of a border is in
    // replication range of players standing just across it. With the default
    // distance of 8, a 16-chunk region has every player within 8 chunks of a
    // border, so each server keeps receiving its neighbours' players.
    int regionChunks = 16;        // --region-chunks
    uint64_t seed = 1;            // --seed: placement and walks
    bool ownState = false;        // --own-state: set the FLEET_OWN_STATE capability bit
    std::string rosterFile;       // --roster-file: minted player tokens, saved and reused
    bool dryRun = false;          // --dry-run: no network
    bool selfTest = false;        // --self-test

    // Other modes.
    bool deletePlayers = false;   // --delete-players: delete the players this client made, and exit
    int64_t orgId = 0;            // --org: the organization's id (deleteManagedPlayer needs it)
    bool rulesCheck = false;      // --rules-check: the per-player rule checks, and exit
    double rulesIdleTimeoutS = 60; // --rules-idle-timeout-s: the app's idle timeout (0 skips that check)
    double rulesStepS = 2.5;      // --rules-step-s: how long a check waits for what it expects

    // The crossing the plan makes. False (the default): the destination session
    // sends FLEET_CARRY and the session that carried before is sent FLEET_RELEASED
    // reason 1, with no coordination between the two servers. True: the old
    // session sends FLEET_RELEASE first, then the destination carries.
    bool releaseFirst = false;    // --crossing-plan release-first
    // A realtime server drops carries past 64 a second a session (burst
    // max_players + 32), unanswered. The client stays under that.
    double carryRate = 60.0;      // --carry-rate: FLEET_CARRY a second a session, resends included
    int carryBurst = 32;          // --carry-burst
    bool signedBundles = true;    // --signed-bundles: advertise BUNDLE_SIGNED
    bool boundControl = true;     // --bound-control: advertise BOUND_CONTROL with a nonce (44, 45)

    // Wire and world details.
    int distance = 8;             // --distance: replication distance of an actor update, chunks
    int decay = 1;                // --decay: 0 none, 1 exponential, 2..5 linear
    double chunkSizeUnits = 1600; // --chunk-size-units: chunk edge in position units (UE5 state)
    int latencySample = 100;      // --latency-sample: players whose updates carry a send time
    double heartbeatS = 2.0;      // --heartbeat-s: the session's own keep-alive (0 turns it off)
    int capsIntervalS = 15;       // --caps-interval-s: CLIENT_CAPABILITIES again this often
    int sessionSettleMs = 1500;   // --session-settle-ms: wait after the API authorizes a session
    int rxSilentS = 0;            // --rx-silent-s: move a session that hears nothing this long (0 off)
    int socketRcvbufBytes = 1024 * 1024; // --socket-rcvbuf-bytes (0 keeps the kernel default)

    // The generator.
    int threads = 4;              // --threads: realtime worker threads
    int apiThreads = 8;           // --api-threads: Application API threads
    int statsIntervalS = 5;       // --stats-interval-s: the progress line on stderr
    int tokenRefreshLeadS = 120;  // --token-refresh-lead-s: refresh a player's token this early
    int drainS = 6;               // --drain-s: wait for open carries and releases at the end
    bool tlsInsecure = false;     // --tls-insecure
    std::string summaryFile;      // --summary-file: also write the summary JSON here

    // Dry run.
    double dryRunS = 5.0;         // --dry-run-s: how long the world runs in a dry run

    // Timeouts; tests shorten them.
    int carryResendMs = 250;      // --carry-resend-ms: send FLEET_CARRY again, with a new time and
                                  //   proof; each wait is twice the last, up to eight times this
    int carryGiveUpMs = 5000;     // give up on a carry and count it
    int carryRetryMs = 1000;      // start again after giving up
    // FLEET_CARRY_RESULT status 1 around a token refresh is sent again:
    int status1WaitMs = 200;      // after this wait, when the token is the newest one
    int status1Retries = 10;      // at most this many times in a row
    int status1RecentMs = 5000;   // "around a refresh": the newest token was stored within this
    int releaseWaitMs = 5000;     // FLEET_RELEASED reason 1 counted missing after this
    int permWindowRadiusChunks = 8;
    int permReloadGraceMs = 5000;
    int firstContactGraceMs = 2000;

    // Whether the command line set these: a dry run picks its own otherwise.
    bool signinRateGiven = false;
    bool sessionSettleGiven = false;
    bool rulesIdleGiven = false;
    bool rulesStepGiven = false;

    /// Read from CK_ORG_TOKEN by load(). Never printed, never in the summary.
    std::string orgToken;

    /// Parse argv. Prints usage and exits on --help or a bad option.
    static FleetConfig load(int argc, char** argv);
    /// Empty when the options make a run possible, otherwise what is wrong.
    std::string validate() const;
    /// The `--api` value with /graphql removed: what a roster records.
    std::string apiOrigin() const;
};

} // namespace lt::fleet
