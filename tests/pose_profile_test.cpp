// The BWF pose profile and the cube population.
//
// Why these exist: every load ladder up to 2026-09-05 targeted Blocks With
// Friends with the UE5 state payload. The servers relayed it happily; the game
// decoded it as x ~ 0, y = 0 (below bedrock) and a scattered z -- an invisible
// population whose minimap dots formed a straight line. These cases pin the byte
// layout BWF's `actorCodec.ts` reads (a 48-byte little-endian float32 struct) and
// the geometry the operator asked for (an 8x8x8 chunk cube with 3D drift, Y up,
// chunk = 16 blocks), so the harness cannot silently drift back to a shape the
// game cannot see.

#include "Config.hpp"
#include "SimClient.hpp"
#include "Wire.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

lt::Config bwfConfig() {
    lt::Config cfg;
    cfg.poseFormat = "bwf";
    cfg.appId = 84168416726016;
    cfg.updateHz = 10;
    cfg.distance = 8;
    cfg.decay = 1;
    return cfg;
}

lt::SimClient clientAt(int globalIndex) {
    lt::SimClient c;
    c.creds.index = globalIndex;
    c.creds.gameTokenId = 4242;
    c.creds.appToken = std::string(64, 'a');
    return c;
}

} // namespace

int main() {
    using namespace lt;

    // --- the BWF payload layout, byte for byte -------------------------------
    {
        const wire::PoseFormat fmt = wire::PoseFormat::BWF;
        check(wire::payloadSize(fmt) == 48, "bwf payload is 48 bytes");
        check(wire::actorUpdateSize(fmt) == 68 + 48 + 41, "bwf actor update is 157 bytes");
        check(wire::actorUpdateHmacOffset(fmt) == 116, "bwf HMAC starts at 116");

        uint8_t buf[wire::ACTOR_UPDATE_MAX_SIZE];
        const std::string token(64, 'k');
        wire::initActorUpdateTemplateFmt(buf, fmt, "0123456789ABCDEF0123456789ABCDEF", 7, 99,
                                         8, wire::DecayRate::EXPONENTIAL);
        wire::ActorPose p;
        p.posX = 12.5;
        p.posY = 33.0;
        p.posZ = -7.25;
        p.yaw = 1.5;
        p.pitch = -0.25;
        p.velX = 4.0;
        p.velY = 0.0;
        p.velZ = -4.0;
        p.grounded = false;
        p.updatedAtMs = 1757000000000.0;
        check(wire::finalizeActorUpdateFmt(buf, fmt, 0, 2, -1, p, 3,
                                           reinterpret_cast<const uint8_t*>(token.data())),
              "bwf finalize signs");

        const uint8_t* st = buf + wire::off::PAYLOAD;
        // Offsets from blocks-with-friends/src/session/actorCodec.ts.
        check(wire::readF32(st + 0) == 12.5f, "x at 0");
        check(wire::readF32(st + 4) == 33.0f, "y at 4");
        check(wire::readF32(st + 8) == -7.25f, "z at 8");
        check(wire::readF32(st + 12) == 1.5f, "yaw at 12");
        check(wire::readF32(st + 16) == -0.25f, "pitch at 16");
        check(wire::readF32(st + 20) == 4.0f, "velocityX at 20");
        check(wire::readF32(st + 24) == 0.0f, "velocityY at 24");
        check(wire::readF32(st + 28) == -4.0f, "velocityZ at 28");
        check(st[32] == 0, "flags byte 32 is 0 for a flying player (never mob)");
        check(st[33] == 0, "heldBlockId byte 33 is 0");
        check(st[34] == 0 && st[35] == 0, "bytes 34-35 reserved");
        check(wire::readF64(st + 36) == 1757000000000.0, "updatedAt f64 at 36");
        check(st[44] == 0 && st[47] == 0, "bytes 44-47 reserved");

        // The header and tail are the platform's, unchanged by the profile.
        check(buf[0] == wire::ACTOR_UPDATE_REQUEST_2, "type byte");
        check(wire::readI64(buf + wire::off::CHUNK_Y) == 2, "chunk y");
        check(wire::readI64(buf + wire::off::CHUNK_Z) == -1, "chunk z");
        check(wire::readI64(buf + wire::actorUpdateTokenIdOffset(fmt)) == 99, "token id after HMAC");
        check(buf[wire::actorUpdateSeqOffset(fmt)] == 3, "seq last");
        // The grounded flag lands on bit0.
        p.grounded = true;
        wire::finalizeActorUpdateFmt(buf, fmt, 0, 0, 0, p, 4,
                                     reinterpret_cast<const uint8_t*>(token.data()));
        check(st[32] == wire::bwfpose::FLAG_GROUNDED, "grounded sets bit0 only");

        // The UE5 profile is untouched: same size and offsets as before.
        check(wire::actorUpdateSize(wire::PoseFormat::UE5) == wire::ACTOR_UPDATE_SIZE,
              "ue5 size unchanged (197)");
        check(wire::actorUpdateHmacOffset(wire::PoseFormat::UE5) == 156, "ue5 HMAC offset 156");
    }

    // --- BWF geometry: Y up, chunk = 16 blocks, absolute positions ----------
    {
        Config cfg = bwfConfig();
        check(cfg.effectiveChunkSize() == 16.0, "bwf chunk defaults to 16 blocks");
        check(cfg.effectiveWalkSpeed() == 4.0, "bwf walk speed defaults to 4 blocks/s");
        Config ue5;
        check(ue5.effectiveChunkSize() == 1600.0 && ue5.effectiveWalkSpeed() == 150.0,
              "ue5 defaults unchanged");

        // 2D walk: hovers at 20 blocks (chunk layer 1), moves on x/z.
        SimClient c = clientAt(0);
        c.initSimulation(cfg, 100.0);
        check(c.worldY == 20.0 && c.chunkY == 1, "bwf 2D walk stands at y=20, chunk y=1");
        check(c.posY == c.worldY && c.posX == c.worldX, "bwf pose position is absolute");
        c.updateWalk(cfg, 101.0);
        check(c.worldY == 20.0, "2D walk keeps its height");
        check(std::llabs(c.chunkX) <= cfg.spawnRadiusChunks + 1, "2D walk stays near the origin");
        check(c.buildUpdate(cfg), "bwf update signs");
    }

    // --- the cube: 8x8x8, filled by global index, drift bounces off faces ----
    {
        Config cfg = bwfConfig();
        cfg.volumeChunks = 8;
        cfg.volumeBaseUp = 0;
        const double size = 16.0;
        const double hmin = -4 * size, hmax = 4 * size;   // centred on chunk (0, 0)
        const double umin = 0.0, umax = 8 * size;

        std::set<std::tuple<int64_t, int64_t, int64_t>> slots;
        for (int i = 0; i < 512; ++i) {
            SimClient c = clientAt(i);
            c.initSimulation(cfg, 0.0);
            slots.insert({c.chunkX, c.chunkY, c.chunkZ});
            if (c.worldX < hmin || c.worldX >= hmax || c.worldZ < hmin || c.worldZ >= hmax ||
                c.worldY < umin || c.worldY >= umax) {
                check(false, "cube slot inside the volume");
                break;
            }
        }
        check(slots.size() == 512, "512 consecutive global indices fill 512 distinct chunks");
        check(std::llabs(std::get<0>(*slots.begin())) <= 4, "horizontal chunks are centred on the origin");

        // Vertical spread: the cube reaches chunk y 7, well above BWF's 3-layer terrain,
        // which is why these bots are visible in the sky rather than underground.
        int64_t maxY = -1;
        for (const auto& s : slots) maxY = std::max(maxY, std::get<1>(s));
        check(maxY == 7, "cube spans chunk y 0..7");

        // Drift: a client walking for a long time stays inside the box.
        SimClient c = clientAt(77);
        c.initSimulation(cfg, 0.0);
        bool inside = true;
        bool moved = false;
        const double x0 = c.worldX;
        for (int t = 1; t <= 6000; ++t) {
            c.updateWalk(cfg, t * 0.1);
            if (c.worldX < hmin || c.worldX > hmax || c.worldZ < hmin || c.worldZ > hmax ||
                c.worldY < umin || c.worldY > umax)
                inside = false;
            if (c.worldX != x0) moved = true;
        }
        check(inside, "600 s of drift stays inside the cube");
        check(moved, "the client actually moves");
        check(c.chunkY == static_cast<int64_t>(std::floor(c.worldY / 16.0)), "chunk y follows world y");
        check(c.buildUpdate(cfg), "cube client update signs");
    }

    // --- the cube with the UE5 profile: chunk-local positions, Z up ----------
    {
        Config cfg;
        cfg.appId = 1;
        cfg.volumeChunks = 2;
        SimClient c = clientAt(3);
        c.initSimulation(cfg, 0.0);
        check(c.posX >= 0 && c.posX < 1600 && c.posZ >= 0 && c.posZ < 1600,
              "ue5 cube pose is local to the chunk");
        check(c.chunkZ >= 0 && c.chunkZ < 2, "ue5 cube stands on z (up) 0..1");
    }

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
