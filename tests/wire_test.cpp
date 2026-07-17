// Unit tests for the wire builder/parser and the spatial HMAC.
//
// The reference vector below was generated with the platform's reference
// wire implementation, for a signed ACTOR_UPDATE_REQUEST_2 built from:
//   gameTokenId=123456789, token="ab"*32,
//   uuid="0123456789ABCDEF0123456789ABCDEF",
//   appId=7, chunk=(1,-2,3), distance=8, decay=1, containsAuth=1, seq=9,
//   actor state v2: pos=(100.5,-200.25,-60.0), rot=(0,0,45.0), vel=(150,0,0).

#include "Wire.hpp"

#include <cstdio>
#include <cstring>
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

// Python-generated reference (197 bytes).
const std::string EXPECTED_HEX =
    "8007000000000000000100000000000000feffffffffffffff0300000000000000080101"
    "3031323334353637383941424344454630313233343536373839414243444546020000000"
    "0000000000000000020594000000000000869c00000000000004ec0000000000000000000"
    "0000000000000000000000008046400000000000c0624000000000000000000000000000"
    "0000000000000000000000a022dd9a277822a22566a8ab7e1ad14c412b3e06c644b7a46d1"
    "70ed65de78f4915cd5b070000000009";

const char* TOKEN =
    "abababababababababababababababababababababababababababababababab";
// (64 chars: "ab" * 32)

std::vector<uint8_t> buildReferenceMessage() {
    std::vector<uint8_t> buf(lt::wire::ACTOR_UPDATE_SIZE);
    lt::wire::initActorUpdateTemplate(
        buf.data(), "0123456789ABCDEF0123456789ABCDEF", /*appId*/ 7,
        /*gameTokenId*/ 123456789, /*distance*/ 8,
        lt::wire::DecayRate::EXPONENTIAL);
    bool ok = lt::wire::finalizeActorUpdate(
        buf.data(), /*chunk*/ 1, -2, 3, /*pos*/ 100.5, -200.25, -60.0,
        /*rot*/ 0.0, 0.0, 45.0, /*vel*/ 150.0, 0.0, 0.0, /*seq*/ 9,
        reinterpret_cast<const uint8_t*>(TOKEN));
    check(ok, "finalizeActorUpdate succeeds");
    return buf;
}

void testActorUpdateVector() {
    auto buf = buildReferenceMessage();
    check(buf.size() == 197, "signed actor update is 197 bytes");
    std::string got = toHex(buf.data(), buf.size());
    if (got != EXPECTED_HEX) {
        std::fprintf(stderr, "expected: %s\n     got: %s\n", EXPECTED_HEX.c_str(),
                     got.c_str());
    }
    check(got == EXPECTED_HEX, "matches the reference wire vector");

    // Structural spot checks.
    check(buf[0] == lt::wire::ACTOR_UPDATE_REQUEST_2, "type byte is 128");
    check(buf[lt::wire::off::CONTAINS_AUTH] == 1, "containsAuth is set");
    check(lt::wire::readI64(buf.data() + lt::wire::ACTOR_UPDATE_TOKEN_ID_OFFSET) ==
              123456789,
          "gameTokenId in tail");
    check(buf[lt::wire::ACTOR_UPDATE_SEQ_OFFSET] == 9, "sequence in tail");
}

void testVerifyNotification() {
    auto buf = buildReferenceMessage();
    lt::wire::InboundView view{buf.data(), buf.size()};
    check(lt::wire::verifyNotification(view,
                                       reinterpret_cast<const uint8_t*>(TOKEN)),
          "verifyNotification accepts a correctly signed message");

    auto tampered = buf;
    tampered[lt::wire::off::PAYLOAD + 8] ^= 0xFF; // flip a payload byte
    lt::wire::InboundView bad{tampered.data(), tampered.size()};
    check(!lt::wire::verifyNotification(bad,
                                        reinterpret_cast<const uint8_t*>(TOKEN)),
          "verifyNotification rejects a tampered message");

    // Unsigned (containsAuth=0) messages verify trivially.
    auto unsigned_ = buf;
    unsigned_[lt::wire::off::CONTAINS_AUTH] = 0;
    lt::wire::InboundView plain{unsigned_.data(), unsigned_.size()};
    check(lt::wire::verifyNotification(plain,
                                       reinterpret_cast<const uint8_t*>(TOKEN)),
          "verifyNotification passes unsigned messages through");
}

