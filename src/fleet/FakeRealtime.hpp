#pragma once

#include "fleet/FleetApi.hpp"
#include "fleet/FleetWorker.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/// An in-process stand-in for the Application API and the realtime servers, for
/// `--dry-run`, `--self-test` and the unit tests. No network: each session talks
/// to it over a datagram socket pair inside this process.
///
/// It is written from the fleet session's public contract ("the wire as built")
/// and does only what that contract states:
///   - a session is opened, refreshed and closed by one org token
///   - every datagram's HMAC is checked with the fleet token; under the
///     session's own id only 29, 49, 50 and 26 are accepted
///   - a FLEET_CARRY's proof is checked with the player's token; past 64 a
///     second (burst max_players + 32) a carry is dropped unanswered; an older
///     one is a replay
///   - one carrier a player: the newest carry wins and the session that carried
///     before is sent FLEET_RELEASED reason 1
///   - 51, 52 and 53 go out as members of a signed bundle, or each with its own
///     HMAC, as the session's capabilities ask
///   - state goes out plain and once a session; text and voice go out inside a
///     FLEET_DELIVERY that lists who may receive them
/// It is a test double, and it says nothing about how the real servers work.
namespace lt::fleet {

class FakeRealtime {
public:
    struct Options {
        uint64_t seed = 7;
        int maxPlayers = 256;                   // FLEET_CARRY_RESULT status 4 past this
        int64_t playerTokenTtlMs = 30 * 60 * 1000;
        int64_t oldTokenGraceMs = 2000;         // a refreshed token's predecessor lives this long
        int64_t idleTimeoutMs = 0;              // FLEET_RELEASED reason 5 after this (0: never)
        bool carryRateLimit = true;             // drop carries past 64 a second, unanswered
        double dropToClient = 0.0;              // chance a datagram to a session is lost
        double dropToServer = 0.0;              // chance a datagram from a session is lost
        bool ignoreCapabilities = false;        // serve every session as if it advertised nothing
        bool framesAlone = false;               // a frame with its own HMAC goes alone, not in a bundle
        int mintBusyEvery = 0;                  // every Nth sign-in answers PLATFORM_BUSY first
        int mintRateLimitedEvery = 0;           // every Nth sign-in answers RATE_LIMITED first
        int64_t clockSkewMs = 0;                // the server's clock against this host's
        // Around a player's token refresh, as the dev tier answers:
        bool oldTokenCarryRefused = true;       // a FLEET_CARRY naming the token a refresh replaced is
                                                //   answered status 1; the carry that stands under it goes on
        bool strictTokenId = true;              // a message must name the token id the carry stands under:
                                                //   after a carry with the new token, the old id is refused
        int64_t newTokenLagMs = 0;              // a refreshed token is unknown to the realtime side for
                                                //   this long after the API answered (a carry: status 1)
        int64_t refreshAnswerDelayMs = 0;       // refreshAppToken's answer takes this long to come back,
                                                //   after the old token was replaced
        // Timing, for the tests of what crosses on the way.
        int64_t carryResultDelayMs = 0;         // hold each FLEET_CARRY_RESULT this long
        int64_t stateDelayMs = 0;               // hold each state notification this long before it goes out
        // Defects, for the tests that the client catches them.
        bool ownStateBug = false;               // send a session its own players' state unasked
        bool plainCommunicationBug = false;     // send a text plain as well as in a 53
        int corruptFrameHmacEvery = 0;          // every Nth frame's own HMAC is wrong
        int strayUnsignedEvery = 0;             // every Nth update is sent once more, alone in a datagram and
                                                //   with no HMAC, to one session: its sender (one time in
                                                //   four) or any session, in range or out of it
    };
    /// The org token that opens the sessions, and another valid one of the same
    /// organization that did not.
    static constexpr const char* ORG_TOKEN = "cko_dry_run_org_token";
    static constexpr const char* OTHER_ORG_TOKEN = "cko_dry_run_second_org_token";
    static constexpr int64_t ORG_ID = 77;

    FakeRealtime(int64_t appId, Options options);
    ~FakeRealtime();
    FakeRealtime(const FakeRealtime&) = delete;
    FakeRealtime& operator=(const FakeRealtime&) = delete;

    void start();
    void stop();

    /// The Application API, as an ApiTransport. Each call makes a new one.
    std::unique_ptr<ApiTransport> transport();
    /// Sockets for the workers: one end of a datagram socket pair.
    Connector connector();

