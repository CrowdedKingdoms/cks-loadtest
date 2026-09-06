#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace lt {

/// Parsed LT_CONTROL_BIND ("host:port").
struct ControlBind {
    std::string host = "127.0.0.1";
    int port = 9109;
    bool loopback = true;   // 127.0.0.1 / ::1 / localhost
    bool wildcard = false;  // 0.0.0.0 / ::
    bool disabled = false;  // "off" / empty after explicit disable
};

ControlBind parseControlBind(const std::string& spec);

/// Runtime configuration. Precedence: CLI flags > environment variables
/// (LT_* names) > config file (--config, KEY=VALUE lines) > defaults.
struct Config {
    // Credentials / endpoints
    std::string email;                 // LT_EMAIL (required)
    std::string password;              // LT_PASSWORD (required unless roster covers)
    std::string managementApiUrl;      // LT_MANAGEMENT_API_URL (required)
    std::string gameApiUrl;            // LT_GAME_API_URL (optional override;
                                       //   default: from mintAppToken response)
    int64_t appId = 0;                 // LT_APP_ID (required)

    // Scale
    // Clients to provision at process start. 0 is valid when the control port
    // is on: the agent then hot-adds. Capacity is LT_INDEX_LIMIT.
    int clients = 10;                  // LT_CLIENTS
    int threads = 1;                   // LT_THREADS
    int updateHz = 10;                 // LT_UPDATE_HZ
    // Position units per second. <= 0 derives it from the pose format: 150
    // Unreal units/s for ue5, 4 blocks/s for bwf (a brisk walk; 150 blocks/s is
    // nine chunks a second and leaves every permission window every second).
    double walkSpeed = -1.0;           // LT_WALK_SPEED
    int spawnRadiusChunks = 8;         // LT_SPAWN_RADIUS_CHUNKS
    int distance = 8;                  // LT_DISTANCE (replication distance, chunks)
    // Which actor-state payload each client writes (Wire.hpp):
    //   ue5  the 88-byte float64 state v2, the platform's reference and the
    //        default; positions in Unreal units local to the chunk, Z up.
    //   bwf  the 48-byte float32 pose Blocks With Friends decodes; positions in
    //        world blocks, Y up, chunk = 16 blocks.
    // A game renders only the profile it speaks; the servers relay either.
    std::string poseFormat = "ue5";    // LT_POSE_FORMAT
    // Edge of one chunk in the pose's position units. <= 0 derives it from the
    // pose format (1600 for ue5, 16 for bwf).
    double chunkSizeUnits = -1.0;      // LT_CHUNK_SIZE_UNITS
    // Population shape. 0 = the original 2D random walk on the ground plane,
    // confined to spawnRadiusChunks of the origin. N >= 1 = a cube of N x N x N
    // chunks, centred on the origin chunk horizontally and standing on
    // volumeBaseUp vertically, filled uniformly by GLOBAL client index, every
    // client drifting in 3D and bouncing off the faces. 8 is the 8x8x8 /
    // 512-chunk geometry that exercises per-ring decay.
    int volumeChunks = 0;              // LT_VOLUME_CHUNKS
    int volumeBaseUp = 0;              // LT_VOLUME_BASE_UP (lowest vertical chunk)
    // The radius, in chunks, of the server's cached grid-permission box. Used
    // ONLY to classify UNAUTHORIZED refusals, never sent on the wire. The
    // server's value is its own constant and is not discoverable from here, so
    // this is a declared assumption: set it wrong and refusals move between the
    // "window reload" and "unexplained" tallies, which is the visible symptom.
    int permWindowRadiusChunks = 8;    // LT_PERMISSION_WINDOW_RADIUS_CHUNKS
    // How long after a window-crossing refusal the follow-on refusals belong to
    // the same reload episode. The server evicts under a backoff, so one
    // crossing legitimately costs several packets.
    int permReloadGraceMs = 5000;      // LT_PERMISSION_RELOAD_GRACE_MS
    int decay = 1;                     // LT_DECAY (0=none 1=exponential 2..5=linear)

    // Fleet identity. Emails use the GLOBAL index (base + local).
    std::string instanceId;            // LT_INSTANCE_ID (default: hostname)
    int indexBase = 0;                 // LT_INDEX_BASE
    // Max clients this process will ever hold. -1 means "unset at load";
    // Config::load fills it with `clients` so add is refused unless the
    // operator raised the limit.
    int indexLimit = -1;               // LT_INDEX_LIMIT
    int indexWidth = 4;                // LT_INDEX_WIDTH (zero-pad {index})

