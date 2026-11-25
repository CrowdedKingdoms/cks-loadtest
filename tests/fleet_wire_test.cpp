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

std::vector<uint8_t> longForm(uint8_t type, const std::vector<uint8_t>& payload, bool withHmac) {
    std::vector<uint8_t> m(wire::HEADER_SIZE, 0);
    m[0] = type;
    wire::writeI64(m.data() + wire::off::APP_ID, APP);
    m[wire::off::CONTAINS_AUTH] = withHmac ? 1 : 0;
    m.insert(m.end(), payload.begin(), payload.end());
    if (withHmac) {
        uint8_t mac[32];
        hmac::spatialSign(m.data(), m.size(), FLEET, mac);
        m.insert(m.end(), mac, mac + 32);
    }
    appendI64(m, 1795000000999LL); // epoch millis
    m.push_back(5);                // sequence
    return m;
}

void testCarryResultParsing() {
    std::vector<uint8_t> payload;
    appendI64(payload, PLAYER_ID);
    appendI64(payload, 4242);
    payload.push_back(0);

    std::vector<uint8_t> compact = {FLEET_CARRY_RESULT};
    compact.insert(compact.end(), payload.begin(), payload.end());
    auto r = parseCarryResult(wire::InboundView{compact.data(), compact.size()}, APP);
    check(compact.size() == 18 && r && r->playerGameTokenId == PLAYER_ID && r->userId == 4242 &&
              r->status == CARRY_CARRIED,
          "FLEET_CARRY_RESULT: [8B player][8B user][1B status] after the type");

    for (uint8_t status : {uint8_t{1}, uint8_t{2}, uint8_t{3}, uint8_t{4}}) {
        compact[9] = compact[10] = 0; // the user id is 0 unless carried
        compact[17] = status;
        r = parseCarryResult(wire::InboundView{compact.data(), compact.size()}, APP);
        check(r && r->status == status, "FLEET_CARRY_RESULT: a refusal status reads back");
    }

    auto truncated = compact;
    truncated.pop_back();
    check(!parseCarryResult(wire::InboundView{truncated.data(), truncated.size()}, APP),
          "FLEET_CARRY_RESULT: a short frame is refused");
    auto wrongType = compact;
    wrongType[0] = FLEET_RELEASED;
    check(!parseCarryResult(wire::InboundView{wrongType.data(), wrongType.size()}, APP),
          "FLEET_CARRY_RESULT: another type is refused");

    // A tail after the payload does not change what is read.
    auto withTail = compact;
    appendI64(withTail, 1795000000999LL);
    withTail.push_back(7);
    r = parseCarryResult(wire::InboundView{withTail.data(), withTail.size()}, APP);
    check(r && r->playerGameTokenId == PLAYER_ID && r->status == 4,
          "FLEET_CARRY_RESULT: trailing bytes are ignored");

    // The same payload framed as a long-form app payload, unsigned and signed.
    payload[16] = 3;
    for (bool withHmac : {false, true}) {
        const auto m = longForm(FLEET_CARRY_RESULT, payload, withHmac);
        r = parseCarryResult(wire::InboundView{m.data(), m.size()}, APP);
        check(looksLongForm(wire::InboundView{m.data(), m.size()}, APP) && r &&
                  r->playerGameTokenId == PLAYER_ID && r->userId == 4242 && r->status == 3,
              withHmac ? "FLEET_CARRY_RESULT: read from a signed long-form frame"
                       : "FLEET_CARRY_RESULT: read from a long-form frame");
    }
    check(!looksLongForm(wire::InboundView{compact.data(), compact.size()}, APP),
          "FLEET_CARRY_RESULT: the compact frame is not taken for a long-form one");
}

