#pragma once

#include "Wire.hpp"
#include "fleet/FleetApi.hpp"
#include "fleet/FleetConfig.hpp"
#include "fleet/FleetStats.hpp"
#include "fleet/FleetWire.hpp"
#include "fleet/Grid.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/// The realtime side of the fleet load client.
///
/// A worker thread owns some of the sessions (one UDP socket each) and every
/// player who stands in those sessions' regions. A player belongs to exactly one
/// worker at a time: when it crosses into a region another worker owns, the
/// player moves to that worker through its mailbox. So per-player state needs no
/// lock. The only state two threads touch is a player's credentials (written by
/// an API thread, read here through a seqlock) and the counters.
///
/// A worker holds no handle to the Application API. What it needs from the API
/// (a session told to move, a player whose sign-in ended) it asks for through
/// `requestApi`, and the code that handles a crossing never calls it.
namespace lt::fleet {

/// Open the datagram socket of a session: returns a connected, non-blocking fd,
/// or -1 with `error` set.
using Connector = std::function<int(const FleetSessionInfo&, std::string& error)>;

/// A UDP socket connected to the session's realtime server.
int connectUdp(const FleetSessionInfo& info, int rcvbufBytes, std::string& error);

/// A player's app token, shared between the API threads (one writer at a time,
/// held by `busy`) and the worker that carries the player (readers). A seqlock
/// over atomics: no lock, and no torn read.
class SharedCreds {
public:
    /// Odd while a write is in progress. 0 until the first token is stored.
    uint32_t version() const { return version_.load(std::memory_order_acquire); }
    int64_t expiresAtMs() const { return expiresAtMs_.load(std::memory_order_relaxed); }
    void store(const PlayerToken& t);
    /// Copies the token (64 octets), its id and the version it was read at.
    /// False when no token has been stored yet.
    bool load(uint8_t* token64, int64_t& gameTokenId, uint32_t& version) const;
    PlayerToken snapshot() const;

    /// Set while an API call for this player is in flight, so there is one writer.
    std::atomic<bool> busy{false};

private:
    std::atomic<uint32_t> version_{0};
    std::atomic<uint64_t> words_[8]{};
    std::atomic<int64_t> gameTokenId_{0};
    std::atomic<int64_t> expiresAtMs_{0};
};

/// What a worker asks the API threads for.
struct ApiRequest {
    enum class Kind : uint8_t { MoveSession, SignInAgain } kind = Kind::MoveSession;
    int index = 0;               // session or player
    ServerAddr current;          // MoveSession: the server the session was on
    int64_t gameTokenId = 0;     // MoveSession: the session's id
    bool fromCrossingPath = false;
};

/// State every worker reads.
struct World {
    World(const FleetConfig& cfg, FleetStats& stats, int workers);

    const FleetConfig& cfg;
    FleetStats& stats;
    Grid grid;
    int workers;
    std::vector<SharedCreds> creds;          // by player
    std::atomic<double> speed{0.0};          // chunks a second
    std::atomic<bool> moving{true};          // false while the run drains
    uint16_t runTag;                         // marks this run's actor updates
    int sampleStride;                        // every Nth player's updates carry a send time

    int workerOf(int session) const {
        return static_cast<int>(static_cast<int64_t>(session) * workers / cfg.servers);
    }
    bool sampled(int player) const {
        return sampleStride > 0 && player % sampleStride == 0 &&
               player / sampleStride < cfg.latencySample;
    }
    /// The player's actor UUID: 32 hex characters, the same in every run.
    void actorUuid(int player, char* out32) const;
};

class FleetWorker {
public:
    FleetWorker(int index, World& world, Connector connect,
                std::function<void(const ApiRequest&)> requestApi);
    ~FleetWorker();
    FleetWorker(const FleetWorker&) = delete;
    FleetWorker& operator=(const FleetWorker&) = delete;

    void setPeers(std::vector<std::unique_ptr<FleetWorker>>* peers) { peers_ = peers; }
    void start();
    void stop(); // signal and join

