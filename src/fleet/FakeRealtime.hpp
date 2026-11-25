#pragma once

#include "fleet/FleetApi.hpp"
#include "fleet/FleetWorker.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/// An in-process stand-in for the Application API and the realtime servers, for
/// `--dry-run`, `--self-test` and the unit tests. No network: each session talks
/// to it over a datagram socket pair inside this process.
///
/// It is written from the fleet session's public contract and does only what
/// that contract states: it authorizes a fleet token when a session is opened,
/// checks every datagram's HMAC with the fleet token, checks a FLEET_CARRY's
/// proof with the player's token, keeps one carrier a player (the newest carry
/// wins, the session that carried before is sent FLEET_RELEASED reason 1),
/// refuses a message that names a player the session does not carry, and sends
/// an actor update once to each session that carries a player in range. It is
/// a test double, and it says nothing about how the real servers work.
namespace lt::fleet {

class FakeRealtime {
public:
    struct Options {
        uint64_t seed = 7;
        int maxPlayers = 256;                   // FLEET_CARRY_RESULT status 4 past this
        int64_t playerTokenTtlMs = 30 * 60 * 1000;
        int64_t oldTokenGraceMs = 2000;         // a refreshed token's predecessor lives this long
        double dropToClient = 0.0;              // chance a datagram to a session is lost
        double dropToServer = 0.0;              // chance a datagram from a session is lost
        bool ignoreCapabilities = false;        // never send signed bundles
        bool longFormFleetFrames = false;       // frame 51/52 as long-form app payloads
        bool ownStateBug = false;               // send a session its own players' state unasked
        int mintBusyEvery = 0;                  // every Nth sign-in answers PLATFORM_BUSY first
        int mintRateLimitedEvery = 0;           // every Nth sign-in answers RATE_LIMITED first
        int64_t clockSkewMs = 0;                // the server's clock against this host's
    };
    static constexpr const char* ORG_TOKEN = "cko_dry_run_org_token";

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

    // Faults a test injects. `ordinal` is the order the sessions were opened in.
    void tellSessionToMove(int ordinal, bool sessionReleased, uint8_t reason);
    void endSignIn(int player);     // FLEET_RELEASED reason 2 to whoever carries the player
    void idleRelease(int player);   // FLEET_RELEASED reason 5
    void setForeign(int player);    // carrying the player answers status 2

    struct Counters {
        std::atomic<uint64_t> datagramsIn{0};
        std::atomic<uint64_t> badHmac{0};
        std::atomic<uint64_t> carriesAccepted{0};
        std::atomic<uint64_t> carriesRefused{0};
        std::atomic<uint64_t> releasedSent{0};       // FLEET_RELEASED, any reason
        std::atomic<uint64_t> releasedReason1{0};
        std::atomic<uint64_t> releaseRequests{0};    // FLEET_RELEASE received
        std::atomic<uint64_t> updatesAccepted{0};
        std::atomic<uint64_t> updatesUnauthorized{0};
        std::atomic<uint64_t> notificationsOut{0};
        std::atomic<uint64_t> ownStateOut{0};
        std::atomic<uint64_t> datagramsOut{0};
        std::atomic<uint64_t> datagramsOutDropped{0};
        std::atomic<uint64_t> capabilities{0};
        std::atomic<uint64_t> heartbeats{0};
        // The API's own count of the calls it answered, by kind.
        std::atomic<uint64_t> apiOpen{0};
        std::atomic<uint64_t> apiMint{0};
        std::atomic<uint64_t> apiClose{0};
        std::atomic<uint64_t> apiRefreshSession{0};
        std::atomic<uint64_t> apiRefreshSessionWithoutServer{0};
        std::atomic<uint64_t> apiRefreshAppToken{0};
        std::atomic<uint64_t> apiBadBearer{0};
        std::atomic<uint64_t> apiOriginHeader{0};
    };
    const Counters& counters() const { return counters_; }
    uint64_t apiCalls() const;
    int carriedPlayers();
    int openSessions();
    /// The session (ordinal) that carries `player`, or -1.
    int carrierOf(int player);

    // The API's operations (FakeTransport calls these).
    FleetSessionInfo apiOpenSession();
    FleetSessionInfo apiRefreshSession(int64_t fleetId, bool namedServer);
    bool apiCloseSession(int64_t fleetId);
    PlayerToken apiMintPlayer(int n);
    PlayerToken apiRefreshPlayer(const std::string& token);
    int64_t appId() const { return appId_; }

private:
    struct Session {
        int ordinal = 0;
        int64_t id = 0;
        std::string token;
        int fd = -1;                 // the server's end of the pair
        uint32_t flags = 0;
        bool moved = false;          // told to move: deaf until the API refreshes it
        int port = 0;
        std::vector<int> carried;    // player numbers
        std::vector<uint8_t> out;    // the bundle being built
        int members = 0;
        bool boxDirty = true;
        int64_t minX = 0, maxX = 0, minY = 0, maxY = 0;
        bool hasBox = false;
    };
    struct Player {
        int n = 0;
        std::string token;
        int64_t tokenId = 0;
        int64_t expiresAtMs = 0;
        std::string oldToken;
        int64_t oldTokenId = 0;
        int64_t oldDeadMs = 0;
        bool foreign = false;
        bool ended = false;
    };
    struct Carry {
        int64_t sessionId = 0;
        int64_t tokenId = 0;
        int64_t cx = 0, cy = 0, cz = 0;
        bool placed = false;
    };

    int connect(const FleetSessionInfo& info, std::string& error);
    void run();
    void onDatagram(Session& s, const uint8_t* d, size_t len);
    void onCarry(Session& s, const uint8_t* d, size_t len);
    void onUpdate(Session& s, const uint8_t* d, size_t len, int64_t tailId, uint8_t seq);
    void queueMember(Session& s, const uint8_t* m, size_t len);
    void queueFleetFrame(Session& s, uint8_t type, const uint8_t* payload, size_t len);
    void queueError(Session& s, uint8_t seq, uint8_t code);
    void flush(Session& s);
    void flushAll();
    void sendRaw(Session& s, const uint8_t* d, size_t len);
    void release(int player, uint8_t reason);
    void dropCarry(int player);
    void sweepTokens();
    const std::string* liveToken(const Player& p, int64_t tokenId, int64_t nowMs) const;
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
    std::unordered_map<int, Player> players_;         // by player number
    std::unordered_map<int64_t, int> tokenOwner_;     // token id -> player number
    std::unordered_map<int, Carry> carries_;          // by player number
    std::vector<int64_t> dirty_;                      // sessions with a bundle to flush
    int64_t nextFleetId_ = 7000000001LL;
    int64_t nextTokenId_ = 9000000001LL;
    int nextPort_ = 40000;
    int mints_ = 0;
    int epollFd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace lt::fleet
