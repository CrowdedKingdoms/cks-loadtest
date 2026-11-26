// Unit tests for the fleet session's wire formats (src/fleet/FleetWire.hpp).
//
// Every builder is checked against bytes computed outside this repository's C++
// (tests/fleet_vectors.hpp says how), and the carry proof a second time against
// an HMAC built here from SHA-256 alone, so the check does not lean on the same
// OpenSSL HMAC call the client uses.

#include "fleet/FleetWire.hpp"
#include "fleet_vectors.hpp"

#include <openssl/evp.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace lt;
using namespace lt::fleet;

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

std::string toHex(const uint8_t* data, size_t len) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0xF]);
    }
    return out;
}

std::vector<uint8_t> fromHex(const std::string& hex) {
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    }
    return out;
}

std::vector<uint8_t> sha256(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out(32);
    unsigned int len = 0;
    EVP_Digest(in.data(), in.size(), out.data(), &len, EVP_sha256(), nullptr);
    return out;
}

/// HMAC-SHA256 from its definition (RFC 2104): H((K ^ opad) || H((K ^ ipad) || m)).
/// The key is 64 octets, exactly one SHA-256 block, so it is used as it is.
std::vector<uint8_t> hmacByHand(const uint8_t* key64, const std::vector<uint8_t>& msg) {
    std::vector<uint8_t> inner(64), outer(64);
    for (int i = 0; i < 64; ++i) {
        inner[static_cast<size_t>(i)] = key64[i] ^ 0x36;
        outer[static_cast<size_t>(i)] = key64[i] ^ 0x5c;
    }
    inner.insert(inner.end(), msg.begin(), msg.end());
    const auto innerHash = sha256(inner);
    outer.insert(outer.end(), innerHash.begin(), innerHash.end());
    return sha256(outer);
}

void appendI64(std::vector<uint8_t>& v, int64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>((static_cast<uint64_t>(x) >> (8 * i)) & 0xFF));
}

const auto* FLEET = reinterpret_cast<const uint8_t*>(fleet_vectors::FLEET_TOKEN);
const auto* PLAYER = reinterpret_cast<const uint8_t*>(fleet_vectors::PLAYER_TOKEN);
constexpr int64_t APP = fleet_vectors::APP_ID;
constexpr int64_t FLEET_ID = fleet_vectors::FLEET_ID;
constexpr int64_t PLAYER_ID = fleet_vectors::PLAYER_ID;
constexpr int64_t WHEN = fleet_vectors::EPOCH_MS;
constexpr uint8_t SEQ = fleet_vectors::SEQ;

void testCarryProof() {
    check(std::strlen(fleet_vectors::FLEET_TOKEN) == 64 &&
              std::strlen(fleet_vectors::PLAYER_TOKEN) == 64,
          "the two tokens are 64 octets");

    uint8_t proof[hmac::TAG_SIZE];
    check(carryProof(PLAYER, FLEET_ID, PLAYER_ID, WHEN, proof), "carry proof: computes");
    check(toHex(proof, sizeof(proof)) == fleet_vectors::PROOF_HEX,
          "carry proof: matches the answer computed outside this code");

    // Independently: the 14 ASCII bytes, then the three numbers, 8 bytes each,
    // little endian, under an HMAC built from SHA-256 by hand.
    std::vector<uint8_t> msg = {'c', 'k', '-', 'f', 'l', 'e', 'e', 't', '-', 'c', 'a', 'r', 'r', 'y'};
    check(msg.size() == 14, "carry proof: the label is 14 bytes");
    appendI64(msg, FLEET_ID);
    appendI64(msg, PLAYER_ID);
    appendI64(msg, WHEN);
    check(msg.size() == 38, "carry proof: the message is 38 bytes");
    const auto byHand = hmacByHand(PLAYER, msg);
    check(std::memcmp(byHand.data(), proof, 32) == 0,
          "carry proof: equals HMAC-SHA256 built from SHA-256 by hand");

    // The proof is keyed with the PLAYER's token: the fleet token gives another.
    const auto wrongKey = hmacByHand(FLEET, msg);
    check(std::memcmp(wrongKey.data(), proof, 32) != 0,
          "carry proof: the fleet token does not produce it");
    // Every input matters.
    uint8_t other[hmac::TAG_SIZE];
    carryProof(PLAYER, FLEET_ID + 1, PLAYER_ID, WHEN, other);
    check(std::memcmp(other, proof, 32) != 0, "carry proof: depends on the fleet gameTokenId");
    carryProof(PLAYER, FLEET_ID, PLAYER_ID + 1, WHEN, other);
    check(std::memcmp(other, proof, 32) != 0, "carry proof: depends on the player gameTokenId");
    carryProof(PLAYER, FLEET_ID, PLAYER_ID, WHEN + 1, other);
    check(std::memcmp(other, proof, 32) != 0, "carry proof: depends on the time");
}