    // Thread-safe: delivered to the worker's mailbox.
    void openSession(int session, const FleetSessionInfo& info);
    void sessionMoved(int session, const FleetSessionInfo& info, bool ok);
    void admitPlayer(int player, const Walker& start);
    /// The players leave the game: FLEET_RELEASE for each carried player.
    void releaseAll();

    // Gauges, refreshed every 100 ms by the worker.
    int players() const { return gPlayers_.load(std::memory_order_relaxed); }
    int carried() const { return gCarried_.load(std::memory_order_relaxed); }
    int pending() const { return gPending_.load(std::memory_order_relaxed); }
    int awaitingRelease() const { return gAwaiting_.load(std::memory_order_relaxed); }
    int activeSessions() const { return gActive_.load(std::memory_order_relaxed); }

    /// The state the worker ended in. Call after stop().
    struct End {
        int players = 0;
        int carried = 0;
        int notCarried = 0;
        int failed = 0;
        int crossingsOpen = 0;
        int releasesPending = 0;
    };
    End end() const;
    /// The sessions this worker owns, as last known. Call after stop().
    std::vector<FleetSessionInfo> sessionInfos() const;

private:
    struct Carry {
        bool active = false;
        CarryCause cause = CarryCause::Join;
        int64_t id = 0;              // the player's gameTokenId this carry names
        uint32_t version = 0;        // the credentials' version it was built from
        uint8_t token[hmac::TOKEN_OCTETS] = {};
        int64_t firstSentNs = 0;
        int64_t lastSentNs = 0;
    };

    struct Player {
        int index = 0;
        bool sampled = false;
        char uuid[32] = {};
        Walker walk;
        std::mt19937_64 rng;

        bool carried = false;        // this session's carry was confirmed
        int64_t curId = 0;           // the gameTokenId its messages name
        uint32_t curVersion = 0;
        Carry carry;
        int64_t retryAtNs = 0;       // start a carry again at this time
        CarryCause retryCause = CarryCause::Join;
        bool waitToken = false;      // waiting for the API threads to store a new token
        uint32_t waitVersion = 0;
        int64_t waitSinceNs = 0;
        bool failed = false;         // refused for good

        // The crossing in progress, and the last session that carried the player.
        bool crossingOpen = false;
        int64_t crossStartNs = 0;
        uint32_t crossingSeq = 0;
        int originSession = -1;
        int64_t originId = 0;
        uint32_t originSeq = 0;

        int64_t nextUpdateNs = 0;
        int64_t lastMoveNs = 0;
        int64_t carriedSinceNs = 0;
        int64_t permCenterX = 0, permCenterY = 0;
        int64_t permReloadNs = 0;
        int unauthStreak = 0;
        bool released = false;       // FLEET_RELEASE was sent at the end of the run

        uint8_t msg[wire::ACTOR_UPDATE_MAX_SIZE] = {};
    };

    /// A player that left this session, whose FLEET_RELEASED reason 1 is awaited.
    struct Departed {
        int player = 0;
        uint32_t crossingSeq = 0;
        int64_t departNs = 0;
        int64_t resultNs = 0;        // when the new session's carry result arrived
        int64_t releasedNs = 0;      // when FLEET_RELEASED reason 1 arrived here
        bool late = false;           // made by a carry result that arrived after the player left
    };

    enum class SendKind : uint8_t { None, Update, Carry, Release, Capabilities, Heartbeat };
    struct SeqEntry {
        int64_t id = 0;
        int64_t sentNs = 0;
        SendKind kind = SendKind::None;
    };

