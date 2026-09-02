#pragma once

#include "Config.hpp"
#include "GraphQLClient.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lt {

/// Everything a simulated client needs to talk to a Buddy server.
struct ClientCredentials {
    int index = 0;
    std::string email;
    std::string sessionToken;                 // identity session (management)
    std::string appToken;                     // 64-char app-scoped token
    int64_t gameTokenId = 0;
    std::string gameApiUrl;
    std::chrono::system_clock::time_point tokenExpiresAt{};

    // Buddy assignment (serverWithLeastClients side effect installs the
    // UDP session for gameTokenId on this server).
    std::string serverIp4;
    uint16_t serverPort = 0;
    /// Earliest time UDP traffic may start (assignment + settle delay).
    std::chrono::steady_clock::time_point udpReadyAt{};
};

/// How each client's identity session was obtained. AUTHENTICATION IS PART OF
/// THE MEASUREMENT: bcrypt runs at 10 rounds and one login costs 0.4-1.4s of
/// API CPU, so a run that signs in is partly measuring its own sign-in. The
/// harness therefore counts these and reports them rather than absorbing them.
struct SignInTally {
    /// Sessions taken from a pre-minted roster: no bcrypt, no API CPU.
    std::atomic<int> reused{0};
    /// Sessions minted by this process before the ramp began.
    std::atomic<int> mintedProvisioning{0};
    /// Sessions minted AFTER the ramp began. Any non-zero value means the
    /// steady-state numbers include somebody's bcrypt. Counted separately
    /// because "during provisioning" and "during the run" are different
    /// claims and only the second corrupts a measurement.
    std::atomic<int> mintedMidRun{0};
};

/// Provisioning against the public GraphQL APIs:
///   1. a roster session, or login (register on first run) -> session token
///   2. mintAppToken(appId)                                -> app token
///   3. serverWithLeastClients on the Game API             -> Buddy address
///
/// Also provides the runtime operations (token refresh, server reassignment)
/// used by the control thread while the test runs.
class Provisioner {
public:
    explicit Provisioner(const Config& config);

    /// Provision `count` clients starting at GLOBAL index `globalStart`
    /// (inclusive), using config.provisionConcurrency parallel threads.
    /// Fatal errors print an actionable message and return an empty vector.
    /// `stop` aborts provisioning early (e.g. on SIGINT).
    std::vector<ClientCredentials> provisionRange(int globalStart, int count,
                                                  std::atomic<bool>& stop);

    /// Provision the start-of-run slice: [indexBase, indexBase + clients).
    std::vector<ClientCredentials> provisionAll(std::atomic<bool>& stop) {
        if (config_.clients <= 0) return {};
        return provisionRange(config_.indexBase, config_.clients, stop);
    }

    /// Rotate the app token (refreshAppToken with the current app token as
    /// bearer) and install a session on a (possibly different) Buddy.
    /// Throws GraphQLError on failure.
    void refreshToken(GraphQLClient& mgmt, ClientCredentials& c);

    /// Re-run serverWithLeastClients for this client's current token and
    /// update the assignment. Throws GraphQLError on failure.
    void assignServer(ClientCredentials& c);

    const Config& config() const { return config_; }

    const SignInTally& signIns() const { return signIns_; }

    /// Called once when the ramp starts. Sign-ins after this point are counted
    /// as mid-run, which is the number a reader of the results needs.
    void markRunStarted() { runStarted_.store(true); }

    /// How many roster entries were loaded, and how many clients they cover.
    int rosterSize() const { return static_cast<int>(roster_.size()); }

private:
    /// A roster session, if this index has one. Empty otherwise.
    std::string rosterSession(int index, const std::string& email) const;
    /// login -> register -> login. Returns the session token.
    std::string signIn(GraphQLClient& mgmt, const std::string& email);
    void mintAppToken(GraphQLClient& mgmt, ClientCredentials& c);
    /// gameClientBootstrap: mirrors the user's entitlements + default grid
    /// access into the game stack so Buddy authorizes spatial traffic.
    void bootstrapGameClient(ClientCredentials& c);

    const Config& config_;
    /// index -> session token, from config_.rosterFile.
    std::unordered_map<int, std::string> roster_;
    /// Emails the roster names, so a roster minted for a different population
    /// is a refusal rather than a silent re-sign-in.
    std::unordered_map<int, std::string> rosterEmails_;
    SignInTally signIns_;
    std::atomic<bool> runStarted_{false};
};

/// Parse an ISO-8601 UTC timestamp ("2026-07-17T21:00:00.000Z").
/// Returns epoch time; on parse failure returns now + fallbackSec.
std::chrono::system_clock::time_point parseIso8601Utc(const std::string& iso,
                                                      int fallbackSec = 25 * 60);

} // namespace lt