void testCarryBytes() {
    uint8_t buf[FLEET_CARRY_SIZE];
    const size_t n = buildCarry(buf, APP, FLEET_ID, PLAYER_ID, WHEN, PLAYER, SEQ, FLEET);
    check(n == 157 && FLEET_CARRY_SIZE == 157, "FLEET_CARRY: 68 + 48 + 41 = 157 bytes");
    const std::string got = toHex(buf, n);
    if (got != fleet_vectors::CARRY_HEX)
        std::fprintf(stderr, "expected: %s\n     got: %s\n", fleet_vectors::CARRY_HEX, got.c_str());
    check(got == fleet_vectors::CARRY_HEX, "FLEET_CARRY: matches the bytes computed outside this code");

    // The layout, field by field.
    check(buf[wire::off::TYPE] == 49, "FLEET_CARRY: type 49");
    check(wire::readI64(buf + wire::off::APP_ID) == APP, "FLEET_CARRY: the app id");
    bool zero = true;
    for (size_t i = wire::off::CHUNK_X; i < wire::off::CONTAINS_AUTH; ++i) zero = zero && buf[i] == 0;
    check(zero, "FLEET_CARRY: chunk (0,0,0), distance 0, decay 0");
    check(buf[wire::off::CONTAINS_AUTH] == 1, "FLEET_CARRY: containsAuth");
    zero = true;
    for (size_t i = wire::off::UUID; i < wire::HEADER_SIZE; ++i) zero = zero && buf[i] == 0;
    check(zero, "FLEET_CARRY: no actor UUID");
    check(wire::readI64(buf + 68) == PLAYER_ID, "FLEET_CARRY: payload starts with the player's gameTokenId");
    check(wire::readI64(buf + 76) == WHEN, "FLEET_CARRY: then the epoch millis");
    check(toHex(buf + 84, 32) == fleet_vectors::PROOF_HEX, "FLEET_CARRY: then the 32-byte proof");
    check(wire::readI64(buf + 148) == FLEET_ID,
          "FLEET_CARRY: the tail carries the fleet session's own gameTokenId");
    check(buf[156] == SEQ, "FLEET_CARRY: the sequence number is last");

    // The tail's HMAC is the client scheme, keyed with the fleet token.
    uint8_t expected[hmac::TAG_SIZE];
    check(hmac::spatialSign(buf, 116, FLEET, expected) && hmac::tagEquals(expected, buf + 116),
          "FLEET_CARRY: HMAC(fleet token, prefix || fleet token)");
    check(hmac::spatialSign(buf, 116, PLAYER, expected) && !hmac::tagEquals(expected, buf + 116),
          "FLEET_CARRY: the player's token does not sign the datagram");

    // A resend carries a fresh time, so a fresh proof.
    uint8_t again[FLEET_CARRY_SIZE];
    buildCarry(again, APP, FLEET_ID, PLAYER_ID, WHEN + 250, PLAYER, SEQ + 1, FLEET);
    check(std::memcmp(buf + 84, again + 84, 32) != 0, "FLEET_CARRY: a resend has a new proof");
}

void testOtherControlBytes() {
    uint8_t rel[FLEET_RELEASE_SIZE];
    const size_t n = buildRelease(rel, APP, FLEET_ID, PLAYER_ID, SEQ, FLEET);
    check(n == 117 && toHex(rel, n) == fleet_vectors::RELEASE_HEX,
          "FLEET_RELEASE: 117 bytes, matches the bytes computed outside this code");
    check(rel[0] == 50 && wire::readI64(rel + 68) == PLAYER_ID && wire::readI64(rel + 108) == FLEET_ID,
          "FLEET_RELEASE: type 50, the player in the payload, the session in the tail");

    uint8_t hb[HEARTBEAT_SIZE];
    const size_t h = buildHeartbeat(hb, APP, FLEET_ID, SEQ, FLEET);
    check(h == 109 && toHex(hb, h) == fleet_vectors::HEARTBEAT_HEX,
          "heartbeat: 109 bytes, matches the bytes computed outside this code");
    check(hb[0] == wire::CLIENT_ACTOR_HEARTBEAT && wire::readI64(hb + 100) == FLEET_ID,
          "heartbeat: type 26 with the session's own gameTokenId");

    // The session's capabilities are built by the harness's own builder.
    uint8_t caps[wire::CAPABILITIES_SIZE];
    check(wire::buildCapabilities(caps, APP, FLEET_ID, wire::CAP_BUNDLE_SIGNED | CAP_FLEET_OWN_STATE,
                                  SEQ, FLEET) &&
              toHex(caps, sizeof(caps)) == fleet_vectors::CAPABILITIES_HEX,
          "capabilities: BUNDLE_SIGNED | FLEET_OWN_STATE matches the bytes computed outside this code");
    uint32_t flags = 0;
    std::memcpy(&flags, caps + wire::HEADER_SIZE, 4);
    check(CAP_FLEET_OWN_STATE == 8 && flags == 9, "capabilities: FLEET_OWN_STATE is bit 3 (value 8)");

    // This client's own builder: the same bytes without a nonce, and with one the
    // BOUND_CONTROL bit and the 8-byte nonce after the flags.
    uint8_t mine[CAPABILITIES_BOUND_SIZE];
    size_t len = buildCapabilities(mine, APP, FLEET_ID, wire::CAP_BUNDLE_SIGNED | CAP_FLEET_OWN_STATE,
                                   nullptr, SEQ, FLEET);
    check(len == wire::CAPABILITIES_SIZE && toHex(mine, len) == fleet_vectors::CAPABILITIES_HEX,
          "capabilities: without a nonce, the harness's bytes");
    const uint8_t nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    len = buildCapabilities(mine, APP, FLEET_ID, wire::CAP_BUNDLE_SIGNED | CAP_FLEET_OWN_STATE, nonce,
                            SEQ, FLEET);
    check(len == 121 && toHex(mine, len) == fleet_vectors::CAPABILITIES_BOUND_HEX,
          "capabilities: with a nonce, 121 bytes matching the bytes computed outside this code");
    std::memcpy(&flags, mine + wire::HEADER_SIZE, 4);
    check(CAP_BOUND_CONTROL == 2 && flags == 11 && std::memcmp(mine + 72, nonce, 8) == 0 &&
              wire::readI64(mine + 112) == FLEET_ID,
          "capabilities: BOUND_CONTROL is bit 1, the nonce follows the flags, the tail is the session's");

    // A heartbeat that keeps a carried player's actor present goes under the PLAYER's id.
    uint8_t phb[HEARTBEAT_SIZE];
    len = buildPlayerHeartbeat(phb, APP, 1, -2, 3, "0123456789ABCDEF0123456789ABCDEF", PLAYER_ID, SEQ,
                               FLEET);
    check(len == 109 && toHex(phb, len) == fleet_vectors::PLAYER_HEARTBEAT_HEX,
          "player heartbeat: matches the bytes computed outside this code");
    check(phb[0] == wire::CLIENT_ACTOR_HEARTBEAT && wire::readI64(phb + 100) == PLAYER_ID &&
              wire::readI64(hb + 100) == FLEET_ID && std::memcmp(phb + wire::off::UUID, "0123", 4) == 0,
          "player heartbeat: the player's id and actor; the session's keepalive has its own id and no actor");
    uint8_t expected[hmac::TAG_SIZE];
    check(hmac::spatialSign(phb, 68, FLEET, expected) && hmac::tagEquals(expected, phb + 68) &&
              hmac::spatialSign(phb, 68, PLAYER, expected) && !hmac::tagEquals(expected, phb + 68),
          "player heartbeat: signed with the fleet token");
}

