#pragma once

#include "logic.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ckmc {

class LogSink;

struct BotSpec {
  std::string name;
  std::string host;
  int port = 25565;
  // seam (east/west shuttle), triangle, or wander.
  std::string path = "seam";
  // "x,y,z,side;..." for triangle. The y is not a fly height.
  // "minX,minZ,maxX,maxZ" for wander: this bot's own box.
  std::string waypoints;
  double bps = 2.0;
  double clearance = 8.0;
  int maxSeconds = 180;
  std::string logPath;
};

// Settings every bot of one worker reads. The worker owns the struct and changes it only
// between ticks, from the commands the control threads hand it.
struct Shared {
  // `bounds`: the box of every bot that has none of its own.
  WanderBox box;
  // `speed`: wander speed for every bot, in blocks per second.
  bool speedOn = false;
  double bps = 0;
  // `altitude`: every bot flies at this feet Y.
  bool altitudeOn = false;
  double altitude = 0;
  // `grid`. gridGen changes with it, so a bot forgets the cell it had in the old grid.
  Grid grid;
  uint32_t gridGen = 0;
};

enum class BotPhase : uint8_t { Connecting = 0, Login = 1, Config = 2, Play = 3, Closed = 4 };

// What a control thread may read about one bot while its worker runs it.
struct BotView {
  // Set before the worker sees the bot, never changed.
  std::string name;
  uint64_t seq = 0;
  std::atomic<uint8_t> phase{static_cast<uint8_t>(BotPhase::Connecting)};
  std::atomic<bool> placed{false};
  // Written once by the worker, before `phase` becomes Closed. Read it only after that.
  std::string reason;
};

// Totals for the life of the process.
struct Counters {
  std::atomic<uint64_t> transfers{0};
  std::atomic<uint64_t> rejoins{0};
  std::atomic<uint64_t> deaths{0};
  std::atomic<uint64_t> respawns{0};
  std::atomic<uint64_t> crossings{0};
};

Counters& counters();

// ServerboundClientCommandPacket: one VarInt, the action. 0 is "perform respawn".
inline constexpr int kPerformRespawn = 0;
std::vector<uint8_t> clientCommandBody(int action);

// One Minecraft 26.3 TCP client. One worker thread owns it and calls everything here.
class Bot {
 public:
  explicit Bot(BotSpec spec, const Shared* shared = nullptr, std::shared_ptr<BotView> view = nullptr);
  ~Bot();

  Bot(const Bot&) = delete;
  Bot& operator=(const Bot&) = delete;

  int fd = -1;
  bool dead = false;
  // The worker's note of what it last registered for this bot. -1 after the socket closes.
  int armedFd = -1;
  bool armedWrite = false;

  std::string name() const { return spec_.name; }
  uint64_t seq() const { return view_ ? view_->seq : 0; }

  // Nonblocking connect. False means no socket is open: the bot is dead, or it is waiting to rejoin.
  bool start();
  void onReadable();
  void onWritable();
  // The wait loop saw an error or a hangup on the socket.
  void onSocketError();
  bool wantsWrite() const { return !out_.empty() || !established_; }
  void tick();
  // Ends the bot for good. `why` is what `list closed` shows.
  void stop(const std::string& why);

  void setRally(double x, double z, double radius);
  void clearRally();

 private:
  enum class Phase { Login, Config, Play };

  struct Waypoint {
    double x = 0;
    double z = 0;
    std::string side;
  };

  struct Rally {
    bool active = false;
    bool arrived = false;
    double x = 0;
    double z = 0;
    double radius = 0;
    WanderBox box;
  };

  BotSpec spec_;
  // Where `add` pointed. A transfer changes spec_.host; a rejoin goes back here.
  std::string origHost_;
  int origPort_ = 0;
  const Shared* shared_;
  std::shared_ptr<BotView> view_;
  Rng rng_;
  std::shared_ptr<LogSink> sink_;

  Phase phase_ = Phase::Login;
  // Counts sockets closed. It tells one connection from the next when the fd number repeats.
  uint64_t connGen_ = 0;
  bool established_ = false;
  int compression_ = -1;
  std::vector<uint8_t> in_;
  std::vector<uint8_t> out_;
  std::vector<std::pair<std::string, std::string>> cookies_;

