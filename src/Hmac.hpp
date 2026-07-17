#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

/// HMAC-SHA256 helpers for the Buddy long-spatial wire protocol.
///
/// The key is the 64-character app-scoped token used AS-IS (64 Latin-1 octets,
/// never hex-decoded). The signed message is `prefix || token` where `prefix`
/// is every byte of the datagram before the 32-byte HMAC field.
/// See cks-docs "HMAC for spatial messages".
namespace lt::hmac {

constexpr size_t TAG_SIZE = 32;
constexpr size_t TOKEN_OCTETS = 64;

/// Compute the spatial-message HMAC tag:
///   tag = HMAC-SHA256(key = token, message = prefix || token)
/// `token` must point to exactly 64 octets. `out` must hold 32 bytes.
inline bool spatialSign(const uint8_t* prefix, size_t prefixLen,
                        const uint8_t* token, uint8_t* out) {
    // The one-shot HMAC() takes a single buffer, so concatenate on the stack.
    // Outbound prefixes are 156 bytes; inbound datagrams are read into 2048-
    // byte buffers, so this bound covers every message we sign or verify.
    constexpr size_t MAX_MSG = 2048 + TOKEN_OCTETS;
    if (prefixLen + TOKEN_OCTETS > MAX_MSG) return false;
    uint8_t msg[MAX_MSG];
    std::memcpy(msg, prefix, prefixLen);
    std::memcpy(msg + prefixLen, token, TOKEN_OCTETS);
    unsigned int outLen = 0;
    return HMAC(EVP_sha256(), token, static_cast<int>(TOKEN_OCTETS), msg,
                prefixLen + TOKEN_OCTETS, out, &outLen) != nullptr &&
           outLen == TAG_SIZE;
}

/// Constant-time comparison of two 32-byte tags.
inline bool tagEquals(const uint8_t* a, const uint8_t* b) {
    return CRYPTO_memcmp(a, b, TAG_SIZE) == 0;
}

} // namespace lt::hmac
