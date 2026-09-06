#pragma once

#include "Config.hpp"
#include "Provisioner.hpp"
#include "Wire.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <thread>

namespace lt {

/// One simulated game client: a random 2D walk around the origin, an actor
/// UUID, an app token, and a pre-built signed actor-update message.
/// Owned exclusively by one worker thread; no internal locking.
struct SimClient {
    enum class State : uint8_t {
        WAITING,    // provisioned, not yet activated (ramp / settle delay)
        ACTIVE,     // sending updates
        SUSPENDED,  // waiting on the control thread (reassign / recover)
    };

    ClientCredentials creds;
    State state = State::WAITING;
    std::chrono::steady_clock::time_point activateAt{};

    int fd = -1;

    // Movement simulation, in the pose profile's position units, on three world
    // axes. Which axis is "up" is the profile's: UE5 is Z-up, BWF (three.js) is
    // Y-up. The 2D walk moves on the two horizontal axes; the cube drifts on all
    // three. Chunk coordinates are floor(world / chunk size) per axis.
    //
    // UE5 stands players 60 units below the chunk origin (the legacy HEIGHT_OFFSET
    // this harness has always sent); BWF's playable terrain is chunk layers 0-2,
    // so a 2D BWF walk hovers at 20 blocks and the cube stands on volumeBaseUp.
    static constexpr double UE5_HEIGHT_OFFSET = -60.0;
    static constexpr double BWF_WALK_HEIGHT = 20.0;
    double worldX = 0, worldY = 0, worldZ = 0;
    double dirX = 1, dirY = 0, dirZ = 0;
    int64_t chunkX = 0, chunkY = 0, chunkZ = 0;
    double posX = 0, posY = 0, posZ = 0;
    double velX = 0, velY = 0, velZ = 0;
    double rotYawDeg = 0;
    // Cube mode: seconds until the next random re-aim, so the population keeps
    // mixing across chunks instead of settling into parallel tracks.
    double reaimAt = 0;

    char uuid[32] = {};
    uint8_t sequence = 0;
    double lastSendTime = 0;
    double lastMoveTime = 0;
    // When this client last received ANY datagram on its current assignment
    // (steady seconds). Reset on activation; see Config::rxSilentReassignSec.
    double lastRxTime = 0;

    // Set when a refresh request is in flight so we only ask once.
    bool refreshRequested = false;
    // Consecutive send() failures (e.g. ICMP unreachable after a Buddy
    // restart); triggers a reassignment past a threshold.
    int consecutiveSendErrors = 0;
    // First server error is logged once per client (diagnosability).
    bool errorLogged = false;
    // Accumulated UNAUTHORIZED replies since the last (re)assignment. A few
    // are normal (the server lazily loads permission windows); a persistent
    // stream means this session is wedged and needs a fresh assignment.
    int unauthorizedCount = 0;
    // When this client last became ACTIVE on an assignment. UNAUTHORIZED
    // refusals arriving shortly after are Buddy loading the permission window,
    // which the first packet triggers; the load spans several send intervals,
    // so the expected count per assignment is a handful rather than exactly
    // one (measured: 137 refusals across 100 clients at 10 Hz).
    std::chrono::steady_clock::time_point activatedAt{};

    // Where the server's cached grid-permission box is centred, as far as this
    // harness can model it: the chunk occupied when the last lookup ran. Set on
    // activation and re-centred whenever a refusal triggers a re-query.
    int64_t permWindowCenterX = 0;
    int64_t permWindowCenterY = 0;
    int64_t permWindowCenterZ = 0;
    bool permWindowValid = false;
    // When the last window-crossing refusal re-centred the box. Buddy evicts a
    // stale window under a backoff and the re-query spans several send
    // intervals, so one crossing produces an EPISODE of refusals rather than
    // one -- its own logging calls them denial episodes. Without this the
    // follow-on refusals look like they arrived inside the box.
    std::chrono::steady_clock::time_point permReloadAt{};

    uint8_t message[wire::ACTOR_UPDATE_MAX_SIZE] = {};

