#pragma once

#include "Config.hpp"
#include "GraphQLClient.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
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

/// Provisioning against the public GraphQL APIs:
///   1. login (or register on first run) each derived account -> session token
///   2. mintAppToken(appId)                                   -> app token
///   3. serverWithLeastClients on the Game API                -> Buddy address
///
/// Also provides the runtime operations (token refresh, server reassignment)
/// used by the control thread while the test runs.
class Provisioner {
public:
    explicit Provisioner(const Config& config);

    /// Provision credentials for all configured clients, using
    /// config.provisionConcurrency parallel threads. Fatal errors (bad
    /// password, missing entitlement, unreachable API) print an actionable
    /// message and abort the whole run by returning an empty vector.
    /// `stop` aborts provisioning early (e.g. on SIGINT).
    std::vector<ClientCredentials> provisionAll(std::atomic<bool>& stop);

    /// Rotate the app token (refreshAppToken with the current app token as
    /// bearer) and install a session on a (possibly different) Buddy.
    /// Throws GraphQLError on failure.
    void refreshToken(GraphQLClient& mgmt, ClientCredentials& c);

    /// Re-run serverWithLeastClients for this client's current token and
    /// update the assignment. Throws GraphQLError on failure.
    void assignServer(ClientCredentials& c);

    const Config& config() const { return config_; }

private:
    /// login -> register -> login. Returns the session token.
    std::string signIn(GraphQLClient& mgmt, const std::string& email);
    void mintAppToken(GraphQLClient& mgmt, ClientCredentials& c);
    /// gameClientBootstrap: mirrors the user's entitlements + default grid
    /// access into the game stack so Buddy authorizes spatial traffic.
    void bootstrapGameClient(ClientCredentials& c);

    const Config& config_;
};

/// Parse an ISO-8601 UTC timestamp ("2026-07-17T21:00:00.000Z").
/// Returns epoch time; on parse failure returns now + fallbackSec.
std::chrono::system_clock::time_point parseIso8601Utc(const std::string& iso,
                                                      int fallbackSec = 25 * 60);

} // namespace lt
