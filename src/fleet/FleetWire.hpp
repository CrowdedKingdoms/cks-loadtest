#pragma once

#include "Hmac.hpp"
#include "Wire.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>

/// Wire formats of a fleet session: one realtime session an organization's game
/// server opens for itself, in which it carries its players.
///
/// A message a fleet session sends FOR A PLAYER is the message that player's own
/// session would send, with two differences in the long-form tail:
///   - the 8-byte game token id is the PLAYER's gameTokenId
///   - the HMAC is keyed with the FLEET token (64 octets)
/// A message that is the session's own (capabilities, heartbeats, FLEET_CARRY,
/// FLEET_RELEASE) carries the fleet session's own gameTokenId.
///
/// Everything here builds on Wire.hpp and Hmac.hpp: the long-form layout, the
/// `prefix || token` signing and the bundle walk are theirs.
namespace lt::fleet {

// Message types.
inline constexpr uint8_t COMMAND_SESSION_RELEASED = 28; // server -> client: [28][reason][32B HMAC]
inline constexpr uint8_t FLEET_CARRY = 49;              // client -> server
inline constexpr uint8_t FLEET_RELEASE = 50;            // client -> server
inline constexpr uint8_t FLEET_CARRY_RESULT = 51;       // server -> client
inline constexpr uint8_t FLEET_RELEASED = 52;           // server -> client
inline constexpr uint8_t FLEET_DELIVERY = 53;           // server -> client

/// CLIENT_CAPABILITIES bit 3: "send me state my own carried players sent, when
/// another of my carried players is in range". Off by default.
inline constexpr uint32_t CAP_FLEET_OWN_STATE = 1u << 3;

/// FLEET_CARRY_RESULT status.
enum CarryStatus : uint8_t {
    CARRY_CARRIED = 0,
    CARRY_NO_LIVE_TOKEN = 1,
    CARRY_OTHER_ORG_OR_APP = 2,
    CARRY_PROOF_OR_TIME = 3,
    CARRY_SESSION_FULL = 4,
};

/// FLEET_RELEASED reason.
enum ReleaseReason : uint8_t {
    RELEASED_CARRIED_ELSEWHERE = 1,
    RELEASED_SIGN_IN_ENDED = 2,
    RELEASED_IDLE = 5,
};

inline constexpr char CARRY_LABEL[] = "ck-fleet-carry";
inline constexpr size_t CARRY_LABEL_LEN = 14;
inline constexpr size_t CARRY_PROOF_MESSAGE_LEN = CARRY_LABEL_LEN + 8 + 8 + 8; // 38
inline constexpr size_t CARRY_PAYLOAD_LEN = 8 + 8 + hmac::TAG_SIZE;            // 48
inline constexpr size_t RELEASE_PAYLOAD_LEN = 8;
inline constexpr size_t FLEET_CARRY_SIZE =
    wire::HEADER_SIZE + CARRY_PAYLOAD_LEN + wire::TAIL_WITH_HMAC;   // 157
inline constexpr size_t FLEET_RELEASE_SIZE =
    wire::HEADER_SIZE + RELEASE_PAYLOAD_LEN + wire::TAIL_WITH_HMAC; // 117
inline constexpr size_t HEARTBEAT_SIZE = wire::HEADER_SIZE + wire::TAIL_WITH_HMAC; // 109
inline constexpr size_t MAX_DELIVERY_PLAYERS = 32;
inline constexpr size_t MAX_CONTROL_SIZE = FLEET_CARRY_SIZE;

/// Plain HMAC-SHA256(key = the 64 token octets, message). The carry proof and
/// the two control commands use this form. The spatial tail uses
/// hmac::spatialSign, whose message is `prefix || token`.
inline bool hmacPlain(const uint8_t* token64, const uint8_t* msg, size_t len, uint8_t* out) {
    unsigned int outLen = 0;
    return HMAC(EVP_sha256(), token64, static_cast<int>(hmac::TOKEN_OCTETS), msg, len, out,
                &outLen) != nullptr &&
           outLen == hmac::TAG_SIZE;
}

/// The proof inside FLEET_CARRY: the game server holds the player's own token.
///   HMAC-SHA256(key = player token, "ck-fleet-carry" + fleet id + player id + epoch millis)
/// each number 8 bytes little endian.
inline bool carryProof(const uint8_t* playerToken64, int64_t fleetGameTokenId,
                       int64_t playerGameTokenId, int64_t epochMs, uint8_t* out) {
    uint8_t msg[CARRY_PROOF_MESSAGE_LEN];
    std::memcpy(msg, CARRY_LABEL, CARRY_LABEL_LEN);
    wire::writeI64(msg + CARRY_LABEL_LEN, fleetGameTokenId);
    wire::writeI64(msg + CARRY_LABEL_LEN + 8, playerGameTokenId);
    wire::writeI64(msg + CARRY_LABEL_LEN + 16, epochMs);
    return hmacPlain(playerToken64, msg, sizeof(msg), out);
}

/// A session's own control message in the long-form layout, exactly as
/// CLIENT_CAPABILITIES is sent: chunk (0,0,0), distance 0, no actor UUID, the
/// session's own gameTokenId in the tail, HMAC keyed with the fleet token.
/// `buf` must hold HEADER_SIZE + payloadLen + TAIL_WITH_HMAC. Returns the length, 0 on failure.
inline size_t buildControl(uint8_t* buf, uint8_t type, int64_t appId, const uint8_t* payload,
                           size_t payloadLen, int64_t fleetGameTokenId, uint8_t seq,
                           const uint8_t* fleetToken64) {
    const size_t hmacOff = wire::HEADER_SIZE + payloadLen;
    const size_t total = hmacOff + wire::TAIL_WITH_HMAC;
    std::memset(buf, 0, total);
    buf[wire::off::TYPE] = type;
    wire::writeI64(buf + wire::off::APP_ID, appId);
    buf[wire::off::CONTAINS_AUTH] = 1;
    if (payloadLen) std::memcpy(buf + wire::HEADER_SIZE, payload, payloadLen);
    wire::writeI64(buf + hmacOff + hmac::TAG_SIZE, fleetGameTokenId);
    buf[total - 1] = seq;
    if (!hmac::spatialSign(buf, hmacOff, fleetToken64, buf + hmacOff)) return 0;
    return total;
}

/// FLEET_CARRY (49). App payload: [8B player gameTokenId][8B epoch millis][32B proof].
inline size_t buildCarry(uint8_t* buf, int64_t appId, int64_t fleetGameTokenId,
                         int64_t playerGameTokenId, int64_t epochMs,
                         const uint8_t* playerToken64, uint8_t seq,
                         const uint8_t* fleetToken64) {
    uint8_t payload[CARRY_PAYLOAD_LEN];
    wire::writeI64(payload, playerGameTokenId);
    wire::writeI64(payload + 8, epochMs);
    if (!carryProof(playerToken64, fleetGameTokenId, playerGameTokenId, epochMs, payload + 16))
        return 0;
    return buildControl(buf, FLEET_CARRY, appId, payload, sizeof(payload), fleetGameTokenId,
                        seq, fleetToken64);
}

/// FLEET_RELEASE (50). App payload: [8B player gameTokenId].
inline size_t buildRelease(uint8_t* buf, int64_t appId, int64_t fleetGameTokenId,
                           int64_t playerGameTokenId, uint8_t seq,
                           const uint8_t* fleetToken64) {
    uint8_t payload[RELEASE_PAYLOAD_LEN];
    wire::writeI64(payload, playerGameTokenId);
    return buildControl(buf, FLEET_RELEASE, appId, payload, sizeof(payload), fleetGameTokenId,
                        seq, fleetToken64);
}

/// The session's own keep-alive: CLIENT_ACTOR_HEARTBEAT (26) in the long-form
/// layout with no actor, no payload and the session's own gameTokenId.
inline size_t buildHeartbeat(uint8_t* buf, int64_t appId, int64_t fleetGameTokenId,
                             uint8_t seq, const uint8_t* fleetToken64) {
    return buildControl(buf, wire::CLIENT_ACTOR_HEARTBEAT, appId, nullptr, 0,
                        fleetGameTokenId, seq, fleetToken64);
}

// ---------------------------------------------------------------------------
// Server -> client.
// ---------------------------------------------------------------------------

/// Where a fleet frame's bytes are. The contract gives the bytes after the type
/// ([51][8B player][8B user][1B status]); that compact form is what this client
/// expects. A server that framed the same bytes as the app payload of a
/// long-form message (as the client-to-server control messages are) is read
/// too, so the first live run does not hinge on that choice.
struct FleetBody {
    const uint8_t* data = nullptr;
    size_t len = 0;
    bool longForm = false;
};

inline bool looksLongForm(const wire::InboundView& m, int64_t appId) {
    if (m.len < wire::HEADER_SIZE + wire::TAIL_NO_HMAC) return false;
    if (wire::readI64(m.data + wire::off::APP_ID) != appId) return false;
    // Control messages sit at chunk (0,0,0), distance 0, decay 0. In the compact
    // form these bytes are player ids or a notification, which are not all zero.
    for (size_t i = wire::off::CHUNK_X; i < wire::off::CONTAINS_AUTH; ++i) {
        if (m.data[i] != 0) return false;
    }
    const uint8_t auth = m.data[wire::off::CONTAINS_AUTH];
    if (auth > 1) return false;
    return m.len >= wire::HEADER_SIZE + (auth ? wire::TAIL_WITH_HMAC : wire::TAIL_NO_HMAC);
}

inline std::optional<FleetBody> fleetBody(const wire::InboundView& m, int64_t appId) {
    if (m.len < 2) return std::nullopt;
    if (looksLongForm(m, appId)) {
        const size_t tail = m.data[wire::off::CONTAINS_AUTH] ? wire::TAIL_WITH_HMAC
                                                             : wire::TAIL_NO_HMAC;
        return FleetBody{m.data + wire::HEADER_SIZE, m.len - wire::HEADER_SIZE - tail, true};
    }
    return FleetBody{m.data + 1, m.len - 1, false};
}

/// FLEET_CARRY_RESULT (51): [8B player gameTokenId][8B user id][1B status].
struct CarryResult {
    int64_t playerGameTokenId = 0;
    int64_t userId = 0;
    uint8_t status = 0;
};
inline std::optional<CarryResult> parseCarryResult(const wire::InboundView& m, int64_t appId) {
    if (m.type() != FLEET_CARRY_RESULT) return std::nullopt;
    auto body = fleetBody(m, appId);
    if (!body || body->len < 17) return std::nullopt;
    CarryResult r;
    r.playerGameTokenId = wire::readI64(body->data);
    r.userId = wire::readI64(body->data + 8);
    r.status = body->data[16];
    return r;
}

/// FLEET_RELEASED (52): [8B player gameTokenId][1B reason].
struct Released {
    int64_t playerGameTokenId = 0;
    uint8_t reason = 0;
};
inline std::optional<Released> parseReleased(const wire::InboundView& m, int64_t appId) {
    if (m.type() != FLEET_RELEASED) return std::nullopt;
    auto body = fleetBody(m, appId);
    if (!body || body->len < 9) return std::nullopt;
    Released r;
    r.playerGameTokenId = wire::readI64(body->data);
    r.reason = body->data[8];
    return r;
}

/// FLEET_DELIVERY (53): [1B count][count x 8B player gameTokenId][one notification,
/// exactly as a player's own session would be sent it]. At most 32 players a frame.
struct Delivery {
    uint8_t count = 0;
    const uint8_t* ids = nullptr;        // count x 8 bytes, little endian
    wire::InboundView notification{nullptr, 0};
    int64_t player(size_t i) const { return wire::readI64(ids + i * 8); }
};
inline std::optional<Delivery> parseDelivery(const wire::InboundView& m, int64_t appId) {
    if (m.type() != FLEET_DELIVERY) return std::nullopt;
    auto body = fleetBody(m, appId);
    if (!body || body->len < 1) return std::nullopt;
    Delivery d;
    d.count = body->data[0];
    if (d.count == 0 || d.count > MAX_DELIVERY_PLAYERS) return std::nullopt;
    const size_t idsLen = static_cast<size_t>(d.count) * 8;
    if (body->len < 1 + idsLen + 1) return std::nullopt; // at least a type byte of notification
    d.ids = body->data + 1;
    d.notification = wire::InboundView{body->data + 1 + idsLen, body->len - 1 - idsLen};
    return d;
}

/// COMMAND_RECONNECT (22): [22][32B HMAC over the type byte, keyed with the token].
inline bool verifyReconnect(const uint8_t* data, size_t len, const uint8_t* token64) {
    if (len < 1 + hmac::TAG_SIZE || data[0] != wire::COMMAND_RECONNECT) return false;
    uint8_t expected[hmac::TAG_SIZE];
    return hmacPlain(token64, data, 1, expected) && hmac::tagEquals(expected, data + 1);
}

/// COMMAND_SESSION_RELEASED (28): [28][reason][32B HMAC over the two leading bytes].
/// Returns the reason when the HMAC holds.
inline std::optional<uint8_t> verifySessionReleased(const uint8_t* data, size_t len,
                                                   const uint8_t* token64) {
    if (len < 2 + hmac::TAG_SIZE || data[0] != COMMAND_SESSION_RELEASED) return std::nullopt;
    uint8_t expected[hmac::TAG_SIZE];
    if (!hmacPlain(token64, data, 2, expected) || !hmac::tagEquals(expected, data + 2))
        return std::nullopt;
    return data[1];
}

/// The app payload of a long-form notification (between the 68-byte header and
/// the tail, which is 41 bytes with an HMAC and 9 without).
inline std::optional<FleetBody> notificationPayload(const wire::InboundView& m) {
    if (m.len < wire::HEADER_SIZE + wire::TAIL_NO_HMAC) return std::nullopt;
    const size_t tail = m.data[wire::off::CONTAINS_AUTH] ? wire::TAIL_WITH_HMAC
                                                         : wire::TAIL_NO_HMAC;
    if (m.len < wire::HEADER_SIZE + tail) return std::nullopt;
    return FleetBody{m.data + wire::HEADER_SIZE, m.len - wire::HEADER_SIZE - tail, true};
}

// ---------------------------------------------------------------------------
// The marker in an actor update's payload.
// ---------------------------------------------------------------------------

/// The platform relays an actor update's payload as it is, so the sender can
/// leave a mark the receiver reads. This client sends the 88-byte UE5 state the
/// existing harness sends, which leaves rotation pitch and yaw at zero. The
/// marker uses those two float64 fields, so the state keeps its size and layout:
///   pitch = 2^40 + sampled * 2^36 + runTag * 2^16 + sender session index
///   yaw   = microseconds on this process's steady clock when the update was built
/// Both are whole numbers below 2^53, so a float64 holds them exactly.
/// The receiver learns which session sent the update (for the own-state check)
/// and, for a sampled player, when (for the end-to-end replication latency).
struct Marker {
    uint16_t session = 0;
    uint16_t runTag = 0;
    bool sampled = false;
    int64_t sendUs = 0;
};

inline constexpr double MARKER_BASE = 1099511627776.0; // 2^40

inline double markerCode(uint16_t session, uint16_t runTag, bool sampled) {
    const uint64_t v = (uint64_t{1} << 40) | (sampled ? (uint64_t{1} << 36) : 0) |
                       (static_cast<uint64_t>(runTag) << 16) | session;
    return static_cast<double>(v);
}

/// Read the marker from an 88-byte UE5 actor state. Empty when the state is not
/// one this tool wrote.
inline std::optional<Marker> readMarker(const uint8_t* state, size_t len) {
    if (len != wire::state::SIZE || state[wire::state::VERSION] != wire::state::VERSION_VALUE)
        return std::nullopt;
    const double code = wire::readF64(state + wire::state::ROTATION_PITCH);
    const double when = wire::readF64(state + wire::state::ROTATION_YAW);
    if (!(code >= MARKER_BASE && code < 2 * MARKER_BASE) || code != std::floor(code))
        return std::nullopt;
    if (!(when >= 0 && when < 9.0e15) || when != std::floor(when)) return std::nullopt;
    const uint64_t v = static_cast<uint64_t>(code);
    // Bits 32 to 35 and 37 to 39 are never set by markerCode.
    const uint64_t reserved = (uint64_t{0xF} << 32) | (uint64_t{0x7} << 37);
    if (v & reserved) return std::nullopt;
    Marker m;
    m.session = static_cast<uint16_t>(v & 0xFFFF);
    m.runTag = static_cast<uint16_t>((v >> 16) & 0xFFFF);
    m.sampled = ((v >> 36) & 1) != 0;
    m.sendUs = static_cast<int64_t>(when);
    return m;
}

} // namespace lt::fleet