void testGoldenVectors() {
    // The vectors the contract fixes for every client. The C++ SDK's tests hold the same.
    const auto* fleet = reinterpret_cast<const uint8_t*>(fleet_vectors::GOLDEN_FLEET_TOKEN);
    const auto* player = reinterpret_cast<const uint8_t*>(fleet_vectors::GOLDEN_PLAYER_TOKEN);
    check(std::strlen(fleet_vectors::GOLDEN_FLEET_TOKEN) == 64 &&
              std::strlen(fleet_vectors::GOLDEN_PLAYER_TOKEN) == 64,
          "golden: both tokens are 64 octets");
    uint8_t proof[hmac::TAG_SIZE];
    carryProof(player, fleet_vectors::GOLDEN_FLEET_ID, fleet_vectors::GOLDEN_PLAYER_ID,
               fleet_vectors::GOLDEN_EPOCH_MS, proof);
    check(toHex(proof, 32) == fleet_vectors::GOLDEN_PROOF_HEX, "golden: the carry proof");

    uint8_t buf[MAX_CLIENT_DATAGRAM];
    size_t len = buildCarry(buf, APP, fleet_vectors::GOLDEN_FLEET_ID, fleet_vectors::GOLDEN_PLAYER_ID,
                            fleet_vectors::GOLDEN_EPOCH_MS, player, 0, fleet);
    check(len == 157 && toHex(buf, len) == fleet_vectors::GOLDEN_CARRY_HEX,
          "golden: FLEET_CARRY, 157 bytes");
    len = buildRelease(buf, APP, fleet_vectors::GOLDEN_FLEET_ID, fleet_vectors::GOLDEN_PLAYER_ID, 2,
                       fleet);
    check(len == 117 && toHex(buf, len) == fleet_vectors::GOLDEN_RELEASE_HEX,
          "golden: FLEET_RELEASE, 117 bytes");
    const uint8_t payload[4] = {0xde, 0xad, 0xbe, 0xef};
    len = buildLongForm(buf, wire::ACTOR_UPDATE_REQUEST_2, APP, 1, -2, 3, 8, 0,
                        "0123456789abcdef0123456789abcdef", payload, sizeof(payload),
                        fleet_vectors::GOLDEN_PLAYER_ID, 1, fleet);
    check(len == 113 && toHex(buf, len) == fleet_vectors::GOLDEN_UPDATE_HEX,
          "golden: a carried player's actor update, 113 bytes");
    const wire::InboundView view{buf, len};
    check(wire::verifyNotification(view, fleet) && !wire::verifyNotification(view, player) &&
              wire::readI64(buf + len - 9) == fleet_vectors::GOLDEN_PLAYER_ID,
          "golden: its tail names the player and its HMAC verifies with the fleet token only");
    check(buildLongForm(buf, wire::CLIENT_TEXT_PACKET_2, APP, 0, 0, 0, 8, 0, nullptr, buf, 1124,
                        PLAYER_ID, 0, fleet) == 0,
          "long form: a message that would pass 1,232 bytes is not built");
}