    static std::mt19937_64& rng() {
        thread_local std::mt19937_64 gen(
            std::chrono::high_resolution_clock::now().time_since_epoch().count() ^
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
        return gen;
    }

    void generateUuid() {
        static const char* hex = "0123456789ABCDEF";
        std::uniform_int_distribution<uint64_t> dist;
        uint64_t a = dist(rng()), b = dist(rng());
        for (int i = 0; i < 16; ++i) {
            uuid[i] = hex[(a >> ((15 - i) * 4)) & 0xF];
            uuid[i + 16] = hex[(b >> ((15 - i) * 4)) & 0xF];
        }
    }

    static wire::PoseFormat poseFormat(const Config& cfg) {
        return cfg.isBwf() ? wire::PoseFormat::BWF : wire::PoseFormat::UE5;
    }

    /// The horizontal axes and the vertical one, as references into the world
    /// position, so the walk and the cube read the same for both profiles.
    struct Axes {
        double& h1;   ///< first horizontal world axis
        double& h2;   ///< second horizontal world axis
        double& up;   ///< vertical world axis
        double& d1;
        double& d2;
        double& dup;
    };
    Axes axes(const Config& cfg) {
        // UE5: X, Y horizontal, Z up. BWF: X, Z horizontal, Y up.
        if (cfg.isBwf()) return Axes{worldX, worldZ, worldY, dirX, dirZ, dirY};
        return Axes{worldX, worldY, worldZ, dirX, dirY, dirZ};
    }

    /// Random unit direction on the horizontal plane (2D walk).
    void randomizeDirection(const Config& cfg) {
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        double x, y;
        do {
            x = dist(rng());
            y = dist(rng());
        } while (std::sqrt(x * x + y * y) < 0.0001);
        double len = std::sqrt(x * x + y * y);
        Axes a = axes(cfg);
        a.d1 = x / len;
        a.d2 = y / len;
        a.dup = 0;
    }

    /// Random unit direction in 3D (cube drift).
    void randomizeDirection3D() {
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        double x, y, z;
        do {
            x = dist(rng());
            y = dist(rng());
            z = dist(rng());
        } while (std::sqrt(x * x + y * y + z * z) < 0.0001);
        double len = std::sqrt(x * x + y * y + z * z);
        dirX = x / len;
        dirY = y / len;
        dirZ = z / len;
    }

    /// The cube's lowest chunk on each of (h1, up, h2): centred on the origin
    /// chunk horizontally (BWF's spawn chunk is (0, 0)), standing on volumeBaseUp.
    static int64_t cubeMinHorizontal(const Config& cfg) {
        return -static_cast<int64_t>(cfg.volumeChunks / 2);
    }

    /// Place a client in its cube slot: uniform by GLOBAL client index so a fleet
    /// of generators fills the volume together rather than each piling into the
    /// same corner, then jittered inside the chunk.
    void placeInCube(const Config& cfg) {
        const int64_t n = cfg.volumeChunks;
        const int64_t slots = n * n * n;
        const int64_t raw = ((static_cast<int64_t>(creds.index) % slots) + slots) % slots;
        // A PERMUTATION of the slot index before the mixed-radix split. Plain
        // mixed radix fills one horizontal slice before it starts the next, so a
        // SMALL population (60 bots on dev) stood as a wall at one edge of the
        // cube and the minimap showed a half-empty square. Multiplying by
        // 1 + n + n^2 -- congruent to 1 mod n, so coprime to n^3 -- walks the
        // diagonal first and spreads every axis from the first handful of clients,
        // while consecutive indices still land in distinct chunks.
        const int64_t idx = (raw * (1 + n + n * n)) % slots;
        const int64_t i1 = idx % n;
        const int64_t iup = (idx / n) % n;
        const int64_t i2 = (idx / (n * n)) % n;
        std::uniform_real_distribution<double> jitter(0.05, 0.95);
        const double size = cfg.effectiveChunkSize();
        Axes a = axes(cfg);
        a.h1 = (static_cast<double>(cubeMinHorizontal(cfg) + i1) + jitter(rng())) * size;
        a.h2 = (static_cast<double>(cubeMinHorizontal(cfg) + i2) + jitter(rng())) * size;
        a.up = (static_cast<double>(cfg.volumeBaseUp + iup) + jitter(rng())) * size;
    }

    /// Initialize walk state and the message template. Called once when the
    /// client first activates (UUID is stable across reassignments).
    void initSimulation(const Config& cfg, double nowSec) {
        const double size = cfg.effectiveChunkSize();
        if (cfg.volumeChunks > 0) {
            placeInCube(cfg);
            randomizeDirection3D();
            std::uniform_real_distribution<double> re(4.0, 12.0);
            reaimAt = nowSec + re(rng());
        } else {
            std::uniform_int_distribution<int64_t> chunkDist(-cfg.spawnRadiusChunks,
                                                             cfg.spawnRadiusChunks);
            Axes a = axes(cfg);
            a.h1 = static_cast<double>(chunkDist(rng())) * size;
            a.h2 = static_cast<double>(chunkDist(rng())) * size;
            a.up = cfg.isBwf() ? BWF_WALK_HEIGHT : 0.0;
            randomizeDirection(cfg);
        }
        generateUuid();
        lastMoveTime = nowSec;
        // Stagger the first send inside one update interval so a batch of
        // clients doesn't burst-send on the same tick.
        double interval = 1.0 / cfg.updateHz;
        std::uniform_real_distribution<double> off(0.0, 1.0);
        lastSendTime = nowSec - interval + off(rng()) * interval;
        sequence = 0;
        rebuildTemplate(cfg);
        // Derive chunk and pose fields once so the first send is consistent.
        projectPose(cfg);
    }

    /// (Re)build the static message fields; needed at init and whenever the
    /// gameTokenId changes (token refresh).
    void rebuildTemplate(const Config& cfg) {
        wire::initActorUpdateTemplateFmt(message, poseFormat(cfg), uuid, cfg.appId,
                                         creds.gameTokenId,
                                         static_cast<uint8_t>(cfg.distance),
                                         static_cast<wire::DecayRate>(cfg.decay));
    }

    /// World position -> chunk coordinates and the pose position the profile
    /// expects (UE5: local to the chunk; BWF: absolute world blocks).
    void projectPose(const Config& cfg) {
        const double size = cfg.effectiveChunkSize();
        chunkX = static_cast<int64_t>(std::floor(worldX / size));
        chunkY = static_cast<int64_t>(std::floor(worldY / size));
        chunkZ = static_cast<int64_t>(std::floor(worldZ / size));
        if (cfg.isBwf()) {
            posX = worldX;
            posY = worldY;
            posZ = worldZ;
        } else {
            posX = worldX - static_cast<double>(chunkX) * size;
            posY = worldY - static_cast<double>(chunkY) * size;
            posZ = cfg.volumeChunks > 0 ? worldZ - static_cast<double>(chunkZ) * size
                                        : UE5_HEIGHT_OFFSET;
        }
    }

    void updateWalk(const Config& cfg, double nowSec) {
        double dt = nowSec - lastMoveTime;
        lastMoveTime = nowSec;
        if (dt <= 0) return;
        const double speed = cfg.effectiveWalkSpeed();
        const double size = cfg.effectiveChunkSize();

        if (cfg.volumeChunks > 0) {
            // 3D drift inside the cube; bounce off each face; re-aim now and then.
            worldX += dirX * dt * speed;
            worldY += dirY * dt * speed;
            worldZ += dirZ * dt * speed;
            Axes a = axes(cfg);
            const double hmin = static_cast<double>(cubeMinHorizontal(cfg)) * size;
            const double hmax = hmin + static_cast<double>(cfg.volumeChunks) * size;
            const double umin = static_cast<double>(cfg.volumeBaseUp) * size;
            const double umax = umin + static_cast<double>(cfg.volumeChunks) * size;
            auto bounce = [](double& v, double& d, double lo, double hi) {
                if (v < lo) { v = lo + (lo - v); d = std::fabs(d); }
                if (v > hi) { v = hi - (v - hi); d = -std::fabs(d); }
                if (v < lo) v = lo;   // a step longer than the box
                if (v > hi) v = hi;
            };
            bounce(a.h1, a.d1, hmin, hmax);
            bounce(a.h2, a.d2, hmin, hmax);
            bounce(a.up, a.dup, umin, umax);
            if (nowSec >= reaimAt) {
                randomizeDirection3D();
                std::uniform_real_distribution<double> re(4.0, 12.0);
                reaimAt = nowSec + re(rng());
            }
            velX = dirX * speed;
            velY = dirY * speed;
            velZ = dirZ * speed;
            rotYawDeg = std::atan2(a.d2, a.d1) * (180.0 / 3.14159265358979);
        } else {
            Axes a = axes(cfg);
            a.h1 += a.d1 * dt * speed;
            a.h2 += a.d2 * dt * speed;
            velX = dirX * speed;
            velY = dirY * speed;
            velZ = dirZ * speed;
            rotYawDeg = std::atan2(a.d2, a.d1) * (180.0 / 3.14159265358979);
            // Bounce back toward the origin at the edge of the spawn area so the
            // simulated crowd stays co-located and generates fan-out.
            const int64_t c1 = static_cast<int64_t>(std::floor(a.h1 / size));
            const int64_t c2 = static_cast<int64_t>(std::floor(a.h2 / size));
            if (std::llabs(c1) > cfg.spawnRadiusChunks || std::llabs(c2) > cfg.spawnRadiusChunks) {
                a.d1 = -a.d1;
                a.d2 = -a.d2;
            }
        }
        projectPose(cfg);
    }

    /// Patch the dynamic fields and re-sign. Returns false on HMAC failure.
    bool buildUpdate(const Config& cfg) {
        wire::ActorPose p;
        p.posX = posX;
        p.posY = posY;
        p.posZ = posZ;
        p.velX = velX;
        p.velY = velY;
        p.velZ = velZ;
        if (cfg.isBwf()) {
            // Radians, and BWF's yaw 0 faces -z; the exact heading only turns the
            // avatar, so atan2 of the horizontal direction is close enough to read.
            p.yaw = std::atan2(dirX, dirZ);
            p.pitch = 0.0;
            p.grounded = false;
            p.updatedAtMs = static_cast<double>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
        } else {
            p.pitch = 0.0;
            p.yaw = 0.0;
            p.roll = rotYawDeg;
        }
        return wire::finalizeActorUpdateFmt(
            message, poseFormat(cfg), chunkX, chunkY, chunkZ, p, sequence++,
            reinterpret_cast<const uint8_t*>(creds.appToken.data()));
    }

    /// Bytes to send for this client's profile.
    static size_t messageSize(const Config& cfg) {
        return wire::actorUpdateSize(poseFormat(cfg));
    }
};

} // namespace lt
