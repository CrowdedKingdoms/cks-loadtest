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

    // Walk simulation (Unreal units; chunk = floor(world / 1600)).
    static constexpr double CHUNK_SIZE = 1600.0;
    static constexpr double HEIGHT_OFFSET = -60.0;
    double worldX = 0, worldY = 0;
    double dirX = 1, dirY = 0;
    int64_t chunkX = 0, chunkY = 0, chunkZ = 0;
    double posX = 0, posY = 0, posZ = 0;
    double velX = 0, velY = 0, velZ = 0;
    double rotYawDeg = 0;

    char uuid[32] = {};
    uint8_t sequence = 0;
    double lastSendTime = 0;
    double lastMoveTime = 0;

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

    uint8_t message[wire::ACTOR_UPDATE_SIZE] = {};

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

    void randomizeDirection() {
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        double x, y;
        do {
            x = dist(rng());
            y = dist(rng());
        } while (std::sqrt(x * x + y * y) < 0.0001);
        double len = std::sqrt(x * x + y * y);
        dirX = x / len;
        dirY = y / len;
    }

    /// Initialize walk state and the message template. Called once when the
    /// client first activates (UUID is stable across reassignments).
    void initSimulation(const Config& cfg, double nowSec) {
        std::uniform_int_distribution<int64_t> chunkDist(-cfg.spawnRadiusChunks,
                                                         cfg.spawnRadiusChunks);
        chunkX = chunkDist(rng());
        chunkY = chunkDist(rng());
        chunkZ = 0;
        worldX = static_cast<double>(chunkX) * CHUNK_SIZE;
        worldY = static_cast<double>(chunkY) * CHUNK_SIZE;
        randomizeDirection();
        generateUuid();
        lastMoveTime = nowSec;
        // Stagger the first send inside one update interval so a batch of
        // clients doesn't burst-send on the same tick.
        double interval = 1.0 / cfg.updateHz;
        std::uniform_real_distribution<double> off(0.0, 1.0);
        lastSendTime = nowSec - interval + off(rng()) * interval;
        sequence = 0;
        rebuildTemplate(cfg);
    }

    /// (Re)build the static message fields; needed at init and whenever the
    /// gameTokenId changes (token refresh).
    void rebuildTemplate(const Config& cfg) {
        wire::initActorUpdateTemplate(message, uuid, cfg.appId, creds.gameTokenId,
                                      static_cast<uint8_t>(cfg.distance),
                                      static_cast<wire::DecayRate>(cfg.decay));
    }

    void updateWalk(const Config& cfg, double nowSec) {
        double dt = nowSec - lastMoveTime;
        lastMoveTime = nowSec;
        if (dt <= 0) return;

        worldX += dirX * dt * cfg.walkSpeed;
        worldY += dirY * dt * cfg.walkSpeed;
        velX = dirX * cfg.walkSpeed;
        velY = dirY * cfg.walkSpeed;
        velZ = 0;
        rotYawDeg = std::atan2(dirY, dirX) * (180.0 / 3.14159265358979);

        chunkX = static_cast<int64_t>(std::floor(worldX / CHUNK_SIZE));
        chunkY = static_cast<int64_t>(std::floor(worldY / CHUNK_SIZE));
        chunkZ = 0;
        posX = worldX - static_cast<double>(chunkX) * CHUNK_SIZE;
        posY = worldY - static_cast<double>(chunkY) * CHUNK_SIZE;
        posZ = HEIGHT_OFFSET;

        // Bounce back toward the origin at the edge of the spawn area so the
        // simulated crowd stays co-located and generates fan-out.
        if (std::llabs(chunkX) > cfg.spawnRadiusChunks ||
            std::llabs(chunkY) > cfg.spawnRadiusChunks) {
            dirX = -dirX;
            dirY = -dirY;
        }
    }

    /// Patch the dynamic fields and re-sign. Returns false on HMAC failure.
    bool buildUpdate(const Config&) {
        return wire::finalizeActorUpdate(
            message, chunkX, chunkY, chunkZ, posX, posY, posZ,
            /*pitch*/ 0.0, /*yaw*/ 0.0, rotYawDeg, velX, velY, velZ, sequence++,
            reinterpret_cast<const uint8_t*>(creds.appToken.data()));
    }
};

} // namespace lt