void testCarriedPlayersUpdate() {
    // The same update wire_test.cpp builds for a player's own session, sent by a
    // fleet session for a carried player.
    uint8_t buf[wire::ACTOR_UPDATE_MAX_SIZE];
    wire::initActorUpdateTemplateFmt(buf, wire::PoseFormat::UE5,
                                     "0123456789ABCDEF0123456789ABCDEF", APP,
                                     /*gameTokenId: the PLAYER's*/ PLAYER_ID, 8,
                                     wire::DecayRate::EXPONENTIAL);
    wire::ActorPose pose;
    pose.posX = 100.5;
    pose.posY = -200.25;
    pose.posZ = -60.0;
    pose.roll = 45.0;
    pose.velX = 150.0;
    check(wire::finalizeActorUpdateFmt(buf, wire::PoseFormat::UE5, 1, -2, 3, pose, SEQ,
                                       /*key: the FLEET token*/ FLEET),
          "carried update: builds");
    const std::string got = toHex(buf, wire::ACTOR_UPDATE_SIZE);
    if (got != fleet_vectors::UPDATE_HEX)
        std::fprintf(stderr, "expected: %s\n     got: %s\n", fleet_vectors::UPDATE_HEX, got.c_str());
    check(got == fleet_vectors::UPDATE_HEX,
          "carried update: matches the bytes computed outside this code");
    check(wire::readI64(buf + wire::ACTOR_UPDATE_TOKEN_ID_OFFSET) == PLAYER_ID,
          "carried update: the tail carries the player's gameTokenId");
    check(wire::readI64(buf + wire::ACTOR_UPDATE_TOKEN_ID_OFFSET) != FLEET_ID,
          "carried update: and so differs from the session's own");

    const wire::InboundView view{buf, wire::ACTOR_UPDATE_SIZE};
    check(wire::verifyNotification(view, FLEET), "carried update: its HMAC verifies with the fleet token");
    check(!wire::verifyNotification(view, PLAYER),
          "carried update: its HMAC does not verify with the player's token");
    // By hand as well: HMAC(fleet token, prefix || fleet token).
    std::vector<uint8_t> signedBytes(buf, buf + wire::ACTOR_UPDATE_HMAC_OFFSET);
    signedBytes.insert(signedBytes.end(), FLEET, FLEET + 64);
    const auto byHand = hmacByHand(FLEET, signedBytes);
    check(std::memcmp(byHand.data(), buf + wire::ACTOR_UPDATE_HMAC_OFFSET, 32) == 0,
          "carried update: the HMAC equals one built from SHA-256 by hand with the fleet token");

    // The same message from the player's own session differs only in the HMAC.
    uint8_t own[wire::ACTOR_UPDATE_MAX_SIZE];
    wire::initActorUpdateTemplateFmt(own, wire::PoseFormat::UE5,
                                     "0123456789ABCDEF0123456789ABCDEF", APP, PLAYER_ID, 8,
                                     wire::DecayRate::EXPONENTIAL);
    wire::finalizeActorUpdateFmt(own, wire::PoseFormat::UE5, 1, -2, 3, pose, SEQ, PLAYER);
    check(std::memcmp(own, buf, wire::ACTOR_UPDATE_HMAC_OFFSET) == 0 &&
              std::memcmp(own + wire::ACTOR_UPDATE_TOKEN_ID_OFFSET,
                          buf + wire::ACTOR_UPDATE_TOKEN_ID_OFFSET, 9) == 0 &&
              std::memcmp(own + wire::ACTOR_UPDATE_HMAC_OFFSET, buf + wire::ACTOR_UPDATE_HMAC_OFFSET,
                          32) != 0,
          "carried update: costs no more bytes than the player's own, and differs in the HMAC only");
}

/// An unsigned long-spatial notification, as a player's own session is sent it
/// inside a signed bundle and as a FLEET_DELIVERY wraps it: the 68-byte header
/// with containsAuth 0, the payload, and the 9-byte tail.
std::vector<uint8_t> notificationOf(uint8_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> m(wire::HEADER_SIZE, 0);
    m[0] = type;
    wire::writeI64(m.data() + wire::off::APP_ID, APP);
    wire::writeI64(m.data() + wire::off::CHUNK_X, 1);
    wire::writeI64(m.data() + wire::off::CHUNK_Y, -2);
    wire::writeI64(m.data() + wire::off::CHUNK_Z, 3);
    m[wire::off::DISTANCE] = 8;
    m[wire::off::DECAY] = 1;
    std::memcpy(m.data() + wire::off::UUID, "0123456789ABCDEF0123456789ABCDEF", 32);
    m.insert(m.end(), payload.begin(), payload.end());
    appendI64(m, 1795000000999LL); // epoch millis
    m.push_back(5);                // sequence
    return m;
}

/// The frame followed by its own HMAC: HMAC(fleet token, frame || fleet token), by hand.
std::vector<uint8_t> withOwnHmac(std::vector<uint8_t> frame) {
    std::vector<uint8_t> signedBytes = frame;
    signedBytes.insert(signedBytes.end(), FLEET, FLEET + 64);
    const auto mac = hmacByHand(FLEET, signedBytes);
    frame.insert(frame.end(), mac.begin(), mac.end());
    return frame;
}

wire::InboundView viewOf(const std::vector<uint8_t>& v) {
    return wire::InboundView{v.data(), v.size()};
}

