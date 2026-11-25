#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/// Counters of the fleet load client. As in Stats.hpp: plain atomics written by
/// the worker threads with relaxed ordering and read by the reporter.
namespace lt::fleet {

/// Nanoseconds on the steady clock since this process started measuring.
int64_t nowNs();
/// Milliseconds since the Unix epoch on the system clock.
int64_t epochMs();

/// A latency histogram in microseconds: 64 buckets, each a quarter wider than
/// the last, from 50 microseconds to about a minute. Percentiles interpolate
/// inside a bucket; the maximum is exact.
class Hist {
public:
    static constexpr int BUCKETS = 64;
    void record(int64_t us);
    uint64_t count() const { return count_.load(std::memory_order_relaxed); }
    double maxMs() const { return static_cast<double>(max_.load(std::memory_order_relaxed)) / 1000.0; }
    double meanMs() const;
    /// `p` in [0, 1]. 0 when nothing was recorded.
    double percentileMs(double p) const;
    /// {samples, p50_ms, p95_ms, p99_ms, max_ms, mean_ms}
    nlohmann::json toJson() const;
    /// Upper bound (exclusive) of bucket `i`, in microseconds.
    static double boundUs(int i);

private:
    std::array<std::atomic<uint64_t>, BUCKETS> buckets_{};
    std::atomic<uint64_t> count_{0};
    std::atomic<uint64_t> sum_{0};
    std::atomic<uint64_t> max_{0};
};

using Counter = std::atomic<uint64_t>;

enum class ApiKind : uint8_t {
    OpenFleetSession,
    MintManagedPlayerToken,
    CloseFleetSession,
    RefreshFleetSession,
    RefreshAppToken,
    COUNT
};
/// Why an API call was made. `Crossing` exists so the summary can state its
/// count; nothing in the client makes a call with it.
enum class ApiCause : uint8_t {
    Setup,          // opening the sessions and signing the players in
    Teardown,       // closing the sessions
    SessionMove,    // a realtime server told a session to move
    SessionKeep,    // a session's token neared its expiry
    TokenRefresh,   // a player's app token neared its expiry
    SignInAgain,    // a player's sign-in ended and the player was signed in again
    Crossing,       // a player crossed from one server to another: must stay 0
    COUNT
};
const char* apiKindName(ApiKind k);
const char* apiCauseName(ApiCause c);

/// Why a carry was started.
enum class CarryCause : uint8_t {
    Join,           // the player joined the game
    Crossing,       // the player stepped into this server's region
    TokenRefresh,   // the player's app token was refreshed: carry again with the new one
    SignInAgain,    // the player was signed in again after the sign-in ended
    Recarry,        // the session was told it no longer carries a player it holds
    SessionMove,    // the session reconnected to a realtime server
    COUNT
};
const char* carryCauseName(CarryCause c);

/// Why a session moved to another realtime server.
enum class MoveTrigger : uint8_t {
    ReconnectCommand,
    SessionReleased,
    SendErrors,
    ErrorStreak,
    RxSilence,
    COUNT
};
const char* moveTriggerName(MoveTrigger t);

struct SessionIo {
    Counter txBytes{0};
    Counter rxBytes{0};
    Counter txDatagrams{0};
    Counter rxDatagrams{0};
    std::atomic<int> players{0};
    std::atomic<int> carried{0};
};

struct ApiCounters {
    static constexpr int K = static_cast<int>(ApiKind::COUNT);
    static constexpr int C = static_cast<int>(ApiCause::COUNT);
    std::array<std::array<Counter, C>, K> calls{};
    std::array<Counter, K> platformBusy{};
    std::array<Counter, K> rateLimited{};
    std::array<Counter, K> failures{};
    /// Calls made while the measured window was open, by kind.
    std::array<Counter, K> duringWindow{};
    std::atomic<bool> windowOpen{false};
    /// An API request raised from inside the code that handles a crossing. The
    /// workers assert this cannot happen; the count is the evidence.
    Counter requestsFromCrossingPath{0};

    uint64_t byKind(ApiKind k) const;
    uint64_t byCause(ApiCause c) const;
    uint64_t total() const;
    void count(ApiKind k, ApiCause c);
};

struct FleetStats {
    explicit FleetStats(int sessions);

    // Sessions.
    Counter sessionsOpened{0};
    Counter sessionsClosed{0};
    Counter sessionCloseFailures{0};
    std::array<Counter, static_cast<int>(MoveTrigger::COUNT)> sessionMoves{};
    Counter sessionMovesCompleted{0};
    Counter sessionMoveFailures{0};
    Counter sessionKeepRefreshes{0};
    Counter reconnectCommands{0};
    std::array<Counter, 256> sessionReleasedByReason{};
    std::atomic<int> maxPlayersPerSession{0};