    // HTTP control (agent / cks-loadtest-ctl). Default loopback; a non-loopback
    // bind requires LT_CONTROL_TOKEN.
    std::string controlBind = "127.0.0.1:9109"; // LT_CONTROL_BIND ("off" disables)
    std::string controlToken;          // LT_CONTROL_TOKEN
    std::string statsDir;              // LT_STATS_DIR (rung JSON + interval JSONL)

    // Ramp-up / provisioning
    int rampBatchSize = 10;            // LT_RAMP_BATCH_SIZE
    int rampIntervalMs = 1000;         // LT_RAMP_INTERVAL_MS
    // A client that has been sending for this long with NOTHING received --
    // no notification, no refusal, no reconnect command -- is orphaned: its
    // Buddy restarted or dropped the session and answers nothing, which no
    // other trigger sees (a restarted Buddy's port is open, so there is no
    // ICMP; a dropped session is a silent drop, not an UNAUTHORIZED). It asks
    // for a fresh assignment, like a real client whose game went quiet would.
    // 0 disables: a lone client in an empty chunk legitimately hears nothing,
    // so this is for fleet runs (LT_RX_SILENT_REASSIGN_SEC=30 on the ladder).
    int rxSilentReassignSec = 0;       // LT_RX_SILENT_REASSIGN_SEC
    int provisionConcurrency = 4;      // LT_PROVISION_CONCURRENCY

    // Run control
    int durationSec = 0;               // LT_DURATION_SEC (0 = until Ctrl-C)
    int statsIntervalSec = 5;          // LT_STATS_INTERVAL_SEC
    std::string csvOut;                // LT_CSV_OUT (path; empty = disabled)

    // Identity derivation: {local} and {domain} come from LT_EMAIL,
    // {index} is the zero-padded GLOBAL client index.
    std::string emailPattern = "{local}+lt-{index}@{domain}"; // LT_EMAIL_PATTERN

    // Pre-minted identity sessions, so a run performs no sign-in at all.
    // Empty = sign in during provisioning (the portable default).
    std::string rosterFile;            // LT_ROSTER_FILE
    // Refuse to run when the roster cannot cover every client, instead of
    // silently signing the remainder in. A partial roster is the shape that
    // makes an unnoticed bcrypt convoy possible: it works, and the cost hides
    // in the clients the file happened not to reach.
    bool rosterRequired = false;       // LT_ROSTER_REQUIRED

    // Behavior toggles
    bool verifyServerHmac = false;     // LT_VERIFY_SERVER_HMAC
    bool tlsInsecure = false;          // LT_TLS_INSECURE (dev/self-signed only)
    int sessionSettleMs = 1500;        // LT_SESSION_SETTLE_MS (wait after assign)
    int rxHealthTimeoutSec = 10;       // LT_RX_HEALTH_TIMEOUT_SEC (0 = disabled)
    int tokenRefreshLeadSec = 120;     // LT_TOKEN_REFRESH_LEAD_SEC

    /// Zero-padded global index for {index} in the email pattern.
    std::string formatIndex(int globalIndex) const;

    /// Derive the sign-in email for a GLOBAL simulated client index.
    std::string derivedEmail(int globalIndex) const;

    /// True when a password is needed at all. With a roster covering every
    /// client the harness never signs in, so requiring LT_PASSWORD would be
    /// requiring a secret for an operation that does not happen.
    bool needsPassword() const { return rosterFile.empty() || !rosterRequired; }

    ControlBind parsedBind() const { return parseControlBind(controlBind); }

    bool isBwf() const { return poseFormat == "bwf"; }
    /// Chunk edge in position units, resolved per pose format when unset.
    double effectiveChunkSize() const {
        if (chunkSizeUnits > 0) return chunkSizeUnits;
        return isBwf() ? 16.0 : 1600.0;
    }
    /// Movement speed in position units per second, resolved per pose format.
    double effectiveWalkSpeed() const {
        if (walkSpeed > 0) return walkSpeed;
        return isBwf() ? 4.0 : 150.0;
    }

    /// Parse CLI + env + optional config file. Exits with a usage message on
    /// --help or invalid/missing options.
    static Config load(int argc, char** argv);

    /// Validate required fields; returns an error string or empty on success.
    std::string validate() const;
};

} // namespace lt