void testCarryResultParsing() {
    std::vector<uint8_t> frame = {FLEET_CARRY_RESULT};
    appendI64(frame, PLAYER_ID);
    appendI64(frame, 4242);
    frame.push_back(0);

    // As a member of a verified signed bundle: 18 bytes, no HMAC of its own.
    FleetFrame f = readFleetFrame(viewOf(frame), /*inVerifiedSignedBundle*/ true, FLEET);
    check(frame.size() == 18 && CARRY_RESULT_SIZE == 18 && f.auth == FrameAuth::SignedBundle,
          "FLEET_CARRY_RESULT: 18 bytes inside a signed bundle");
    auto r = parseCarryResult(f.frame);
    check(r && r->playerGameTokenId == PLAYER_ID && r->userId == 4242 && r->status == CARRY_CARRIED,
          "FLEET_CARRY_RESULT: [51][8B player][8B user][1B status]");

    // To a session that did not advertise BUNDLE_SIGNED: followed by its own HMAC, 50 bytes.
    const auto signedFrame = withOwnHmac(frame);
    check(signedFrame.size() == 50 && toHex(signedFrame.data(), signedFrame.size()) ==
                                          fleet_vectors::CARRY_RESULT_SIGNED_HEX,
          "FLEET_CARRY_RESULT: with its own HMAC it is 50 bytes, as computed outside this code");
    f = readFleetFrame(viewOf(signedFrame), false, FLEET);
    r = parseCarryResult(f.frame);
    check(f.auth == FrameAuth::OwnHmac && f.frame.len == 18 && r && r->playerGameTokenId == PLAYER_ID &&
              r->userId == 4242 && r->status == 0,
          "FLEET_CARRY_RESULT: its own HMAC verifies with the fleet token and the frame reads back");
    check(readFleetFrame(viewOf(signedFrame), false, PLAYER).auth == FrameAuth::Failed,
          "FLEET_CARRY_RESULT: the HMAC does not verify with a player's token");
    auto tampered = signedFrame;
    tampered[17] = 4; // the status
    check(readFleetFrame(viewOf(tampered), false, FLEET).auth == FrameAuth::Failed,
          "FLEET_CARRY_RESULT: a rewritten status fails the HMAC");
    tampered = signedFrame;
    tampered[40] ^= 1; // a byte of the HMAC
    check(readFleetFrame(viewOf(tampered), false, FLEET).auth == FrameAuth::Failed,
          "FLEET_CARRY_RESULT: a flipped HMAC byte fails");
    check(readFleetFrame(viewOf(frame), false, FLEET).auth == FrameAuth::Failed,
          "FLEET_CARRY_RESULT: outside a signed bundle, a frame with no HMAC is refused");

    for (uint8_t status : {uint8_t{1}, uint8_t{2}, uint8_t{3}, uint8_t{4}}) {
        frame[9] = frame[10] = 0; // the user id is 0 unless carried
        frame[17] = status;
        r = parseCarryResult(viewOf(frame));
        check(r && r->status == status && r->userId == 0, "FLEET_CARRY_RESULT: a refusal status reads back");
    }
    auto truncated = frame;
    truncated.pop_back();
    check(!parseCarryResult(viewOf(truncated)), "FLEET_CARRY_RESULT: 17 bytes is not a frame");
    auto longer = frame;
    longer.push_back(0);
    check(!parseCarryResult(viewOf(longer)), "FLEET_CARRY_RESULT: 19 bytes is not a frame");
    auto wrongType = frame;
    wrongType[0] = FLEET_RELEASED;
    check(!parseCarryResult(viewOf(wrongType)), "FLEET_CARRY_RESULT: another type is refused");

    // The names a summary reports carries by.
    check(std::string(carryStatusName(0)) == "0_carried" &&
              std::string(carryStatusName(1)) == "1_no_live_token" &&
              std::string(carryStatusName(2)) == "2_other_organization" &&
              std::string(carryStatusName(3)) == "3_proof_time_or_replay" &&
              std::string(carryStatusName(4)) == "4_session_full" &&
              std::string(carryStatusName(9)) == "other",
          "FLEET_CARRY_RESULT: the five statuses have the contract's names");
}

void testReleasedParsing() {
    std::vector<uint8_t> frame = {FLEET_RELEASED};
    appendI64(frame, PLAYER_ID);
    frame.push_back(RELEASED_CARRIED_ELSEWHERE);
    auto r = parseReleased(viewOf(frame));
    check(frame.size() == 10 && RELEASED_SIZE == 10 && r && r->playerGameTokenId == PLAYER_ID &&
              r->reason == 1,
          "FLEET_RELEASED: [52][8B player][1B reason], reason 1 another session carries the player");

    const auto signedFrame = withOwnHmac(frame);
    check(signedFrame.size() == 42 &&
              toHex(signedFrame.data(), signedFrame.size()) == fleet_vectors::RELEASED_SIGNED_HEX,
          "FLEET_RELEASED: with its own HMAC it is 42 bytes, as computed outside this code");
    const FleetFrame f = readFleetFrame(viewOf(signedFrame), false, FLEET);
    r = parseReleased(f.frame);
    check(f.auth == FrameAuth::OwnHmac && r && r->playerGameTokenId == PLAYER_ID && r->reason == 1,
          "FLEET_RELEASED: its own HMAC verifies and the frame reads back");
    auto tampered = signedFrame;
    tampered[9] = 5; // the reason
    check(readFleetFrame(viewOf(tampered), false, FLEET).auth == FrameAuth::Failed,
          "FLEET_RELEASED: a rewritten reason fails the HMAC");

    for (uint8_t reason : {uint8_t{2}, uint8_t{5}}) {
        frame[9] = reason;
        r = parseReleased(viewOf(frame));
        check(r && r->reason == reason, "FLEET_RELEASED: reasons 2 and 5 read back");
    }
    auto truncated = frame;
    truncated.pop_back();
    check(!parseReleased(viewOf(truncated)), "FLEET_RELEASED: 9 bytes is not a frame");
    check(std::string(releaseReasonName(1)) == "1_carried_by_another_session" &&
              std::string(releaseReasonName(2)) == "2_sign_in_ended" &&
              std::string(releaseReasonName(5)) == "5_idle" &&
              std::string(releaseReasonName(3)) == "other",
          "FLEET_RELEASED: the three reasons have the contract's names");
}

