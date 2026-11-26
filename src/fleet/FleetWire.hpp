#pragma once

#include "Hmac.hpp"
#include "Wire.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>

/// Wire formats of a fleet session: one realtime session an organization's game
/// server opens for itself, in which it carries its players. This follows the
/// wire as built (realtime servers v0.50.0).
///
/// A message a fleet session sends FOR A PLAYER is the message that player's own
/// session would send, with two differences in the long-form tail:
///   - the 8-byte game token id is the PLAYER's gameTokenId
///   - the HMAC is keyed with the FLEET token (64 octets)
/// Under its OWN game token id a session sends only CLIENT_CAPABILITIES (29),
/// FLEET_CARRY (49), FLEET_RELEASE (50) and CLIENT_ACTOR_HEARTBEAT (26, a
/// keepalive that registers no actor). Anything else under its own id is
/// answered UNAUTHORIZED.
///
/// Everything here builds on Wire.hpp and Hmac.hpp: the long-form layout, the
/// `prefix || token` signing and the bundle walk are theirs.
namespace lt::fleet {

// Message types.
inline constexpr uint8_t CHANNEL_MESSAGE_NOTIFICATION = 18;
inline constexpr uint8_t COMMAND_SESSION_RELEASED = 28;       // [28][reason][32B HMAC]
inline constexpr uint8_t CHANNEL_AUDIO_NOTIFICATION = 36;
inline constexpr uint8_t COMMAND_RECONNECT_BOUND = 44;        // [44][8B nonce][32B HMAC]
inline constexpr uint8_t COMMAND_SESSION_RELEASED_BOUND = 45; // [45][reason][8B nonce][32B HMAC]
inline constexpr uint8_t CONTROL_BOUND_ACK = 46;              // [46][8B nonce][32B HMAC]
inline constexpr uint8_t FLEET_CARRY = 49;                    // client -> server
inline constexpr uint8_t FLEET_RELEASE = 50;                  // client -> server
inline constexpr uint8_t FLEET_CARRY_RESULT = 51;             // server -> client
inline constexpr uint8_t FLEET_RELEASED = 52;                 // server -> client
inline constexpr uint8_t FLEET_DELIVERY = 53;                 // server -> client
inline constexpr uint8_t CLIENT_AUDIO_PACKET = 134;
inline constexpr uint8_t CLIENT_AUDIO_NOTIFICATION = 135;
inline constexpr uint8_t CLIENT_TEXT_NOTIFICATION = 137;
inline constexpr uint8_t CLIENT_VIDEO_NOTIFICATION = 144;
inline constexpr uint8_t ACTOR_LEFT_NOTIFICATION = 145;
inline constexpr uint8_t CHUNK_CHANGED_NOTIFICATION = 146;

/// CLIENT_CAPABILITIES bit 1: send 44 and 45 in place of 22 and 28, bound to the
/// 8-byte nonce that follows the flags.
inline constexpr uint32_t CAP_BOUND_CONTROL = 1u << 1;
/// CLIENT_CAPABILITIES bit 3: "send me state my own carried players sent, when
/// another of my carried players is in range". Off by default.
inline constexpr uint32_t CAP_FLEET_OWN_STATE = 1u << 3;

/// FLEET_CARRY_RESULT status.
enum CarryStatus : uint8_t {
    CARRY_CARRIED = 0,
    CARRY_NO_LIVE_TOKEN = 1,       // also: a token of another app
    CARRY_OTHER_ORGANIZATION = 2,  // another organization's player
    CARRY_PROOF_OR_TIME = 3,       // the proof or its time failed; also: a replay
    CARRY_SESSION_FULL = 4,        // carries as many as it may; also: no place for it
};

/// FLEET_RELEASED reason.
enum ReleaseReason : uint8_t {
    RELEASED_CARRIED_ELSEWHERE = 1, // another session carries the player
    RELEASED_SIGN_IN_ENDED = 2,     // revoked, or expired and not carried again with a live token
    RELEASED_IDLE = 5,              // no gameplay input for the app's idle timeout
};

/// The name a summary gives a status or a reason. "other" for a number the
/// contract does not define.
inline const char* carryStatusName(uint8_t status) {
    switch (status) {
        case CARRY_CARRIED: return "0_carried";
        case CARRY_NO_LIVE_TOKEN: return "1_no_live_token";
        case CARRY_OTHER_ORGANIZATION: return "2_other_organization";
        case CARRY_PROOF_OR_TIME: return "3_proof_time_or_replay";
        case CARRY_SESSION_FULL: return "4_session_full";
        default: return "other";
    }
}
inline const char* releaseReasonName(uint8_t reason) {
    switch (reason) {
        case RELEASED_CARRIED_ELSEWHERE: return "1_carried_by_another_session";
        case RELEASED_SIGN_IN_ENDED: return "2_sign_in_ended";
        case RELEASED_IDLE: return "5_idle";
        default: return "other";
    }
}

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
inline constexpr size_t CAPABILITIES_BOUND_SIZE =
    wire::HEADER_SIZE + 4 + 8 + wire::TAIL_WITH_HMAC;               // 121
inline constexpr size_t CONTROL_NONCE_LEN = 8;

inline constexpr size_t CARRY_RESULT_SIZE = 18;   // [51][8B player][8B user][1B status]
inline constexpr size_t RELEASED_SIZE = 10;       // [52][8B player][1B reason]
inline constexpr size_t MAX_DELIVERY_PLAYERS = 32;
/// The largest datagram a client may send, and the largest a fleet session is
/// sent: a FLEET_DELIVERY with one player, alone, with its own HMAC.
inline constexpr size_t MAX_CLIENT_DATAGRAM = 1232;
inline constexpr size_t MAX_SERVER_DATAGRAM = 1242;
/// A FLEET_DELIVERY above this cannot be a member of a signed bundle.
inline constexpr size_t MAX_BUNDLED_DELIVERY = 1197;

/// Plain HMAC-SHA256(key = the 64 token octets, message). The carry proof and
/// the move commands use this form. The spatial tail and the fleet frames use
/// hmac::spatialSign, whose message is `bytes || token`.
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

// ---------------------------------------------------------------------------
// Client -> server.
// ---------------------------------------------------------------------------

/// Any long-form message: the 68-byte header, the payload, and the 41-byte tail
/// whose game token id is `tailGameTokenId` and whose HMAC is keyed with
/// `fleetToken64`. `buf` must hold HEADER_SIZE + payloadLen + TAIL_WITH_HMAC.
/// Returns the length, 0 on failure.
inline size_t buildLongForm(uint8_t* buf, uint8_t type, int64_t appId, int64_t chunkX,
                            int64_t chunkY, int64_t chunkZ, uint8_t distance, uint8_t decay,
                            const char* uuid32, const uint8_t* payload, size_t payloadLen,
                            int64_t tailGameTokenId, uint8_t seq, const uint8_t* fleetToken64) {
    const size_t hmacOff = wire::HEADER_SIZE + payloadLen;
    const size_t total = hmacOff + wire::TAIL_WITH_HMAC;
    if (total > MAX_CLIENT_DATAGRAM) return 0;
    std::memset(buf, 0, total);
    buf[wire::off::TYPE] = type;
    wire::writeI64(buf + wire::off::APP_ID, appId);
    wire::writeI64(buf + wire::off::CHUNK_X, chunkX);
    wire::writeI64(buf + wire::off::CHUNK_Y, chunkY);
    wire::writeI64(buf + wire::off::CHUNK_Z, chunkZ);
    buf[wire::off::DISTANCE] = distance;
    buf[wire::off::DECAY] = decay;
    buf[wire::off::CONTAINS_AUTH] = 1;
    if (uuid32) std::memcpy(buf + wire::off::UUID, uuid32, 32);
    if (payloadLen) std::memcpy(buf + wire::HEADER_SIZE, payload, payloadLen);
    wire::writeI64(buf + hmacOff + hmac::TAG_SIZE, tailGameTokenId);
    buf[total - 1] = seq;
    if (!hmac::spatialSign(buf, hmacOff, fleetToken64, buf + hmacOff)) return 0;
    return total;
}

/// A session's own control message, exactly as CLIENT_CAPABILITIES is sent:
/// chunk (0,0,0), distance 0, no actor UUID, the session's own gameTokenId in
/// the tail.
inline size_t buildControl(uint8_t* buf, uint8_t type, int64_t appId, const uint8_t* payload,
                           size_t payloadLen, int64_t fleetGameTokenId, uint8_t seq,
                           const uint8_t* fleetToken64) {
    return buildLongForm(buf, type, appId, 0, 0, 0, 0, 0, nullptr, payload, payloadLen,
                         fleetGameTokenId, seq, fleetToken64);
}

/// CLIENT_CAPABILITIES (29): a 4-byte flags word and, with `nonce8`, the 8-byte
/// control nonce of this assignment (CAP_BOUND_CONTROL is then set). Without a
/// nonce the bytes are those of wire::buildCapabilities.
/// `buf` must hold CAPABILITIES_BOUND_SIZE.
inline size_t buildCapabilities(uint8_t* buf, int64_t appId, int64_t fleetGameTokenId,
                                uint32_t flags, const uint8_t* nonce8, uint8_t seq,
                                const uint8_t* fleetToken64) {
    uint8_t payload[4 + CONTROL_NONCE_LEN];
    if (nonce8) flags |= CAP_BOUND_CONTROL;
    std::memcpy(payload, &flags, 4);
    if (nonce8) std::memcpy(payload + 4, nonce8, CONTROL_NONCE_LEN);
    return buildControl(buf, wire::CLIENT_CAPABILITIES, appId, payload,
                        nonce8 ? sizeof(payload) : 4, fleetGameTokenId, seq, fleetToken64);
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

/// FLEET_RELEASE (50). App payload: [8B player gameTokenId]. Not answered; idempotent.
inline size_t buildRelease(uint8_t* buf, int64_t appId, int64_t fleetGameTokenId,
                           int64_t playerGameTokenId, uint8_t seq,
                           const uint8_t* fleetToken64) {
    uint8_t payload[RELEASE_PAYLOAD_LEN];
    wire::writeI64(payload, playerGameTokenId);
    return buildControl(buf, FLEET_RELEASE, appId, payload, sizeof(payload), fleetGameTokenId,
                        seq, fleetToken64);
}

/// The session's own keepalive: CLIENT_ACTOR_HEARTBEAT (26) under the session's
/// own gameTokenId, with no actor and no payload. It registers no actor.
inline size_t buildHeartbeat(uint8_t* buf, int64_t appId, int64_t fleetGameTokenId,
                             uint8_t seq, const uint8_t* fleetToken64) {
    return buildControl(buf, wire::CLIENT_ACTOR_HEARTBEAT, appId, nullptr, 0,
                        fleetGameTokenId, seq, fleetToken64);
}

/// A heartbeat that keeps a CARRIED PLAYER's actor present: CLIENT_ACTOR_HEARTBEAT
/// under the PLAYER's gameTokenId, with the actor's chunk and UUID. It is not
/// gameplay input, so it does not hold off the idle release.
inline size_t buildPlayerHeartbeat(uint8_t* buf, int64_t appId, int64_t chunkX, int64_t chunkY,
                                   int64_t chunkZ, const char* uuid32,
                                   int64_t playerGameTokenId, uint8_t seq,
                                   const uint8_t* fleetToken64) {
    return buildLongForm(buf, wire::CLIENT_ACTOR_HEARTBEAT, appId, chunkX, chunkY, chunkZ, 0, 0,
                         uuid32, nullptr, 0, playerGameTokenId, seq, fleetToken64);
}

// ---------------------------------------------------------------------------
// Server -> client.
// ---------------------------------------------------------------------------

/// How a fleet frame (51, 52, 53) was authenticated.
enum class FrameAuth : uint8_t {
    SignedBundle, // a member of a verified MESSAGE_BUNDLE_SIGNED: no HMAC of its own
    OwnHmac,      // followed by its own 32-byte HMAC, which verified
    Failed,       // outside a signed bundle with no HMAC, or one that does not verify
};

/// A fleet frame without its HMAC, and how it was authenticated.
struct FleetFrame {
    wire::InboundView frame{nullptr, 0};
    FrameAuth auth = FrameAuth::Failed;
};

/// Read one fleet frame. `m` is a whole datagram or one bundle member.
///
/// To a session that advertised BUNDLE_SIGNED the frame is a member of a
/// MESSAGE_BUNDLE_SIGNED and has no HMAC of its own. Otherwise, and for a
/// FLEET_DELIVERY too large to be such a member, its last 32 bytes are
///   HMAC-SHA256(key = fleet token, every byte before the HMAC || fleet token)
/// alone in a datagram or as a member of a MESSAGE_BUNDLE. A session must read
/// both forms.
inline FleetFrame readFleetFrame(const wire::InboundView& m, bool inVerifiedSignedBundle,
                                 const uint8_t* fleetToken64) {
    if (inVerifiedSignedBundle) return FleetFrame{m, FrameAuth::SignedBundle};
    if (m.len <= hmac::TAG_SIZE) return FleetFrame{};
    const size_t body = m.len - hmac::TAG_SIZE;
    uint8_t expected[hmac::TAG_SIZE];
    if (!hmac::spatialSign(m.data, body, fleetToken64, expected) ||
        !hmac::tagEquals(expected, m.data + body))
        return FleetFrame{};
    return FleetFrame{wire::InboundView{m.data, body}, FrameAuth::OwnHmac};
}

/// FLEET_CARRY_RESULT (51): [51][8B player gameTokenId][8B user id][1B status], 18 bytes.
struct CarryResult {
    int64_t playerGameTokenId = 0;
    int64_t userId = 0;
    uint8_t status = 0;
};
inline std::optional<CarryResult> parseCarryResult(const wire::InboundView& frame) {
    if (frame.type() != FLEET_CARRY_RESULT || frame.len != CARRY_RESULT_SIZE) return std::nullopt;
    CarryResult r;
    r.playerGameTokenId = wire::readI64(frame.data + 1);
    r.userId = wire::readI64(frame.data + 9);
    r.status = frame.data[17];
    return r;
}

/// FLEET_RELEASED (52): [52][8B player gameTokenId][1B reason], 10 bytes.
struct Released {
    int64_t playerGameTokenId = 0;
    uint8_t reason = 0;
};
inline std::optional<Released> parseReleased(const wire::InboundView& frame) {
    if (frame.type() != FLEET_RELEASED || frame.len != RELEASED_SIZE) return std::nullopt;
    Released r;
    r.playerGameTokenId = wire::readI64(frame.data + 1);
    r.reason = frame.data[9];
    return r;
}

/// FLEET_DELIVERY (53): [53][1B count][count x 8B player gameTokenId][notification].
/// The notification is what a player's own session would have been sent,
/// unsigned: a long-spatial one keeps its 9-byte tail, has `containsAuth` 0 and no
/// HMAC; a channel one (18, 36) is byte for byte what any session receives. Its
/// length is the frame's minus `2 + 8 x count`. At most 32 players a frame.
struct Delivery {
    uint8_t count = 0;
    const uint8_t* ids = nullptr;        // count x 8 bytes, little endian
    wire::InboundView notification{nullptr, 0};
    int64_t player(size_t i) const { return wire::readI64(ids + i * 8); }
};
inline std::optional<Delivery> parseDelivery(const wire::InboundView& frame) {
    if (frame.type() != FLEET_DELIVERY || frame.len < 2) return std::nullopt;
    Delivery d;
    d.count = frame.data[1];
    if (d.count == 0 || d.count > MAX_DELIVERY_PLAYERS) return std::nullopt;
    const size_t idsLen = static_cast<size_t>(d.count) * 8;
    if (frame.len < 2 + idsLen + 1) return std::nullopt; // at least the notification's type
    d.ids = frame.data + 2;
    d.notification = wire::InboundView{frame.data + 2 + idsLen, frame.len - 2 - idsLen};
    return d;
}

/// A communication: it reaches a fleet session only inside a FLEET_DELIVERY.
/// Spatial voice, text and video, SINGLE_ACTOR_MESSAGE, channel text and audio.
inline bool isCommunication(uint8_t type) {
    return type == CLIENT_AUDIO_NOTIFICATION || type == CLIENT_TEXT_NOTIFICATION ||
           type == CLIENT_VIDEO_NOTIFICATION || type == wire::SINGLE_ACTOR_MESSAGE ||
           type == CHANNEL_MESSAGE_NOTIFICATION || type == CHANNEL_AUDIO_NOTIFICATION;
}

/// State: it reaches a fleet session plain and once. A client's event (138) or
/// an app-defined spatial message (140) that some of the session's players may
/// not receive comes inside a FLEET_DELIVERY listing only those who may.
inline bool isState(uint8_t type) {
    return type == wire::ACTOR_UPDATE_NOTIFICATION_2 || type == wire::VOXEL_UPDATE_NOTIFICATION_2 ||
           type == wire::CLIENT_EVENT_NOTIFICATION_2 || type == wire::SERVER_EVENT_NOTIFICATION_2 ||
           type == wire::GENERIC_SPATIAL_1 || type == ACTOR_LEFT_NOTIFICATION ||
           type == CHUNK_CHANGED_NOTIFICATION;
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

/// The bound forms a session is sent after it advertised CAP_BOUND_CONTROL with
/// a nonce. Each is true only when the HMAC holds AND the nonce is this
/// assignment's: a frame with another nonce was captured earlier and sent again.
///   44: [44][8B nonce][32B HMAC over the first 9 bytes]
///   46: [46][8B nonce][32B HMAC over the first 9 bytes]   (the server's acknowledgement)
inline bool verifyBoundNonceFrame(uint8_t type, const uint8_t* data, size_t len,
                                  const uint8_t* token64, const uint8_t* nonce8) {
    if (len < 1 + CONTROL_NONCE_LEN + hmac::TAG_SIZE || data[0] != type) return false;
    uint8_t expected[hmac::TAG_SIZE];
    if (!hmacPlain(token64, data, 1 + CONTROL_NONCE_LEN, expected) ||
        !hmac::tagEquals(expected, data + 1 + CONTROL_NONCE_LEN))
        return false;
    return CRYPTO_memcmp(data + 1, nonce8, CONTROL_NONCE_LEN) == 0;
}
inline bool verifyReconnectBound(const uint8_t* data, size_t len, const uint8_t* token64,
                                 const uint8_t* nonce8) {
    return verifyBoundNonceFrame(COMMAND_RECONNECT_BOUND, data, len, token64, nonce8);
}
inline bool verifyBoundAck(const uint8_t* data, size_t len, const uint8_t* token64,
                           const uint8_t* nonce8) {
    return verifyBoundNonceFrame(CONTROL_BOUND_ACK, data, len, token64, nonce8);
}
///   45: [45][1B reason][8B nonce][32B HMAC over the first 10 bytes]
inline std::optional<uint8_t> verifySessionReleasedBound(const uint8_t* data, size_t len,
                                                        const uint8_t* token64,
                                                        const uint8_t* nonce8) {
    if (len < 2 + CONTROL_NONCE_LEN + hmac::TAG_SIZE || data[0] != COMMAND_SESSION_RELEASED_BOUND)
        return std::nullopt;
    uint8_t expected[hmac::TAG_SIZE];
    if (!hmacPlain(token64, data, 2 + CONTROL_NONCE_LEN, expected) ||
        !hmac::tagEquals(expected, data + 2 + CONTROL_NONCE_LEN))
        return std::nullopt;
    if (CRYPTO_memcmp(data + 2, nonce8, CONTROL_NONCE_LEN) != 0) return std::nullopt;
    return data[1];
}

/// The app payload of a long-form notification (between the 68-byte header and
/// the tail, which is 41 bytes with an HMAC and 9 without).
struct Payload {
    const uint8_t* data = nullptr;
    size_t len = 0;
};
inline std::optional<Payload> notificationPayload(const wire::InboundView& m) {
    if (m.len < wire::HEADER_SIZE + wire::TAIL_NO_HMAC) return std::nullopt;
    const size_t tail = m.data[wire::off::CONTAINS_AUTH] ? wire::TAIL_WITH_HMAC
                                                         : wire::TAIL_NO_HMAC;
    if (m.len < wire::HEADER_SIZE + tail) return std::nullopt;
    return Payload{m.data + wire::HEADER_SIZE, m.len - wire::HEADER_SIZE - tail};
}

// ---------------------------------------------------------------------------
// The marker in an actor update's payload.
// ---------------------------------------------------------------------------

/// The platform relays an actor update's payload as it is, so the sender can
/// leave a mark the receiver reads. This client sends the 88-byte UE5 state the
/// existing harness sends, which leaves rotation pitch and yaw at zero. The
/// marker uses those two float64 fields, so the state keeps its size and layout:
///   pitch = 2^52 + player * 2^28 + sampled * 2^27 + first * 2^26 + runTag * 2^16
///           + sender session
///   yaw   = microseconds on this process's steady clock when the update was sent
/// Both are whole numbers below 2^53, so a float64 holds them exactly.
///
/// So every update a session receives says WHICH SESSION SENT IT, for which
/// player, when, and whether it was the first update that session sent for the
/// player after a FLEET_CARRY_RESULT. That is what lets the client tell state a
/// session sent itself (which must not come back to it) from state another
/// session sent for a player the receiver has carried since, and measure how
/// long an update took.
struct Marker {
    uint16_t session = 0;   // the session that sent the update
    uint16_t runTag = 0;    // 10 bits: this run's, so another run's updates are not read as ours
    bool first = false;     // the sender's first update for the player after a carry result
    bool sampled = false;   // one of the players whose latency is measured
    int32_t player = 0;     // the player the update is for (24 bits)
    int64_t sendUs = 0;
};

inline constexpr double MARKER_BASE = 4503599627370496.0; // 2^52
inline constexpr uint32_t MARKER_RUN_TAG_MASK = 0x3FF;
inline constexpr int32_t MARKER_MAX_PLAYER = (1 << 24) - 1;

inline double markerCode(uint16_t session, uint16_t runTag, bool sampled, int32_t player,
                         bool first = false) {
    const uint64_t v = (uint64_t{1} << 52) |
                       ((static_cast<uint64_t>(player) & 0xFFFFFF) << 28) |
                       (sampled ? (uint64_t{1} << 27) : 0) | (first ? (uint64_t{1} << 26) : 0) |
                       ((static_cast<uint64_t>(runTag) & MARKER_RUN_TAG_MASK) << 16) | session;
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
    Marker m;
    m.session = static_cast<uint16_t>(v & 0xFFFF);
    m.runTag = static_cast<uint16_t>((v >> 16) & MARKER_RUN_TAG_MASK);
    m.first = ((v >> 26) & 1) != 0;
    m.sampled = ((v >> 27) & 1) != 0;
    m.player = static_cast<int32_t>((v >> 28) & 0xFFFFFF);
    m.sendUs = static_cast<int64_t>(when);
    return m;
}

} // namespace lt::fleet