    // Faults a test injects. `ordinal` is the order the sessions were opened in;
    // `player` is the n of fleet-load-<n>.
    void tellSessionToMove(int ordinal, bool sessionReleased, uint8_t reason);
    void sendLegacyReconnect(int ordinal);  // a valid 22, though the session's control is bound
    void deauthorize(int ordinal);          // the token must be refreshed; the carries stand
    void endSignIn(int player);             // FLEET_RELEASED reason 2 to whoever carries the player
    void idleRelease(int player);           // FLEET_RELEASED reason 5
    void setForeign(int player);            // carrying the player answers status 2
    /// Send a session a text: inside a FLEET_DELIVERY that lists the first player it
    /// carries, or (a defect) plain.
    void injectText(int ordinal, bool insideDelivery);

    struct Counters {
        std::atomic<uint64_t> datagramsIn{0};
        std::atomic<uint64_t> badHmac{0};
        std::atomic<uint64_t> carriesAccepted{0};
        std::atomic<uint64_t> carriesRefused{0};
        std::atomic<uint64_t> carriesRepeated{0};    // the same 49 again, answered as it was
        std::atomic<uint64_t> carriesReplayed{0};    // an older 49: status 3
        std::atomic<uint64_t> carriesDropped{0};     // past the carry rate: unanswered
        std::atomic<uint64_t> releasedSent{0};       // FLEET_RELEASED, any reason
        std::atomic<uint64_t> releasedReason1{0};
        std::atomic<uint64_t> releasedReason5{0};
        std::atomic<uint64_t> releaseRequests{0};    // FLEET_RELEASE received
        std::atomic<uint64_t> updatesAccepted{0};
        std::atomic<uint64_t> updatesUnauthorized{0};
        std::atomic<uint64_t> notificationsOut{0};
        std::atomic<uint64_t> ownStateOut{0};
        std::atomic<uint64_t> strayUnsigned{0};      // Options::strayUnsignedEvery
        std::atomic<uint64_t> strayUnsignedToSender{0};
        std::atomic<uint64_t> deliveriesOut{0};      // FLEET_DELIVERY frames
        std::atomic<uint64_t> deliveryPlayersOut{0};
        std::atomic<uint64_t> framesInSignedBundle{0};
        std::atomic<uint64_t> framesWithOwnHmac{0};
        std::atomic<uint64_t> framesAloneOversize{0}; // a 53 too large for a signed bundle
        std::atomic<uint64_t> datagramsOut{0};
        std::atomic<uint64_t> datagramsOutDropped{0};
        std::atomic<uint64_t> largestDatagramOut{0};
        std::atomic<uint64_t> capabilities{0};
        std::atomic<uint64_t> boundAcksSent{0};
        std::atomic<uint64_t> heartbeats{0};         // the session's own keepalive
        std::atomic<uint64_t> playerHeartbeats{0};   // under a carried player's id
        std::atomic<uint64_t> ownIdRefused{0};       // a message under the session's id that is not 29, 49, 50 or 26
        std::atomic<uint64_t> capabilitiesUnderPlayerId{0};
        std::atomic<uint64_t> actorRefused{0};       // a send under another player's actor
        std::atomic<uint64_t> voiceRefused{0};       // a minor's voice
        // The API's own count of the calls it answered, by kind.
        std::atomic<uint64_t> apiOpen{0};
        std::atomic<uint64_t> apiMint{0};
        std::atomic<uint64_t> apiClose{0};
        std::atomic<uint64_t> apiRefreshSession{0};
        std::atomic<uint64_t> apiRefreshSessionWithoutServer{0};
        std::atomic<uint64_t> apiRefreshAppToken{0};
        std::atomic<uint64_t> apiDelete{0};
        std::atomic<uint64_t> apiBlock{0};
        std::atomic<uint64_t> apiRefusedOtherOrgToken{0};
    };
    const Counters& counters() const { return counters_; }
    /// Every counter by its name, for a test that compares two moments.
    std::map<std::string, uint64_t> snapshot() const;
    uint64_t apiCalls() const;
    int carriedPlayers();
    int openSessions();
    int players();
    /// The session (ordinal) that carries fleet-load-<player>, or -1.
    int carrierOf(int player);