void testDeliveryParsing() {
    // The notification inside is the unsigned form: its 9-byte tail, containsAuth 0, no HMAC.
    const std::vector<uint8_t> text = {'h', 'e', 'l', 'l', 'o'};
    const auto notification = notificationOf(CLIENT_TEXT_NOTIFICATION, text);
    check(notification.size() == 68 + 5 + 9 && notification[wire::off::CONTAINS_AUTH] == 0,
          "FLEET_DELIVERY: the wrapped notification is 68 + payload + 9 bytes with containsAuth 0");

    std::vector<uint8_t> frame = {FLEET_DELIVERY, 2};
    for (int64_t id : {PLAYER_ID, PLAYER_ID + 1}) appendI64(frame, id);
    frame.insert(frame.end(), notification.begin(), notification.end());

    auto d = parseDelivery(viewOf(frame));
    check(d && d->count == 2 && d->player(0) == PLAYER_ID && d->player(1) == PLAYER_ID + 1,
          "FLEET_DELIVERY: [53][1B count][count x 8B player gameTokenId]");
    check(d && d->notification.len == frame.size() - (2 + 8 * 2) &&
              d->notification.len == notification.size() &&
              std::memcmp(d->notification.data, notification.data(), notification.size()) == 0 &&
              d->notification.type() == 137,
          "FLEET_DELIVERY: the notification's length is the frame's minus 2 + 8 x count");
    const auto inner = d ? notificationPayload(d->notification) : std::nullopt;
    check(inner && inner->len == 5 && std::memcmp(inner->data, "hello", 5) == 0,
          "FLEET_DELIVERY: the notification's own payload is found before its 9-byte tail");

    // With its own HMAC, after the wrapped notification: 2 + 8 x count + notification + 32.
    const auto signedFrame = withOwnHmac(frame);
    check(signedFrame.size() == 2 + 16 + 82 + 32 &&
              toHex(signedFrame.data(), signedFrame.size()) == fleet_vectors::DELIVERY_SIGNED_HEX,
          "FLEET_DELIVERY: with its own HMAC it is 132 bytes here, as computed outside this code");
    FleetFrame f = readFleetFrame(viewOf(signedFrame), false, FLEET);
    d = parseDelivery(f.frame);
    check(f.auth == FrameAuth::OwnHmac && d && d->count == 2 && d->notification.len == notification.size(),
          "FLEET_DELIVERY: its own HMAC verifies, and the notification ends before it");
    auto tampered = signedFrame;
    tampered[2] ^= 1; // a listed player
    check(readFleetFrame(viewOf(tampered), false, FLEET).auth == FrameAuth::Failed,
          "FLEET_DELIVERY: a rewritten player id fails the HMAC");

    // A channel notification inside is byte for byte what any session receives.
    std::vector<uint8_t> channel = {CHANNEL_MESSAGE_NOTIFICATION};
    appendI64(channel, 100);                       // channelId
    channel.insert(channel.end(), 32, 'u');        // sender uuid
    channel.push_back(2);                          // payload length, little endian
    channel.push_back(0);
    channel.push_back('h');
    channel.push_back('i');
    appendI64(channel, 1795000000999LL);
    channel.push_back(7);
    std::vector<uint8_t> channelFrame = {FLEET_DELIVERY, 1};
    appendI64(channelFrame, PLAYER_ID);
    channelFrame.insert(channelFrame.end(), channel.begin(), channel.end());
    d = parseDelivery(viewOf(channelFrame));
    check(d && d->count == 1 && d->notification.type() == 18 && d->notification.len == channel.size() &&
              std::memcmp(d->notification.data, channel.data(), channel.size()) == 0,
          "FLEET_DELIVERY: a channel notification inside is byte for byte");

    auto none = frame;
    none[1] = 0;
    check(!parseDelivery(viewOf(none)), "FLEET_DELIVERY: a count of 0 is refused");
    auto tooMany = frame;
    tooMany[1] = 33;
    check(!parseDelivery(viewOf(tooMany)), "FLEET_DELIVERY: more than 32 players is refused");
    std::vector<uint8_t> shortFrame(frame.begin(), frame.begin() + 2 + 16); // ids, no notification
    check(!parseDelivery(viewOf(shortFrame)), "FLEET_DELIVERY: a frame with no notification is refused");

    // 32 players, the most a frame holds.
    std::vector<uint8_t> full = {FLEET_DELIVERY, 32};
    for (int i = 0; i < 32; ++i) appendI64(full, PLAYER_ID + i);
    full.insert(full.end(), notification.begin(), notification.end());
    d = parseDelivery(viewOf(full));
    check(d && d->count == 32 && d->player(31) == PLAYER_ID + 31 &&
              d->notification.len == notification.size(),
          "FLEET_DELIVERY: 32 players a frame");

    // The largest datagram a fleet session is sent: a 53 with one player, wrapping the
    // largest notification, alone, with its own HMAC. 1,242 bytes.
    const std::vector<uint8_t> big(MAX_CLIENT_DATAGRAM - wire::HEADER_SIZE - wire::TAIL_WITH_HMAC, 'x');
    const auto bigNotification = notificationOf(CLIENT_TEXT_NOTIFICATION, big);
    std::vector<uint8_t> bigFrame = {FLEET_DELIVERY, 1};
    appendI64(bigFrame, PLAYER_ID);
    bigFrame.insert(bigFrame.end(), bigNotification.begin(), bigNotification.end());
    const auto bigSigned = withOwnHmac(bigFrame);
    check(big.size() == 1123 && bigFrame.size() > MAX_BUNDLED_DELIVERY && bigSigned.size() == 1242 &&
              MAX_SERVER_DATAGRAM == 1242,
          "FLEET_DELIVERY: the largest is 1,242 bytes, too large to be a member of a signed bundle");
    f = readFleetFrame(viewOf(bigSigned), false, FLEET);
    d = parseDelivery(f.frame);
    const auto bigInner = d ? notificationPayload(d->notification) : std::nullopt;
    check(f.auth == FrameAuth::OwnHmac && d && d->count == 1 && bigInner && bigInner->len == 1123,
          "FLEET_DELIVERY: a 1,242-byte datagram is read, and its HMAC verified");
    // The harness's walk takes a datagram that is not a bundle as one message.
    int walked = 0;
    wire::forEachMessage(bigSigned.data(), bigSigned.size(), [&](const wire::InboundView& m) {
        if (m.len == bigSigned.size() && m.type() == FLEET_DELIVERY) ++walked;
    });
    check(walked == 1, "FLEET_DELIVERY: alone in its datagram, it is the datagram");

    // Form two: members of a MESSAGE_BUNDLE_SIGNED, none with an HMAC of its own.
    std::vector<uint8_t> result = {FLEET_CARRY_RESULT};
    appendI64(result, PLAYER_ID);
    appendI64(result, 4242);
    result.push_back(0);
    std::vector<uint8_t> released = {FLEET_RELEASED};
    appendI64(released, PLAYER_ID + 9);
    released.push_back(1);
    std::vector<uint8_t> bundle = {wire::MESSAGE_BUNDLE_SIGNED};
    for (const auto* m : {&result, &released, &frame}) {
        bundle.push_back(static_cast<uint8_t>(m->size() & 0xFF));
        bundle.push_back(static_cast<uint8_t>(m->size() >> 8));
        bundle.insert(bundle.end(), m->begin(), m->end());
    }
    uint8_t mac[32];
    hmac::spatialSign(bundle.data(), bundle.size(), FLEET, mac);
    bundle.insert(bundle.end(), mac, mac + 32);
    check(wire::verifySignedBundle(bundle.data(), bundle.size(), FLEET),
          "signed bundle: verifies with the fleet token");
    check(!wire::verifySignedBundle(bundle.data(), bundle.size(), PLAYER),
          "signed bundle: does not verify with a player's token");
    int results = 0, releases = 0, deliveries = 0;
    bool walkedAll = wire::forEachMessage(bundle.data(), bundle.size(), [&](const wire::InboundView& m) {
        const FleetFrame member = readFleetFrame(m, true, FLEET);
        if (parseCarryResult(member.frame)) ++results;
        if (parseReleased(member.frame)) ++releases;
        if (auto dd = parseDelivery(member.frame); dd && dd->count == 2) ++deliveries;
    });
    check(walkedAll && results == 1 && releases == 1 && deliveries == 1,
          "signed bundle: the three fleet frames are read from its members, with no HMAC each");

    // Form one: members of a MESSAGE_BUNDLE, whose length prefix counts the frame and its HMAC.
    std::vector<uint8_t> plain = {wire::MESSAGE_BUNDLE};
    for (const auto& m : {withOwnHmac(result), withOwnHmac(released), withOwnHmac(frame)}) {
        plain.push_back(static_cast<uint8_t>(m.size() & 0xFF));
        plain.push_back(static_cast<uint8_t>(m.size() >> 8));
        plain.insert(plain.end(), m.begin(), m.end());
    }
    results = releases = deliveries = 0;
    int verified = 0;
    std::vector<size_t> lengths;
    walkedAll = wire::forEachMessage(plain.data(), plain.size(), [&](const wire::InboundView& m) {
        lengths.push_back(m.len);
        const FleetFrame member = readFleetFrame(m, false, FLEET);
        if (member.auth == FrameAuth::OwnHmac) ++verified;
        if (parseCarryResult(member.frame)) ++results;
        if (parseReleased(member.frame)) ++releases;
        if (auto dd = parseDelivery(member.frame); dd && dd->count == 2) ++deliveries;
    });
    check(walkedAll && lengths == std::vector<size_t>({50, 42, 132}),
          "plain bundle: each member's length counts the frame and its HMAC");
    check(verified == 3 && results == 1 && releases == 1 && deliveries == 1,
          "plain bundle: each frame's own HMAC verifies and the three read back");

    // What must come in a 53, and what comes plain.
    check(isCommunication(135) && isCommunication(137) && isCommunication(144) && isCommunication(142) &&
              isCommunication(18) && isCommunication(36),
          "communications: voice, text, video, single-actor messages, channel text and audio");
    check(isState(130) && isState(133) && isState(138) && isState(139) && isState(140) && isState(145) &&
              isState(146) && !isState(137) && !isCommunication(130) && !isCommunication(140),
          "state: actor updates and departures, voxel edits, events, generic messages, chunk changes");
}

