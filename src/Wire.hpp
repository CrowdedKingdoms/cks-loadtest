#pragma once

#include "Hmac.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>

/// On-wire builders/parsers for the public Buddy client protocol
/// (long-form spatial messages, message bundles, error messages).
///
/// Long spatial layout (client -> server, containsAuth = 1):
///   [1B type][8B appId][8B chunkX][8B chunkY][8B chunkZ]
///   [1B distance][1B decay][1B containsAuth][32B uuid]
///   [payload...]
///   [32B HMAC][8B gameTokenId LE][1B seq]
///
/// Server -> client notifications reuse the layout; the 8-byte slot after the
/// (optional) HMAC carries the server's epoch milliseconds instead of the
/// game token id.
namespace lt::wire {

// Message types, per the public wire-format reference
// (docs.crowdedkingdoms.com/replication-api/wire-formats).
enum : uint8_t {
    MESSAGE_BUNDLE = 2,
    GENERIC_ERROR_MESSAGE = 3,
    COMMAND_RECONNECT = 22,
    CLIENT_ACTOR_HEARTBEAT = 26,
    ACTOR_UPDATE_REQUEST_2 = 128,
    ACTOR_UPDATE_NOTIFICATION_2 = 130,
    VOXEL_UPDATE_NOTIFICATION_2 = 133,
    CLIENT_TEXT_PACKET_2 = 136,
    CLIENT_EVENT_NOTIFICATION_2 = 138,
    SERVER_EVENT_NOTIFICATION_2 = 139,
    GENERIC_SPATIAL_1 = 140,
    SINGLE_ACTOR_MESSAGE = 142,
};
constexpr uint8_t SPATIAL_TYPE_BIT = 0x80;

// Error codes carried by GENERIC_ERROR_MESSAGE: [1B type=3][1B seq][1B code].
enum : uint8_t {
    ERR_INVALID_TOKEN = 5,
    ERR_UNAUTHORIZED = 7,
    ERR_INVALID_REQUEST = 15,
    ERR_INVALID_APP_ID = 18,
    ERR_USER_NOT_AUTHENTICATED = 20,
    ERR_TOKEN_EXPIRED = 32,
};

enum class DecayRate : uint8_t {
    NONE = 0,
    EXPONENTIAL = 1,
    LINEAR_50 = 2,
    LINEAR_25 = 3,
    LINEAR_10 = 4,
    LINEAR_5 = 5,
};

// Long spatial byte offsets.
namespace off {
constexpr size_t TYPE = 0;
constexpr size_t APP_ID = 1;
constexpr size_t CHUNK_X = 9;
constexpr size_t CHUNK_Y = 17;
constexpr size_t CHUNK_Z = 25;
constexpr size_t DISTANCE = 33;
constexpr size_t DECAY = 34;
constexpr size_t CONTAINS_AUTH = 35;
constexpr size_t UUID = 36;
constexpr size_t PAYLOAD = 68;
} // namespace off

constexpr size_t HEADER_SIZE = off::PAYLOAD;                     // 68
constexpr size_t TAIL_WITH_HMAC = hmac::TAG_SIZE + 8 + 1;        // 41
constexpr size_t TAIL_NO_HMAC = 8 + 1;                           // 9

// Actor state payload v2 (88 bytes, UE5-compatible), all fields little-endian.
namespace state {
constexpr uint8_t VERSION_VALUE = 2;
constexpr size_t SIZE = 88;
constexpr size_t VERSION = 0;
constexpr size_t POSITION_X = 8;
constexpr size_t POSITION_Y = 16;
constexpr size_t POSITION_Z = 24;
constexpr size_t ROTATION_PITCH = 32;
constexpr size_t ROTATION_YAW = 40;
constexpr size_t ROTATION_ROLL = 48;
constexpr size_t VELOCITY_X = 56;
constexpr size_t VELOCITY_Y = 64;
constexpr size_t VELOCITY_Z = 72;
constexpr size_t B_CROUCH = 80;
constexpr size_t E_ATTACHMENTS = 81;
} // namespace state

/// Actor pose payload as Blocks With Friends encodes and decodes it
/// (`blocks-with-friends/src/session/actorCodec.ts`): 48 bytes, little-endian
/// float32 fields, positions in world BLOCKS (Y up, chunk = 16 blocks), yaw and
/// pitch in radians, `flags` bit0 = grounded, bit1 = mob (never set by a player),
/// `heldBlockId` a block id, `updatedAt` epoch milliseconds as float64.
///
/// WHY A SECOND PROFILE EXISTS. The platform relays the payload opaquely, so a
/// load test "against" a game can send anything and the servers will not care --
/// but the game will. Every ladder up to 2026-09-05 targeted BWF with the UE5
/// state above, and BWF decoded it as x ~ 0 (the version byte as a denormal),
/// y = 0 (below bedrock, so no avatar) and z = the mantissa bits of the UE5
/// posX: an invisible population whose minimap dots formed a straight line.
/// A load test a player can see is the only kind whose fan-out numbers mean
/// what the game's players would experience.
namespace bwfpose {
constexpr size_t SIZE = 48;
constexpr size_t X = 0;
constexpr size_t Y = 4;
constexpr size_t Z = 8;
constexpr size_t YAW = 12;
constexpr size_t PITCH = 16;
constexpr size_t VELOCITY_X = 20;
constexpr size_t VELOCITY_Y = 24;
constexpr size_t VELOCITY_Z = 28;
constexpr size_t FLAGS = 32;
constexpr size_t HELD_BLOCK_ID = 33;
constexpr size_t UPDATED_AT = 36;   // float64
constexpr uint8_t FLAG_GROUNDED = 0x01;
constexpr uint8_t FLAG_MOB = 0x02;
} // namespace bwfpose

/// Which actor-state payload a client writes.
enum class PoseFormat : uint8_t {
    UE5 = 0,  ///< 88-byte float64 state v2 (the reference; the default)
    BWF = 1,  ///< 48-byte float32 pose Blocks With Friends decodes
};

constexpr size_t payloadSize(PoseFormat f) {
    return f == PoseFormat::BWF ? bwfpose::SIZE : state::SIZE;
}
/// Signed ACTOR_UPDATE_REQUEST_2 length for a profile: header + payload + tail.
constexpr size_t actorUpdateSize(PoseFormat f) {
    return HEADER_SIZE + payloadSize(f) + TAIL_WITH_HMAC;
}
constexpr size_t actorUpdateHmacOffset(PoseFormat f) {
    return HEADER_SIZE + payloadSize(f);
}
constexpr size_t actorUpdateTokenIdOffset(PoseFormat f) {
    return actorUpdateHmacOffset(f) + hmac::TAG_SIZE;
}
constexpr size_t actorUpdateSeqOffset(PoseFormat f) {
    return actorUpdateTokenIdOffset(f) + 8;
}

/// Signed ACTOR_UPDATE_REQUEST_2 with the UE5 state: 68 + 88 + 41 = 197 bytes.
/// These four are the UE5 profile's offsets; per-profile code uses the
/// functions above. The largest profile sizes the client's message buffer.
constexpr size_t ACTOR_UPDATE_SIZE = actorUpdateSize(PoseFormat::UE5);
constexpr size_t ACTOR_UPDATE_HMAC_OFFSET = actorUpdateHmacOffset(PoseFormat::UE5); // 156
constexpr size_t ACTOR_UPDATE_TOKEN_ID_OFFSET = actorUpdateTokenIdOffset(PoseFormat::UE5);
constexpr size_t ACTOR_UPDATE_SEQ_OFFSET = actorUpdateSeqOffset(PoseFormat::UE5);
constexpr size_t ACTOR_UPDATE_MAX_SIZE = ACTOR_UPDATE_SIZE;
static_assert(actorUpdateSize(PoseFormat::BWF) <= ACTOR_UPDATE_MAX_SIZE);

inline void writeI64(uint8_t* dst, int64_t v) { std::memcpy(dst, &v, 8); }
inline void writeF64(uint8_t* dst, double v) { std::memcpy(dst, &v, 8); }
inline void writeF32(uint8_t* dst, float v) { std::memcpy(dst, &v, 4); }
inline int64_t readI64(const uint8_t* src) {
    int64_t v;
    std::memcpy(&v, src, 8);
    return v;
}
inline float readF32(const uint8_t* src) {
    float v;
    std::memcpy(&v, src, 4);
    return v;
}
inline double readF64(const uint8_t* src) {
    double v;
    std::memcpy(&v, src, 8);
    return v;
}

/// The values one send carries, in the profile's own units and axes.
struct ActorPose {
    double posX = 0, posY = 0, posZ = 0;
    double pitch = 0, yaw = 0, roll = 0;      // UE5: degrees; BWF: radians (roll unused)
    double velX = 0, velY = 0, velZ = 0;
    bool grounded = false;                    // BWF flags bit0
    double updatedAtMs = 0;                   // BWF only
};

/// Fill the static fields of a signed actor-update template for a profile.
inline void initActorUpdateTemplateFmt(uint8_t* buf, PoseFormat fmt, const char* uuid32,
                                       int64_t appId, int64_t gameTokenId,
                                       uint8_t distance, DecayRate decay) {
    std::memset(buf, 0, ACTOR_UPDATE_MAX_SIZE);
    buf[off::TYPE] = ACTOR_UPDATE_REQUEST_2;
    writeI64(buf + off::APP_ID, appId);
    buf[off::DISTANCE] = distance;
    buf[off::DECAY] = static_cast<uint8_t>(decay);
    buf[off::CONTAINS_AUTH] = 1;
    std::memcpy(buf + off::UUID, uuid32, 32);
    if (fmt == PoseFormat::UE5) buf[HEADER_SIZE + state::VERSION] = state::VERSION_VALUE;
    writeI64(buf + actorUpdateTokenIdOffset(fmt), gameTokenId);
}

/// Patch the per-send fields for a profile and re-sign. See finalizeActorUpdate.
inline bool finalizeActorUpdateFmt(uint8_t* buf, PoseFormat fmt, int64_t chunkX,
                                   int64_t chunkY, int64_t chunkZ, const ActorPose& p,
                                   uint8_t seq, const uint8_t* token64) {
    writeI64(buf + off::CHUNK_X, chunkX);
    writeI64(buf + off::CHUNK_Y, chunkY);
    writeI64(buf + off::CHUNK_Z, chunkZ);
    uint8_t* st = buf + off::PAYLOAD;
    if (fmt == PoseFormat::BWF) {
        writeF32(st + bwfpose::X, static_cast<float>(p.posX));
        writeF32(st + bwfpose::Y, static_cast<float>(p.posY));
        writeF32(st + bwfpose::Z, static_cast<float>(p.posZ));
        writeF32(st + bwfpose::YAW, static_cast<float>(p.yaw));
        writeF32(st + bwfpose::PITCH, static_cast<float>(p.pitch));
        writeF32(st + bwfpose::VELOCITY_X, static_cast<float>(p.velX));
        writeF32(st + bwfpose::VELOCITY_Y, static_cast<float>(p.velY));
        writeF32(st + bwfpose::VELOCITY_Z, static_cast<float>(p.velZ));
        // Never FLAG_MOB (or the NPC flag BWF's mob codec defines): the game routes
        // those to lanes that expect a mob or an NPC suffix, not a player.
        st[bwfpose::FLAGS] = p.grounded ? bwfpose::FLAG_GROUNDED : 0;
        st[bwfpose::HELD_BLOCK_ID] = 0;
        writeF64(st + bwfpose::UPDATED_AT, p.updatedAtMs);
    } else {
        writeF64(st + state::POSITION_X, p.posX);
        writeF64(st + state::POSITION_Y, p.posY);
        writeF64(st + state::POSITION_Z, p.posZ);
        writeF64(st + state::ROTATION_PITCH, p.pitch);
        writeF64(st + state::ROTATION_YAW, p.yaw);
        writeF64(st + state::ROTATION_ROLL, p.roll);
        writeF64(st + state::VELOCITY_X, p.velX);
        writeF64(st + state::VELOCITY_Y, p.velY);
        writeF64(st + state::VELOCITY_Z, p.velZ);
    }
    buf[actorUpdateSeqOffset(fmt)] = seq;
    return hmac::spatialSign(buf, actorUpdateHmacOffset(fmt), token64,
                             buf + actorUpdateHmacOffset(fmt));
}

/// Fill the static fields of a signed actor-update template. Chunk coords,
/// actor state, HMAC and sequence are patched per send.
inline void initActorUpdateTemplate(uint8_t* buf, const char* uuid32,
                                    int64_t appId, int64_t gameTokenId,
                                    uint8_t distance, DecayRate decay) {
    std::memset(buf, 0, ACTOR_UPDATE_SIZE);
    buf[off::TYPE] = ACTOR_UPDATE_REQUEST_2;
    writeI64(buf + off::APP_ID, appId);
    buf[off::DISTANCE] = distance;
    buf[off::DECAY] = static_cast<uint8_t>(decay);
    buf[off::CONTAINS_AUTH] = 1;
    std::memcpy(buf + off::UUID, uuid32, 32);
    buf[HEADER_SIZE + state::VERSION] = state::VERSION_VALUE;
    writeI64(buf + ACTOR_UPDATE_TOKEN_ID_OFFSET, gameTokenId);
}

/// Patch the per-send fields (chunk, actor state, sequence) then recompute the
/// HMAC over the prefix (everything before the HMAC field).
/// `token64` must point to the 64-octet app token.
inline bool finalizeActorUpdate(uint8_t* buf, int64_t chunkX, int64_t chunkY,
                                int64_t chunkZ, double posX, double posY,
                                double posZ, double rotPitch, double rotYaw,
                                double rotRoll, double velX, double velY,
                                double velZ, uint8_t seq,
                                const uint8_t* token64) {
    writeI64(buf + off::CHUNK_X, chunkX);
    writeI64(buf + off::CHUNK_Y, chunkY);
    writeI64(buf + off::CHUNK_Z, chunkZ);
    uint8_t* st = buf + off::PAYLOAD;
    writeF64(st + state::POSITION_X, posX);
    writeF64(st + state::POSITION_Y, posY);
    writeF64(st + state::POSITION_Z, posZ);
    writeF64(st + state::ROTATION_PITCH, rotPitch);
    writeF64(st + state::ROTATION_YAW, rotYaw);
    writeF64(st + state::ROTATION_ROLL, rotRoll);
    writeF64(st + state::VELOCITY_X, velX);
    writeF64(st + state::VELOCITY_Y, velY);
    writeF64(st + state::VELOCITY_Z, velZ);
    buf[ACTOR_UPDATE_SEQ_OFFSET] = seq;
    return hmac::spatialSign(buf, ACTOR_UPDATE_HMAC_OFFSET, token64,
                             buf + ACTOR_UPDATE_HMAC_OFFSET);
}

/// One parsed inbound message (a whole datagram, or one bundle member).
struct InboundView {
    const uint8_t* data;
    size_t len;
    uint8_t type() const { return len > 0 ? data[0] : 0; }
};

/// For a server->client long-spatial notification, extract the epoch-millis
/// slot from the tail (8 bytes at len-9). Returns nullopt for non-spatial or
/// too-short messages.
inline std::optional<int64_t> notificationEpochMs(const InboundView& m) {
    if (m.len < HEADER_SIZE + TAIL_NO_HMAC) return std::nullopt;
    if (!(m.type() & SPATIAL_TYPE_BIT)) return std::nullopt;
    return readI64(m.data + m.len - TAIL_NO_HMAC);
}

/// Verify the server-side HMAC on a signed (containsAuth=1) long-spatial
/// notification, keyed on this client's app token. Unsigned messages return
/// true (nothing to verify).
inline bool verifyNotification(const InboundView& m, const uint8_t* token64) {
    if (m.len <= off::CONTAINS_AUTH || !(m.type() & SPATIAL_TYPE_BIT)) return true;
    if (m.data[off::CONTAINS_AUTH] == 0) return true;
    if (m.len < HEADER_SIZE + TAIL_WITH_HMAC) return false;
    const size_t prefixLen = m.len - TAIL_WITH_HMAC;
    uint8_t expected[hmac::TAG_SIZE];
    if (!hmac::spatialSign(m.data, prefixLen, token64, expected)) return false;
    return hmac::tagEquals(expected, m.data + prefixLen);
}

/// Iterate the messages inside a datagram. A MESSAGE_BUNDLE
/// ([1B type=2]{[2B len LE][msg]}...) yields each member; any other datagram
/// yields itself once. Returns false if a bundle is malformed/truncated
/// (members before the truncation are still delivered).
inline bool forEachMessage(const uint8_t* data, size_t len,
                           const std::function<void(const InboundView&)>& fn) {
    if (len == 0) return false;
    if (data[0] != MESSAGE_BUNDLE) {
        fn(InboundView{data, len});
        return true;
    }
    size_t offset = 1;
    while (offset < len) {
        if (offset + 2 > len) return false;
        uint16_t msgLen;
        std::memcpy(&msgLen, data + offset, 2);
        offset += 2;
        if (offset + msgLen > len) return false;
        fn(InboundView{data + offset, msgLen});
        offset += msgLen;
    }
    return true;
}

} // namespace lt::wire