    // The API's operations (the transport calls these).
    FleetSessionInfo apiOpenSession(const std::string& orgToken);
    FleetSessionInfo apiRefreshSession(const std::string& orgToken, int64_t fleetId,
                                       bool namedServer);
    bool apiCloseSession(const std::string& orgToken, int64_t fleetId);
    PlayerToken apiMintPlayer(const std::string& externalId, int ageYears);
    PlayerToken apiRefreshPlayer(const std::string& token);
    bool apiDeletePlayer(const std::string& externalId);
    void apiBlockPlayer(const std::string& playerToken, int64_t blockedUserId);
    int64_t appId() const { return appId_; }
    const Options& options() const { return opt_; }

private:
    struct Session {
        int ordinal = 0;
        int64_t id = 0;
        std::string token;
        std::string openedBy;        // the org token that opened it: its one owner
        int fd = -1;                 // the server's end of the pair
        uint32_t flags = 0;
        uint8_t nonce[CONTROL_NONCE_LEN] = {};
        bool hasNonce = false;
        bool moved = false;          // told to move: deaf until the API refreshes it
        bool deauthorized = false;   // answers INVALID_TOKEN until the API refreshes it
        int port = 0;
        std::vector<int> carried;    // players
        std::unordered_map<int, int64_t> lastAcceptedMs; // by player: the time of its last accepted carry
        double carryTokens = 0;
        int64_t carryRefillNs = 0;
        std::vector<uint8_t> out;    // the bundle being built
        int members = 0;
        bool boxDirty = true;
        int64_t minX = 0, maxX = 0, minY = 0, maxY = 0;
        bool hasBox = false;
    };
    struct Player {
        int index = 0;
        std::string externalId;
        std::string token;
        int64_t tokenId = 0;
        int64_t expiresAtMs = 0;
        std::string oldToken;
        int64_t oldTokenId = 0;
        int64_t oldDeadMs = 0;
        int64_t tokenKnownAtMs = 0;  // the realtime side knows the newest token from this time
        bool foreign = false;
        bool ended = false;
        bool minor = false;          // under 18: voice is off
    };
    struct Carry {
        int64_t sessionId = 0;
        int64_t tokenId = 0;
        int64_t acceptedMs = 0;      // the time its FLEET_CARRY named
        int64_t cx = 0, cy = 0, cz = 0;
        bool placed = false;
        int64_t lastInputNs = 0;     // the idle clock
        uint64_t lastStateHash = 0;
        bool firstUpdatePending = false; // no update yet since this session's carry was accepted
    };
    /// Something held back for a while (Options::carryResultDelayMs, stateDelayMs).
    struct Held {
        int64_t dueNs = 0;
        bool frame = false;          // a fleet frame for `sessionId`; otherwise an update it sent
        int64_t sessionId = 0;
        int player = -1;
        uint8_t seq = 0;
        std::vector<uint8_t> bytes;
    };

    int connect(const FleetSessionInfo& info, std::string& error);
    void run();
    void onDatagram(Session& s, const uint8_t* d, size_t len);
    void onCapabilities(Session& s, const uint8_t* d, size_t len);
    void onCarry(Session& s, const uint8_t* d);
    void onUpdate(Session& s, int player, const uint8_t* d, size_t len, uint8_t seq);
    void fanOut(Session& s, int player, const uint8_t* d, size_t len, uint8_t seq);
    void deliverHeld(int64_t now);
    void sendCarryResult(Session& s, const uint8_t* frame);
    void onCommunication(Session& s, int player, const uint8_t* d, size_t len, uint8_t seq,
                         uint8_t notificationType);
    void queueMember(Session& s, const uint8_t* m, size_t len);
    void queueFleetFrame(Session& s, const uint8_t* frame, size_t len);
    void queueError(Session& s, uint8_t seq, uint8_t code);
    void flush(Session& s);
    void flushAll();
    void sendRaw(Session& s, const uint8_t* d, size_t len);
    void release(int player, uint8_t reason);
    void dropCarry(int player);
    void dropCarriesOf(Session& s);
    void sweepPlayers();
    Session* byOrdinal(int ordinal);
    int playerOf(int n);             // fleet-load-<n>'s index, or -1
    const std::string* liveToken(const Player& p, int64_t tokenId, int64_t nowMs) const;
    bool blocked(int a, int b) const;
    void refreshBox(Session& s);
    bool anyInRange(Session& s, int64_t cx, int64_t cy, int64_t cz, int distance, int except);
    std::string randomToken();
    double roll();
    int64_t serverNowMs() const;

    int64_t appId_;
    Options opt_;
    Counters counters_;
    std::mutex mu_;
    std::mt19937_64 rng_;
    std::unordered_map<int64_t, Session> sessions_;   // by fleet gameTokenId
    std::vector<int64_t> order_;                      // the order sessions were opened in
    std::unordered_map<int, int64_t> byFd_;
    std::unordered_map<int, Player> players_;         // by player index
    std::unordered_map<std::string, int> byExternalId_;
    std::unordered_map<int64_t, int> tokenOwner_;     // token id -> player index
    std::unordered_map<int, Carry> carries_;          // by player index
    std::unordered_map<std::string, int> actorOwner_; // actor uuid -> player index
    std::set<std::pair<int, int>> blocks_;            // (blocker, blocked)
    std::vector<int64_t> dirty_;                      // sessions with a bundle to flush
    std::deque<Held> held_;                           // in the order they fall due
    uint64_t updatesFannedOut_ = 0;
    int64_t nextFleetId_ = 7000000001LL;
    int64_t nextTokenId_ = 9000000001LL;
    int nextPlayer_ = 0;
    int nextPort_ = 40000;
    int mints_ = 0;
    uint64_t framesWithHmac_ = 0;
    int epollFd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace lt::fleet