void testControlCommands() {
    const auto* token = reinterpret_cast<const uint8_t*>(fleet_vectors::CONTROL_TOKEN);
    auto reconnect = fromHex(fleet_vectors::RECONNECT_HEX);
    check(reconnect.size() == 33 && verifyReconnect(reconnect.data(), reconnect.size(), token),
          "COMMAND_RECONNECT: the public test vector verifies");
    check(!verifyReconnect(reconnect.data(), reconnect.size(), FLEET),
          "COMMAND_RECONNECT: another session's token does not verify it");
    reconnect[5] ^= 1;
    check(!verifyReconnect(reconnect.data(), reconnect.size(), token),
          "COMMAND_RECONNECT: a flipped byte fails");

    auto released = fromHex(fleet_vectors::SESSION_RELEASED_HEX);
    auto reason = verifySessionReleased(released.data(), released.size(), token);
    check(released.size() == 34 && reason && *reason == 3,
          "COMMAND_SESSION_RELEASED: verifies and gives the reason");
    released[1] = 5; // the reason is inside the signed bytes
    check(!verifySessionReleased(released.data(), released.size(), token),
          "COMMAND_SESSION_RELEASED: a rewritten reason fails");
    check(!verifySessionReleased(released.data(), 20, token),
          "COMMAND_SESSION_RELEASED: a short frame fails");

    // The bound forms, with the public reference's vectors and nonce.
    const uint8_t nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t otherNonce[8] = {1, 2, 3, 4, 5, 6, 7, 9};
    auto bound = fromHex(fleet_vectors::RECONNECT_BOUND_HEX);
    check(bound.size() == 41 && verifyReconnectBound(bound.data(), bound.size(), token, nonce),
          "COMMAND_RECONNECT_BOUND (44): the public test vector verifies with its nonce");
    check(!verifyReconnectBound(bound.data(), bound.size(), token, otherNonce),
          "COMMAND_RECONNECT_BOUND: another assignment's nonce is refused");
    check(!verifyReconnectBound(bound.data(), bound.size(), FLEET, nonce),
          "COMMAND_RECONNECT_BOUND: another token is refused");
    check(!verifyReconnect(bound.data(), bound.size(), token), "COMMAND_RECONNECT_BOUND: is not a 22");
    auto boundReleased = fromHex(fleet_vectors::SESSION_RELEASED_BOUND_HEX);
    reason = verifySessionReleasedBound(boundReleased.data(), boundReleased.size(), token, nonce);
    check(boundReleased.size() == 42 && reason && *reason == 5,
          "COMMAND_SESSION_RELEASED_BOUND (45): the public test vector verifies and gives reason 5");
    check(!verifySessionReleasedBound(boundReleased.data(), boundReleased.size(), token, otherNonce),
          "COMMAND_SESSION_RELEASED_BOUND: another nonce is refused");
    boundReleased[1] = 3;
    check(!verifySessionReleasedBound(boundReleased.data(), boundReleased.size(), token, nonce),
          "COMMAND_SESSION_RELEASED_BOUND: a rewritten reason fails");
    auto ack = fromHex(fleet_vectors::BOUND_ACK_HEX);
    check(ack.size() == 41 && verifyBoundAck(ack.data(), ack.size(), token, nonce),
          "CONTROL_BOUND_ACK (46): the public test vector verifies");
    check(!verifyBoundAck(bound.data(), bound.size(), token, nonce), "CONTROL_BOUND_ACK: a 44 is not a 46");
}