void testReleasedParsing() {
    std::vector<uint8_t> payload;
    appendI64(payload, PLAYER_ID);
    payload.push_back(RELEASED_CARRIED_ELSEWHERE);
    std::vector<uint8_t> compact = {FLEET_RELEASED};
    compact.insert(compact.end(), payload.begin(), payload.end());
    auto r = parseReleased(wire::InboundView{compact.data(), compact.size()}, APP);
    check(compact.size() == 10 && r && r->playerGameTokenId == PLAYER_ID && r->reason == 1,
          "FLEET_RELEASED: [8B player][1B reason], reason 1 carried by another session");
    for (uint8_t reason : {uint8_t{2}, uint8_t{5}}) {
        compact[9] = reason;
        r = parseReleased(wire::InboundView{compact.data(), compact.size()}, APP);
        check(r && r->reason == reason, "FLEET_RELEASED: reasons 2 and 5 read back");
    }
    auto truncated = compact;
    truncated.pop_back();
    check(!parseReleased(wire::InboundView{truncated.data(), truncated.size()}, APP),
          "FLEET_RELEASED: a short frame is refused");
    const auto m = longForm(FLEET_RELEASED, payload, false);
    r = parseReleased(wire::InboundView{m.data(), m.size()}, APP);
    check(r && r->playerGameTokenId == PLAYER_ID && r->reason == 1,
          "FLEET_RELEASED: read from a long-form frame");
}

void testDeliveryParsing() {
    // A text notification as a player's own session would be sent it.
    const std::vector<uint8_t> text = {'h', 'e', 'l', 'l', 'o'};
    auto notification = longForm(137 /*CLIENT_TEXT_NOTIFICATION*/, text, false);
    wire::writeI64(notification.data() + wire::off::CHUNK_X, 12); // a real chunk, not (0,0,0)

    std::vector<uint8_t> frame = {FLEET_DELIVERY, 3};
    for (int64_t id : {PLAYER_ID, PLAYER_ID + 1, PLAYER_ID + 2}) appendI64(frame, id);
    frame.insert(frame.end(), notification.begin(), notification.end());

    auto d = parseDelivery(wire::InboundView{frame.data(), frame.size()}, APP);
    check(d && d->count == 3, "FLEET_DELIVERY: the count");
    check(d && d->player(0) == PLAYER_ID && d->player(1) == PLAYER_ID + 1 &&
              d->player(2) == PLAYER_ID + 2,
          "FLEET_DELIVERY: count x 8-byte player gameTokenIds");
    check(d && d->notification.len == notification.size() &&
              std::memcmp(d->notification.data, notification.data(), notification.size()) == 0 &&
              d->notification.type() == 137,
          "FLEET_DELIVERY: then the notification, byte for byte");
    const auto inner = d ? notificationPayload(d->notification) : std::nullopt;
    check(inner && inner->len == 5 && std::memcmp(inner->data, "hello", 5) == 0,
          "FLEET_DELIVERY: the notification's own payload is reachable");

    auto none = frame;
    none[1] = 0;
    check(!parseDelivery(wire::InboundView{none.data(), none.size()}, APP),
          "FLEET_DELIVERY: a count of 0 is refused");
    auto tooMany = frame;
    tooMany[1] = 33;
    check(!parseDelivery(wire::InboundView{tooMany.data(), tooMany.size()}, APP),
          "FLEET_DELIVERY: more than 32 players is refused");
    std::vector<uint8_t> shortFrame(frame.begin(), frame.begin() + 2 + 24); // ids, no notification
    check(!parseDelivery(wire::InboundView{shortFrame.data(), shortFrame.size()}, APP),
          "FLEET_DELIVERY: a frame with no notification is refused");

    // 32 players, the most a frame holds.
    std::vector<uint8_t> full = {FLEET_DELIVERY, 32};
    for (int i = 0; i < 32; ++i) appendI64(full, PLAYER_ID + i);
    full.insert(full.end(), notification.begin(), notification.end());
    d = parseDelivery(wire::InboundView{full.data(), full.size()}, APP);
    check(d && d->count == 32 && d->player(31) == PLAYER_ID + 31 &&
              d->notification.len == notification.size(),
          "FLEET_DELIVERY: 32 players a frame");

    // Inside a signed bundle, as a session that advertised it receives its frames.
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
    const bool walked = wire::forEachMessage(bundle.data(), bundle.size(), [&](const wire::InboundView& m) {
        if (parseCarryResult(m, APP)) ++results;
        if (parseReleased(m, APP)) ++releases;
        if (auto dd = parseDelivery(m, APP); dd && dd->count == 3) ++deliveries;
    });
    check(walked && results == 1 && releases == 1 && deliveries == 1,
          "signed bundle: the three fleet frames are read from its members");
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