    // Players.
    Counter playersSignedIn{0};       // mintManagedPlayerToken answered during setup
    Counter playersFromRoster{0};
    Counter playersAdmitted{0};       // handed to the world
    Counter signInFailures{0};
    Counter signedInAgain{0};
    Counter signInAgainFailures{0};
    Counter tokenRefreshes{0};
    Counter tokenRefreshFailures{0};
    Counter playersFailed{0};         // refused for good (another organization's or app's player)

    // Carries.
    std::array<Counter, static_cast<int>(CarryCause::COUNT)> carryAttempts{};
    std::array<Counter, static_cast<int>(CarryCause::COUNT)> carryCarried{};
    Counter carryDatagrams{0};
    std::array<Counter, 256> carryRefused{};   // by status (1 and up)
    Counter carryTimedOut{0};
    Counter carryAbandoned{0};        // the player moved on before the result
    Counter carryRetries{0};
    Counter carryResultsDuplicate{0};
    Counter carryResultsLate{0};      // for a player the session no longer holds
    Counter carryResultsUnknown{0};
    Hist carryLatency;

    // Crossings.
    Counter crossingsAttempted{0};
    Counter crossingsCompleted{0};
    Counter crossingsSuperseded{0};   // crossed again before the carry completed
    Hist crossingLatency;

    // FLEET_RELEASED.
    std::array<Counter, 256> releasedByReason{};
    Counter releaseExpected{0};       // crossings that left a session which carried the player
    Counter releaseReceived{0};       // of those, FLEET_RELEASED reason 1 arrived and was paired
    Counter releaseBeforeResult{0};   // it arrived before the new session's result
    Counter releaseMissing{0};        // it had not arrived 5 s after the new session's result
    Counter releaseUnpaired{0};       // the expectation ended with no result to pair with
    Hist releaseDelay;                // after the new session's result
    Counter releasedUnexpected{0};    // reason 1 for a player the session holds
    Counter releasedAfterLateCarry{0};
    Counter releasedUnknownPlayer{0};
    Counter released2Held{0};         // reason 2 for a player the session holds
    Counter released5Held{0};         // reason 5 for a player the session holds
    Counter releasedOtherHeld{0};
    Counter recarriedAfterRelease{0};
    Counter resignRequests{0};

    // Refusals (GENERIC_ERROR_MESSAGE).
    std::array<Counter, 256> errorCodes{};
    Counter playerMessagesRefused{0}; // UNAUTHORIZED answering a message sent for a player
    Counter unauthFirstContact{0};
    Counter unauthWindowReload{0};
    Counter unauthAfterHandoff{0};
    Counter unauthUnexplained{0};
    Counter unauthSessionMessages{0}; // UNAUTHORIZED answering a session's own message
    Counter recarriedAfterUnauthorized{0};

    // Traffic.
    Counter updatesSent{0};
    Counter updatesSkipped{0};        // the generator fell a whole interval behind
    Counter capabilitiesSent{0};
    Counter heartbeatsSent{0};
    Counter releasesSent{0};
    Counter txSendErrors{0};
    std::array<Counter, 256> rxByType{};
    Counter rxSignedBundles{0};
    Counter rxPlainBundles{0};
    Counter rxHmacFailures{0};
    Counter rxMalformed{0};
    Counter rxUnsignedSpatial{0};
    Counter fleetFramesUnsigned{0};
    Counter fleetFramesLongForm{0};
    Counter deliveries{0};
    Counter deliveryPlayers{0};
    Counter deliveryForUncarried{0};
    std::array<Counter, 256> deliveryByType{};

    // Replication latency and own state.
    Counter markersSeen{0};
    Counter ownStateReceived{0};
    Hist replicationLatency;

    // The generator itself.
    Hist sendLag;

    // Bytes.
    Counter txBytes{0};
    Counter rxBytes{0};
    Counter txDatagrams{0};
    Counter rxDatagrams{0};
    std::vector<std::unique_ptr<SessionIo>> sessions;

    ApiCounters api;
};

uint64_t sum(const Counter* begin, size_t n);
template <size_t N> uint64_t sum(const std::array<Counter, N>& a) { return sum(a.data(), N); }

const char* errorCodeName(int code);
const char* messageTypeName(int type);

} // namespace lt::fleet
