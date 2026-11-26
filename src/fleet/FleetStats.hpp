#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
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

/// What happened, for the events a counter alone does not explain: a refusal,
/// a carry answered "no live token", state that came back to its sender, a
/// notification outside a signed bundle. Each kind keeps its first 200, as JSON
/// objects, for the summary; the counters keep the totals.
class EventLog {
public:
    static constexpr size_t PER_KIND = 200;
    EventLog();
    void add(const std::string& kind, nlohmann::json event);
    /// {kind: {"recorded": n, "not_recorded": n, "events": [...]}}
    nlohmann::json toJson() const;

private:
    mutable std::mutex mu_;
    int64_t startNs_ = 0;
    std::map<std::string, std::vector<nlohmann::json>> events_;
    std::map<std::string, uint64_t> dropped_;
};

enum class ApiKind : uint8_t {
    OpenFleetSession,
    MintManagedPlayerToken,
    CloseFleetSession,
    RefreshFleetSession,
    RefreshAppToken,
    DeleteManagedPlayer,
    BlockPlayer,
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
    Cleanup,        // --delete-players
    RulesCheck,     // --rules-check
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
    Counter plainState{0};           // state notifications received plain
    Counter deliveries{0};           // FLEET_DELIVERY frames
    Counter deliveryPlayers{0};      // players those frames listed
    Counter plainCommunications{0};  // a communication outside a FLEET_DELIVERY: a server defect
    Counter updatesOutOfRange{0};    // updates from beyond the replication distance of its region
    Counter rxBytesOutOfRange{0};    // their bytes, as members (without the bundle's own bytes)
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
    /// Calls made while the measured window was open, by kind and by cause.
    std::array<Counter, K> duringWindow{};
    std::array<Counter, C> duringWindowByCause{};
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
    // Status 1 (no such live token) around a token refresh or a sign-in: the
    // carry is sent again before it counts as a refusal.
    Counter carryTookNewerToken{0};             // a waiting or resent carry named a token stored meanwhile
    Counter carryRetriedAfterRefresh{0};        // all such retries
    Counter carryRetriedWithNewerToken{0};      // the carry named the token a refresh had replaced
    Counter carryRetriedAfterWait{0};           // the realtime server did not know the new token yet
    Counter carryStatus1Refusals{0};            // not explained by a refresh: the player signs in again
    Hist tokenCarryDelay;                       // from the refresh's answer to the carry with the new token
    Counter carryTimedOut{0};
    Counter carryAbandoned{0};        // the player moved on before the result
    Counter carryRetries{0};          // carries started again after giving up
    Counter carryResends{0};          // FLEET_CARRY sent again, with a new time and proof
    Counter carryPacedWaits{0};       // carries that waited for the session's carry rate
    Counter carryResultsDuplicate{0};
    Counter carryResultsLate{0};      // for a player the session no longer holds
    Counter carryResultsUnknown{0};
    Hist carryLatency;

    // Crossings.
    Counter crossingsAttempted{0};
    Counter crossingsCompleted{0};
    Counter crossingsSuperseded{0};   // crossed again before the carry completed
    Counter crossingReleasesFirst{0}; // FLEET_RELEASE sent by the old session (release-first plan)
    Hist crossingLatency;

    // FLEET_RELEASED.
    std::array<Counter, 256> releasedByReason{};
    Counter releaseExpected{0};       // crossings that left a session which carried the player
    Counter releaseReceived{0};       // of those, FLEET_RELEASED reason 1 arrived and was paired
    Counter releaseBeforeResult{0};   // it arrived before the new session's result
    Counter releaseMissing{0};        // it had not arrived 5 s after the new session's result
    Counter releaseUnpaired{0};       // the expectation ended with no result to pair with
    Counter releaseNamedNewerToken{0}; // it named the token id of a carry the player left behind unanswered
    Hist releaseDelay;                // after the new session's result
    Counter releasedUnexpected{0};    // reason 1 for a player the session holds
    Counter releasedAfterLateCarry{0};
    Counter releasedAfterOwnRelease{0}; // reason 1 after the session's own FLEET_RELEASE
    Counter releasedUnknownPlayer{0};
    Counter released2Held{0};         // reason 2 for a player the session holds
    Counter released5Held{0};         // reason 5 for a player the session holds
    Counter releasedOtherHeld{0};
    Counter recarriedAfterRelease{0};
    Counter resignRequests{0};

