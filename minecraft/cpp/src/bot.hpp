#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ckmc {

struct BotSpec {
  std::string name;
  std::string host;
  int port = 25565;
  // seam (east/west shuttle), triangle, or wander.
  std::string path = "seam";
  // "x,y,z,side;..." for triangle. The y is not a fly height.
  std::string waypoints;
  double bps = 2.0;
  double clearance = 8.0;
  int maxSeconds = 180;
  std::string logPath;
};

// One Minecraft 26.3 TCP client. The worker thread calls the socket and tick methods.
class Bot {
 public:
  explicit Bot(BotSpec spec);
  ~Bot();

  Bot(const Bot&) = delete;
  Bot& operator=(const Bot&) = delete;

  int fd = -1;
  bool dead = false;
  std::string name() const { return spec_.name; }

  // Nonblocking connect. False means the bot is already dead (and has logged why).
  bool start();
  void onReadable();
  void onWritable();
  bool wantsWrite() const { return !out_.empty() || !established_; }
  void tick();
  void stop(const std::string& why);

 private:
  enum class Phase { Login, Config, Play };

  struct Waypoint {
    double x = 0;
    double z = 0;
    std::string side;
  };

  BotSpec spec_;
  Phase phase_ = Phase::Login;
  bool established_ = false;
  int compression_ = -1;
  std::vector<uint8_t> in_;
  std::vector<uint8_t> out_;
  std::vector<std::pair<std::string, std::string>> cookies_;

  bool placed_ = false;
  bool transfer_ = false;
  bool sentAbilities_ = false;
  int ticks_ = 0;
  int maxTicks_ = 3600;
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
  double serverX_ = 0;
  double progressX_ = 0;
  bool progressSet_ = false;
  float yaw_ = -90.f;
  float pitch_ = 8.f;
  double wanderHeading_ = 0;
  int waypointIndex_ = 0;
  std::vector<Waypoint> waypoints_;
  std::FILE* log_ = nullptr;

  void say(const std::string& line);
  void send(int packetId, const std::vector<uint8_t>& body);
  void sendEmpty(int packetId);
  void enterPlay();
  void beginConfigFromPlay();
  void handle(int packetId, const uint8_t* body, size_t len);
  void handleLogin(int packetId, const uint8_t* body, size_t len);
  void handleConfig(int packetId, const uint8_t* body, size_t len);
  void handlePlay(int packetId, const uint8_t* body, size_t len);
  void onPosition(const uint8_t* body, size_t len);
  void sendFlyPose(bool withInput);
  void tickMotion();
  void sendAbilities();
  void fail(const std::string& why);
  void parseWaypoints();
};

}  // namespace ckmc
