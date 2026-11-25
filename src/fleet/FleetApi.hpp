#pragma once

#include "GraphQLClient.hpp"
#include "fleet/FleetStats.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

/// The Application API calls of the fleet load client, each counted by kind and
/// by cause.
///
/// The proof of the feature is that a crossing makes no call here. So every call
/// goes through this one class, the class is used by the API threads only, and
/// the realtime worker threads that handle crossings hold no handle to it.
namespace lt::fleet {

struct ServerAddr {
    std::string ip4;
    uint16_t port = 0;
    bool operator==(const ServerAddr& o) const { return ip4 == o.ip4 && port == o.port; }
};

/// openFleetSession / refreshFleetSession's answer.
struct FleetSessionInfo {
    std::string token;          // 64 characters: the session's signing key
    int64_t gameTokenId = 0;
    int64_t appId = 0;
    int64_t expiresAtMs = 0;    // epoch milliseconds
    int maxPlayers = 0;
    ServerAddr server;
};

/// mintManagedPlayerToken / refreshAppToken's answer.
struct PlayerToken {
    std::string token;          // 64 characters: the player's app token
    int64_t gameTokenId = 0;
    int64_t expiresAtMs = 0;
};

/// The only thing that reaches the network. One instance a thread.
class ApiTransport {
public:
    virtual ~ApiTransport() = default;
    /// POST one GraphQL document with `Authorization: Bearer <bearer>` and no
    /// Origin header. Returns `data`. Throws GraphQLError.
    virtual nlohmann::json request(const std::string& query, const nlohmann::json& variables,
                                   const std::string& bearer) = 0;
    /// refreshAppToken with the player's own app token as the bearer.
    /// `httpCalls` is set to the HTTP requests it took, including failed ones.
    /// Throws GraphQLError.
    virtual PlayerToken refreshAppToken(const std::string& playerToken, int& httpCalls) = 0;
};

/// libcurl, through the harness's GraphQLClient. A player's token is refreshed
/// by the harness's Provisioner::refreshToken, the same code the per-player
/// harness uses.
std::unique_ptr<ApiTransport> makeCurlTransport(const std::string& apiUrl, bool tlsInsecure);

/// The app's externalId for player `n`: fleet-load-<n>.
std::string playerExternalId(int n);

class FleetApi {
public:
    /// `sleepMs` waits between retries; the default sleeps and returns early
    /// when `stop` is set. Tests pass their own.
    FleetApi(std::unique_ptr<ApiTransport> transport, std::string orgToken, int64_t appId,
             ApiCounters& counters, std::atomic<bool>* stop = nullptr,
             std::function<void(int)> sleepMs = nullptr);

    FleetSessionInfo openFleetSession(ApiCause cause);
    PlayerToken mintManagedPlayerToken(int n, ApiCause cause);
    bool closeFleetSession(int64_t gameTokenId, ApiCause cause);
    FleetSessionInfo refreshFleetSession(int64_t gameTokenId, const ServerAddr& current,
                                         ApiCause cause);
    PlayerToken refreshAppToken(const std::string& playerToken, ApiCause cause);

    /// Replace every occurrence of the org token in `text`. Error text from the
    /// API should never hold it; this makes sure a log line cannot.
    std::string redact(std::string text) const;

    /// An API answer that says "try again": the wait it names, or the default.
    static constexpr int DEFAULT_RETRY_AFTER_MS = 1500;
    /// Give up on one operation after this many busy or rate-limited answers.
    static constexpr int MAX_BUSY_RETRIES = 60;
    /// Attempts for a failure below GraphQL (connection, timeout, a bad gateway).
    static constexpr int MAX_TRANSPORT_ATTEMPTS = 3;

private:
    /// Run one operation: count each HTTP call, wait and retry on PLATFORM_BUSY
    /// and RATE_LIMITED, retry a transport failure a few times.
    template <typename Fn> auto call(ApiKind kind, ApiCause cause, Fn&& once);

    std::unique_ptr<ApiTransport> transport_;
    std::string orgToken_;
    int64_t appId_;
    ApiCounters& counters_;
    std::atomic<bool>* stop_;
    std::function<void(int)> sleepMs_;
};

/// Set on a realtime worker thread while it handles a crossing. FleetApi counts
/// a call made with it set (ApiCounters::requestsFromCrossingPath) and tags the
/// call ApiCause::Crossing, so a regression that reached the API from a crossing
/// would fail the run instead of passing unseen.
bool& crossingScopeFlag();
struct CrossingScope {
    bool prev;
    CrossingScope() : prev(crossingScopeFlag()) { crossingScopeFlag() = true; }
    ~CrossingScope() { crossingScopeFlag() = prev; }
};

} // namespace lt::fleet