void testBundleParsing() {
    auto msg = buildReferenceMessage();
    std::vector<uint8_t> small = {lt::wire::GENERIC_ERROR_MESSAGE, 9,
                                  lt::wire::ERR_TOKEN_EXPIRED};

    std::vector<uint8_t> bundle;
    bundle.push_back(lt::wire::MESSAGE_BUNDLE);
    auto append = [&bundle](const std::vector<uint8_t>& m) {
        uint16_t len = static_cast<uint16_t>(m.size());
        bundle.push_back(static_cast<uint8_t>(len & 0xFF));
        bundle.push_back(static_cast<uint8_t>(len >> 8));
        bundle.insert(bundle.end(), m.begin(), m.end());
    };
    append(msg);
    append(small);

    std::vector<size_t> lens;
    bool ok = lt::wire::forEachMessage(
        bundle.data(), bundle.size(),
        [&](const lt::wire::InboundView& m) { lens.push_back(m.len); });
    check(ok, "well-formed bundle parses");
    check(lens.size() == 2 && lens[0] == 197 && lens[1] == 3,
          "bundle yields both members with correct lengths");

    // Non-bundle datagrams yield themselves once.
    lens.clear();
    ok = lt::wire::forEachMessage(
        msg.data(), msg.size(),
        [&](const lt::wire::InboundView& m) { lens.push_back(m.len); });
    check(ok && lens.size() == 1 && lens[0] == 197,
          "plain datagram yields itself");

    // Truncated bundle reports failure.
    auto truncated = bundle;
    truncated.resize(bundle.size() - 2);
    ok = lt::wire::forEachMessage(truncated.data(), truncated.size(),
                                  [](const lt::wire::InboundView&) {});
    check(!ok, "truncated bundle reports malformed");
}

void testEpochExtraction() {
    // Build a fake unsigned notification: header + payload + epoch + seq.
    std::vector<uint8_t> notif(lt::wire::HEADER_SIZE + 4 + lt::wire::TAIL_NO_HMAC,
                               0);
    notif[0] = lt::wire::ACTOR_UPDATE_NOTIFICATION_2;
    const int64_t epoch = 1752787200123;
    lt::wire::writeI64(notif.data() + notif.size() - lt::wire::TAIL_NO_HMAC,
                       epoch);
    notif[notif.size() - 1] = 42;
    lt::wire::InboundView view{notif.data(), notif.size()};
    auto got = lt::wire::notificationEpochMs(view);
    check(got.has_value() && *got == epoch, "epoch millis extracted from tail");

    std::vector<uint8_t> err = {lt::wire::GENERIC_ERROR_MESSAGE, 1, 2};
    lt::wire::InboundView errView{err.data(), err.size()};
    check(!lt::wire::notificationEpochMs(errView).has_value(),
          "no epoch for non-spatial messages");
}

// Reference hex for the fromHex helper sanity (self-test).
void testHexRoundTrip() {
    auto bytes = fromHex(EXPECTED_HEX);
    check(bytes.size() == 197, "reference hex decodes to 197 bytes");
    check(toHex(bytes.data(), bytes.size()) == EXPECTED_HEX, "hex round trip");
}

} // namespace

int main() {
    testHexRoundTrip();
    testActorUpdateVector();
    testVerifyNotification();
    testBundleParsing();
    testEpochExtraction();

    if (g_failures) {
        std::fprintf(stderr, "%d test(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all wire tests passed\n");
    return 0;
}