void testMarker() {
    uint8_t buf[wire::ACTOR_UPDATE_MAX_SIZE];
    wire::initActorUpdateTemplateFmt(buf, wire::PoseFormat::UE5,
                                     "0123456789ABCDEF0123456789ABCDEF", APP, PLAYER_ID, 8,
                                     wire::DecayRate::EXPONENTIAL);
    wire::ActorPose pose;
    pose.posX = 12.5;
    pose.pitch = markerCode(99, 0xA5C3, true);
    pose.yaw = 1234567890123.0;
    wire::finalizeActorUpdateFmt(buf, wire::PoseFormat::UE5, 4, 5, 0, pose, 1, FLEET);

    // What a receiving session sees: the same payload inside a notification
    // without an HMAC of its own (a member of a signed bundle).
    std::vector<uint8_t> notification(buf, buf + wire::ACTOR_UPDATE_HMAC_OFFSET);
    notification[0] = wire::ACTOR_UPDATE_NOTIFICATION_2;
    notification[wire::off::CONTAINS_AUTH] = 0;
    appendI64(notification, 1795000000999LL);
    notification.push_back(1);
    const wire::InboundView view{notification.data(), notification.size()};
    const auto payload = notificationPayload(view);
    check(payload && payload->len == wire::state::SIZE, "marker: the notification's payload is the 88-byte state");
    auto m = payload ? readMarker(payload->data, payload->len) : std::nullopt;
    check(m && m->session == 99 && m->runTag == 0xA5C3 && m->sampled && m->sendUs == 1234567890123LL,
          "marker: session, run tag, sample flag and send time read back");

    // And from the signed form, whose tail is 41 bytes.
    const wire::InboundView signedView{buf, wire::ACTOR_UPDATE_SIZE};
    const auto signedPayload = notificationPayload(signedView);
    m = signedPayload ? readMarker(signedPayload->data, signedPayload->len) : std::nullopt;
    check(m && m->session == 99, "marker: read from a notification that carries its own HMAC");

    pose.pitch = markerCode(65535, 0, false);
    pose.yaw = 0.0;
    wire::finalizeActorUpdateFmt(buf, wire::PoseFormat::UE5, 4, 5, 0, pose, 1, FLEET);
    m = readMarker(buf + wire::HEADER_SIZE, wire::state::SIZE);
    check(m && m->session == 65535 && m->runTag == 0 && !m->sampled && m->sendUs == 0,
          "marker: the largest session index, no sample flag");

    // A state this tool did not write has no marker.
    pose.pitch = 0.0;
    pose.yaw = 0.0;
    wire::finalizeActorUpdateFmt(buf, wire::PoseFormat::UE5, 4, 5, 0, pose, 1, FLEET);
    check(!readMarker(buf + wire::HEADER_SIZE, wire::state::SIZE), "marker: a plain state has none");
    pose.pitch = 12.5;
    pose.yaw = 90.0;
    wire::finalizeActorUpdateFmt(buf, wire::PoseFormat::UE5, 4, 5, 0, pose, 1, FLEET);
    check(!readMarker(buf + wire::HEADER_SIZE, wire::state::SIZE),
          "marker: a real pitch and yaw are not taken for one");
    check(!readMarker(buf + wire::HEADER_SIZE, 48), "marker: another payload size has none");
}

} // namespace

int main() {
    testCarryProof();
    testCarryBytes();
    testOtherControlBytes();
    testGoldenVectors();
    testCarriedPlayersUpdate();
    testCarryResultParsing();
    testReleasedParsing();
    testDeliveryParsing();
    testControlCommands();
    testMarker();

    if (g_failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all fleet wire tests passed\n");
    return 0;
}