  // The server has sent a position and the bot is flying.
  bool placed_ = false;
  // Placed at least once on this connection, and on any connection.
  bool connPlaced_ = false;
  bool everPlaced_ = false;
  int placedAtTick_ = 0;
  // Handshake next-state: 2 is a fresh login, 3 is a transfer (the client is following a move).
  int handshakeState_ = 2;
  int transfersFollowed_ = 0;
  int transferRetries_ = 0;
  // Rejoins used since the last 120 s of play, and the tick the next one starts (-1: none waiting).
  int rejoinAttempts_ = 0;
  int rejoinAtTick_ = -1;
  int rejoins_ = 0;
  bool sentAbilities_ = false;
  int ticks_ = 0;
  int maxTicks_ = 3600;
  int statusOffset_ = 0;
  bool statusNow_ = false;

  // Survival. awaitingRespawn_ is set from the death until the bot is placed again.
  bool awaitingRespawn_ = false;
  bool respawnSeen_ = false;
  bool posSinceDeath_ = false;
  int respawnAskTick_ = 0;
  // Ticks left to wait for a position after a respawn packet. 0: not waiting.
  int waitPosTicks_ = 0;
  bool needLoaded_ = false;
  bool haveEntityId_ = false;
  int32_t entityId_ = 0;
  int deaths_ = 0;

  // Line crossings.
  CellTracker cells_;
  uint32_t cellGen_ = 0;
  int crossings_ = 0;
  int lastCrossLogTick_ = -1000000;

  int hold_ = 0;
  int pauseSide_ = 0;
  std::string pauseLabel_;
  int eastPauses_ = 0;
  int westPauses_ = 0;
  int northPauses_ = 0;
  int direction_ = 1;
  int stallTicks_ = 0;
  int stuck_ = 0;
  bool escaping_ = false;
  double laneZ_ = 0;
  double x_ = 0;
  double y_ = 0;
  double z_ = 0;
  double surfaceY_ = 0;
  // Set while `altitude` is on, and afterwards until the bot is back at the clearance height.
  bool ramping_ = false;
  double serverX_ = 0;
  double progressX_ = 0;
  bool progressSet_ = false;
  float yaw_ = -90.f;
  float pitch_ = 8.f;
  Wander wander_;
  WanderBox box_{};
  Rally rally_;
  int waypointIndex_ = 0;
  std::vector<Waypoint> waypoints_;

  BotPhase pubPhase_ = BotPhase::Connecting;
  bool pubPlaced_ = false;

  void say(const std::string& line);
  void send(int packetId, const std::vector<uint8_t>& body);
  void sendEmpty(int packetId);
  void enterPlay();
  void beginConfigFromPlay();
  // The server could not keep this connection and named the server to join. Reconnect there,
  // keeping the cookies the last server stored, the way a game client follows a transfer.
  void followTransfer(const std::string& host, uint32_t port);
  void reconnect(const std::string& host, int port, int handshakeState);
  void closeSocket();
  void resetConnection();
  void handle(int packetId, const uint8_t* body, size_t len);
  void handleLogin(int packetId, const uint8_t* body, size_t len);
  void handleConfig(int packetId, const uint8_t* body, size_t len);
  void handlePlay(int packetId, const uint8_t* body, size_t len);
  void onPosition(const uint8_t* body, size_t len);
  void onDeath();
  void onHealth(float health);
  void onRespawnPacket();
  // The bot has a position to fly from. Finishes a respawn when one was pending.
  void place();
  void readSocket();
  void writeSocket();
  void flushOut();
  // After any entry point: push out what was queued, then publish the phase.
  void settle();
  void publish();
  void sendFlyPose(bool withInput);
  void tickMotion();
  void tickWander();
  void updateY();
  void noteCell();
  void sayStatus();
  double wanderStep() const;
  const WanderBox& activeBox() const;
  void sendAbilities();
  // Ends the bot and logs `closed: why`.
  void fail(const std::string& why);
  // The connection is gone. Schedules a rejoin when the bot has been placed and has attempts left.
  void lost(const std::string& why);
  void parseWaypoints();
  void parseWanderBox();
};

}  // namespace ckmc