    struct Session {
        enum class State : uint8_t { Unopened, Active, Moving } state = State::Unopened;
        int index = 0;               // the region and the global session number
        FleetSessionInfo info;
        uint8_t token[hmac::TOKEN_OCTETS] = {};
        int fd = -1;
        uint8_t seq = 0;
        int64_t activateAtNs = 0;    // no datagram before this (the API's authorization settles)
        bool needCarryAll = false;   // carry every held player once active
        int64_t lastCapsNs = 0;
        int64_t lastHeartbeatNs = 0;
        int64_t lastRxNs = 0;
        int64_t lastMoveNs = 0;
        int64_t moveRetryAtNs = 0;
        bool movePending = false;    // told to move while the last move was settling
        MoveTrigger pendingTrigger = MoveTrigger::ReconnectCommand;
        int errStreak = 0;
        int sendErrStreak = 0;
        std::vector<std::unique_ptr<Player>> players;
        std::unordered_map<int64_t, Player*> byId;
        std::unordered_map<int64_t, Departed> departed;
        SeqEntry ring[256];
        SessionIo* io = nullptr;
    };

    struct Mail {
        enum class Kind : uint8_t { OpenSession, SessionMoved, Arrive, CrossingDone, ReleaseAll };
        Kind kind = Kind::OpenSession;
        int session = -1;
        FleetSessionInfo info;
        bool ok = true;
        std::unique_ptr<Player> player;
        CarryCause cause = CarryCause::Join;
        int64_t playerId = 0;
        uint32_t crossingSeq = 0;
        int64_t resultNs = 0;
    };

    void run();
    void post(Mail&& m);
    void drainMail(int64_t now);
    Session* local(int session);

    void activate(Session& s, const FleetSessionInfo& info, int64_t now);
    void closeSocket(Session& s);
    void tickSession(Session& s, int64_t now);
    void tickCarry(Session& s, Player& p, int64_t now);
    void sweep(int64_t now);
    void drainSocket(Session& s);

    bool sendOn(Session& s, const uint8_t* buf, size_t len, SendKind kind, int64_t id, int64_t now);
    void sendCapabilities(Session& s, int64_t now);
    void sendHeartbeat(Session& s, int64_t now);
    void sendCarry(Session& s, Player& p, int64_t now);
    void sendUpdate(Session& s, Player& p, int64_t now, int64_t scheduledNs);
    void startCarry(Session& s, Player& p, CarryCause cause, int64_t now);
    void endCarryEpisode(Session& s, Player& p);
    void adopt(Session& s, std::unique_ptr<Player> p, CarryCause cause, int64_t now);
    void depart(Session& s, size_t slot, int toRegion, int64_t now);
    void deliver(std::unique_ptr<Player> p, int toSession, CarryCause cause, int64_t now);
    void requestSignInAgain(Player& p, int64_t now);
    void requestMove(Session& s, MoveTrigger trigger, int64_t now);
    void requestApi(const ApiRequest& r);

    void handleDatagram(Session& s, const uint8_t* data, size_t len, int64_t now);
    void handleMessage(Session& s, const wire::InboundView& m, bool trusted, int64_t now);
    void onCarryResult(Session& s, const CarryResult& r, int64_t now);
    void onReleased(Session& s, const Released& r, int64_t now);
    void onError(Session& s, const wire::InboundView& m, int64_t now);
    void onActorNotification(Session& s, const wire::InboundView& m, int64_t now);
    void onCrossingDone(Session& s, int64_t playerId, uint32_t crossingSeq, int64_t resultNs);
    void pairRelease(Session& s, std::unordered_map<int64_t, Departed>::iterator it);

    int index_;
    World& world_;
    Connector connect_;
    std::function<void(const ApiRequest&)> requestApi_;
    std::vector<std::unique_ptr<FleetWorker>>* peers_ = nullptr;

    std::vector<Session> sessions_;
    std::vector<int> localOf_;       // global session -> index in sessions_, or -1
    int epollFd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int64_t lastSweepNs_ = 0;
    int64_t updateIntervalNs_ = 0;

    std::mutex mailMu_;
    std::vector<Mail> mail_;

    std::atomic<int> gPlayers_{0};
    std::atomic<int> gCarried_{0};
    std::atomic<int> gPending_{0};
    std::atomic<int> gAwaiting_{0};
    std::atomic<int> gActive_{0};
};

} // namespace lt::fleet