    // Refusals (GENERIC_ERROR_MESSAGE).
    std::array<Counter, 256> errorCodes{};
    Counter playerMessagesRefused{0}; // UNAUTHORIZED answering a message sent for a player
    Counter unauthTokenRefresh{0};            // the message named the token id a refresh was replacing
    Counter unauthTokenRefreshBeforeResult{0}; // of those: before the carry with the new token was answered
    Counter unauthTokenRefreshAfterResult{0};  // of those: after it
    Counter unauthFirstContact{0};
    Counter unauthWindowReload{0};
    Counter unauthAfterHandoff{0};
    Counter unauthAfterCarryEnded{0};         // sent before a FLEET_RELEASED or a session move ended the carry
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
    Counter fleetFramesInSignedBundle{0}; // 51, 52, 53 as members of a verified signed bundle
    Counter fleetFramesWithOwnHmac{0};    // 51, 52, 53 followed by their own HMAC, verified
    Counter fleetFrameHmacFailures{0};    // 51, 52, 53 whose HMAC is missing or wrong: dropped
    Counter plainState{0};                // state notifications received plain
    Counter plainCommunications{0};       // communications outside a FLEET_DELIVERY: a defect
    Counter legacyControlDropped{0};      // 22 or 28 after the server acknowledged bound control
    Counter boundAcks{0};
    Counter sessionReturnsSameServer{0};  // capabilities only: the carries stand
    Counter sessionReturnsOtherServer{0}; // capabilities and a new carry for every player
    Counter deliveries{0};
    Counter deliveryPlayers{0};
    Counter deliveryForUncarried{0};
    std::array<Counter, 256> deliveryByType{};

    // Where each message arrived, by type: as a member of a verified signed
    // bundle, as a member of a plain bundle, or alone in its datagram. And for the
    // two last, how it was authenticated.
    std::array<Counter, 256> rxInSignedBundle{};
    std::array<Counter, 256> rxInPlainBundle{};
    std::array<Counter, 256> rxStandalone{};
    std::array<Counter, 256> rxOutsideOwnHmacOk{};      // its own HMAC verified
    std::array<Counter, 256> rxOutsideOwnHmacFailed{};  // its own HMAC did not verify
    std::array<Counter, 256> rxOutsideNoHmac{};         // it carried none
    Counter plainBundleMembers{0};

    // ACTOR_LEFT (145).
    std::array<Counter, 256> actorLeftByReason{};
    Counter actorLeftForCarriedPlayer{0};   // the actor's player was carried by a session at that moment
    Counter actorLeftForUncarriedPlayer{0};
    Counter actorLeftUnknownActor{0};       // not one of this run's actors

    // Actor updates that are not this run's.
    Counter actorUpdatesOtherPayload{0};    // no marker: another payload
    Counter actorUpdatesOtherRun{0};        // a marker of another run of this tool

    // Replication latency and own state. Every update's marker names the session
    // that sent it and the player it is for.
    Counter markersSeen{0};
    Counter ownStateReceived{0};            // a session received an update IT sent: must be 0
    Counter ownSentWhileCarrying{0};        // of those: it still carried the player
    Counter ownSentAfterPlayerLeft{0};      // of those: the player had crossed to another session
    Counter ownSentFirstUpdate{0};          // of those: the session's first update after a carry result
    Counter ownSentUnsigned{0};             // of those: outside a signed bundle, with no HMAC
    // The first update a session sends for a player after a carry result, as the
    // sessions that receive it see it.
    Counter firstUpdatesSent{0};
    Counter firstUpdatesReceived{0};
    Counter firstUpdatesReceivedUnsigned{0};
    // Notifications outside a signed bundle with no HMAC, by what they were.
    Counter unsignedSentByReceiver{0};      // an update the receiving session sent
    Counter unsignedSentByAnother{0};       // an update another session of this run sent
    Counter unsignedFirstUpdate{0};         // an update that was its sender's first after a carry result
    Counter unsignedNotThisRun{0};          // an update with no marker of this run
    Counter unsignedOtherType{0};           // not an actor update
    Counter unsignedOutOfRange{0};          // an update from beyond the replication distance of the
                                            //   receiving session's whole region
    Counter updatesOutOfRange{0};           // the same, for every update of this run received
    Counter updatesOutOfRangeBytes{0};      // their bytes, as members
    Counter updatesOutOfRangeNearFormerPlayer{0}; // of the sampled ones: in range of a player the
                                                  //   session carried before and no longer carries
    Counter updatesOutOfRangeSampled{0};
    // Every update of this run by how far its chunk is from the receiving
    // session's region, in chunks (the last entry: that many or more).
    std::array<Counter, 258> updatesByChunksFromRegion{};
    Counter actorLeftOutOfRange{0};         // an ACTOR_LEFT from beyond it
    Hist ownSentAge;                        // how old those updates were when they came back
    Counter otherSentBeforeHandoff{0};      // sent by ANOTHER session before the receiver's carry result
    Hist handoffLead;                       // how long before the receiver's carry result they were sent
    Counter otherSentAfterCarry{0};         // sent by another session after the receiver's carry result
    Hist replicationLatency;
    EventLog events;

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
