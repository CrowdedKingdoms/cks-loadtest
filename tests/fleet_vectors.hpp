#pragma once

// Known answers for the fleet wire formats, computed OUTSIDE this repository's
// C++ with Python 3 (hmac, hashlib, struct) from the text of the contract:
//
//   proof   = HMAC-SHA256(key = player token,
//                         "ck-fleet-carry" + le64(fleet id) + le64(player id) + le64(epoch ms))
//   control = [type][le64 appId][24 zero bytes: chunk][0][0][1][32 zero bytes: no uuid][payload]
//             [HMAC-SHA256(key = fleet token, all bytes before + fleet token)][le64 fleet id][seq]
//   update  = the long-form ACTOR_UPDATE_REQUEST of wire_test.cpp (uuid, app 7, chunk (1,-2,3),
//             distance 8, decay 1, the same UE5 state), with the tail's id the PLAYER's and the
//             HMAC keyed with the FLEET token
//   frame   = a server frame (51, 52, 53) followed by
//             HMAC-SHA256(key = fleet token, the frame + fleet token)
//
// Inputs: app 7, fleet gameTokenId 7000000001, player gameTokenId 9000000002,
// epoch millis 1795000000123, sequence 9, and the two 64-octet tokens below.
// The reconnect and bound-control vectors are the ones the public wire-format
// reference prints. The GOLDEN_* vectors are the ones the contract fixes for
// every client, as the C++ SDK's tests hold them.

namespace fleet_vectors {

inline constexpr const char* FLEET_TOKEN =
    "fleet0123456789fleet0123456789fleet0123456789fleet0123456789abcd";
inline constexpr const char* PLAYER_TOKEN =
    "player012345678player012345678player012345678player012345678wxyz";
inline constexpr long long APP_ID = 7;
inline constexpr long long FLEET_ID = 7000000001LL;
inline constexpr long long PLAYER_ID = 9000000002LL;
inline constexpr long long EPOCH_MS = 1795000000123LL;
inline constexpr unsigned char SEQ = 9;

inline constexpr const char* PROOF_HEX =
    "98ddf5f217ddacdf9bb4145b0572eddbecd4c951f4867d3fbb404db2a0caa715";

// FLEET_CARRY (49), 157 bytes.
inline constexpr const char* CARRY_HEX =
    "3107000000000000000000000000000000000000000000000000000000000000000000010000000000000000"
    "000000000000000000000000000000000000000000000000021a7118020000007b5e56eea101000098ddf5f2"
    "17ddacdf9bb4145b0572eddbecd4c951f4867d3fbb404db2a0caa7154f1f8158ce46cbb97998dc051c526b06"
    "7844f46c8a6e5952206d1c74ed8abdd801863ba10100000009";

// FLEET_RELEASE (50), 117 bytes.
inline constexpr const char* RELEASE_HEX =
    "3207000000000000000000000000000000000000000000000000000000000000000000010000000000000000"
    "000000000000000000000000000000000000000000000000021a7118020000002fd1790f3af784f324277c9a"
    "9e64d906924c383c0de3316131fca6b4a743b2ed01863ba10100000009";

// The session's own keepalive (26, no actor), 109 bytes.
inline constexpr const char* HEARTBEAT_HEX =
    "1a07000000000000000000000000000000000000000000000000000000000000000000010000000000000000"
    "000000000000000000000000000000000000000000000000e97f85782bec125273972cbc8ce369461dbbc50d"
    "8d331b2640974ea4397b023301863ba10100000009";

// A heartbeat for a carried player's actor (26 under the PLAYER's id, the actor's
// chunk (1,-2,3) and uuid), 109 bytes.
inline constexpr const char* PLAYER_HEARTBEAT_HEX =
    "1a07000000000000000100000000000000feffffffffffffff03000000000000000000013031323334353637"
    "383941424344454630313233343536373839414243444546ee3974ea86e0f7cb45b346c8111b39d72cd98b9b"
    "2f83a5381e8ca5720ef20b43021a71180200000009";

// CLIENT_CAPABILITIES (29) with BUNDLE_SIGNED | FLEET_OWN_STATE (flags 9), 113 bytes.
inline constexpr const char* CAPABILITIES_HEX =
    "1d07000000000000000000000000000000000000000000000000000000000000000000010000000000000000"
    "00000000000000000000000000000000000000000000000009000000a9ee0ccf4d7c09515ca6e263ba47931d"
    "c6897c658fafe8cb054aa1678738037601863ba10100000009";

// CLIENT_CAPABILITIES (29) with BUNDLE_SIGNED | BOUND_CONTROL | FLEET_OWN_STATE (flags 11)
// and the control nonce 01 02 03 04 05 06 07 08 after the flags, 121 bytes.
inline constexpr const char* CAPABILITIES_BOUND_HEX =
    "1d07000000000000000000000000000000000000000000000000000000000000000000010000000000000000"
    "0000000000000000000000000000000000000000000000000b000000010203040506070817b3a0e55bb80eb4"
    "ca9aac6bcf8892e0107d11412ce1faed2791546d63190f7b01863ba10100000009";

// A carried player's ACTOR_UPDATE_REQUEST, 197 bytes.
inline constexpr const char* UPDATE_HEX =
    "8007000000000000000100000000000000feffffffffffffff03000000000000000801013031323334353637"
    "3839414243444546303132333435363738394142434445460200000000000000000000000020594000000000"
    "000869c00000000000004ec00000000000000000000000000000000000000000008046400000000000c06240"
    "000000000000000000000000000000000000000000000000164b43357848a1cef78a2d2f8d4a59d45595450f"
    "1c4e701c4edaec4c22568c1d021a71180200000009";

// FLEET_CARRY_RESULT (51: the player, user 4242, status 0) with its own HMAC, 50 bytes.
inline constexpr const char* CARRY_RESULT_SIGNED_HEX =
    "33021a711802000000921000000000000000169952aab2e91f5eb17d0964728f858b088460a8637962830ed2"
    "f47cbb95a988";

// FLEET_RELEASED (52: the player, reason 1) with its own HMAC, 42 bytes.
inline constexpr const char* RELEASED_SIGNED_HEX =
    "34021a71180200000001a4388108bd0a6f006b6ca7c0d9bd71ea2a6f35880930d497e656963ac307804a";

// FLEET_DELIVERY (53) listing the player and the next id, wrapping an unsigned text
// notification (137, chunk (1,-2,3), payload "hello", its 9-byte tail), with its own
// HMAC: 2 + 16 + 82 + 32 = 132 bytes.
inline constexpr const char* DELIVERY_SIGNED_HEX =
    "3502021a711802000000031a7118020000008907000000000000000100000000000000feffffffffffffff03"
    "0000000000000008010030313233343536373839414243444546303132333435363738394142434445466865"
    "6c6c6fe76156eea101000005dfafce8991003dea724cb80c641e1405f70d16a8f8ffec0912b4e8b50f7ea2d3";

// COMMAND_RECONNECT (22) and COMMAND_SESSION_RELEASED (28, reason 3) for this token.
inline constexpr const char* CONTROL_TOKEN =
    "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+/";
inline constexpr const char* RECONNECT_HEX =
    "16c80d2cee8977578129f7295001cd1bea65dd11be3f518560529cb298ce5fc42b";
inline constexpr const char* SESSION_RELEASED_HEX =
    "1c03754b04b29c2d18087bd77fc3b7b5e204ff806bcb2dbcf36c38d4e1bf5ed774a5";
// The bound forms for the same token and the nonce 01 02 03 04 05 06 07 08, as the
// public wire-format reference prints them: 44, 45 with reason 5, and the server's 46.
inline constexpr const char* RECONNECT_BOUND_HEX =
    "2c010203040506070801da5c2e0f16ec592bd41e81286527373b1ecabb591bcc7f6481dfdb6ed2a5c2";
inline constexpr const char* SESSION_RELEASED_BOUND_HEX =
    "2d050102030405060708fae450e083640dfdf23fd6cd963aef35d6f50be32304624468258dc7d8c18edf";
inline constexpr const char* BOUND_ACK_HEX =
    "2e0102030405060708a2587a4b9f7b7a617387df74f45b78ace492b99e376dedff75f88956dbf62f85";

// The contract's golden vectors: fleet token below with gameTokenId 9001, player token
// below with gameTokenId 4242, app 7, epoch millis 1795000000000.
inline constexpr const char* GOLDEN_FLEET_TOKEN =
    "fleet-token-0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP";
inline constexpr const char* GOLDEN_PLAYER_TOKEN =
    "player-token-0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNO";
inline constexpr long long GOLDEN_FLEET_ID = 9001;
inline constexpr long long GOLDEN_PLAYER_ID = 4242;
inline constexpr long long GOLDEN_EPOCH_MS = 1795000000000LL;
inline constexpr const char* GOLDEN_PROOF_HEX =
    "5602abb9300581f6a20e247ea0b406f690646a3d27f9427e95c5f6363115b35a";
// FLEET_CARRY (49), sequence 0.
inline constexpr const char* GOLDEN_CARRY_HEX =
    "3107000000000000000000000000000000000000000000000000000000000000000000010000000000000000"
    "0000000000000000000000000000000000000000000000009210000000000000005e56eea10100005602abb9"
    "300581f6a20e247ea0b406f690646a3d27f9427e95c5f6363115b35a9d69b5b07c19a2450e61725d9995c19c"
    "940e41fa602fcce2535e780e5d2d81e3292300000000000000";
// FLEET_RELEASE (50), sequence 2.
inline constexpr const char* GOLDEN_RELEASE_HEX =
    "3207000000000000000000000000000000000000000000000000000000000000000000010000000000000000"
    "0000000000000000000000000000000000000000000000009210000000000000903cf330c14c7344619b97d3"
    "736c97d042c2669c9c8c1ed8e4b1922c327f684d292300000000000002";
// ACTOR_UPDATE_REQUEST (128) for the carried player, sequence 1: chunk (1,-2,3), distance 8,
// decay 0, uuid "0123456789abcdef0123456789abcdef", payload de ad be ef.
inline constexpr const char* GOLDEN_UPDATE_HEX =
    "8007000000000000000100000000000000feffffffffffffff03000000000000000800013031323334353637"
    "383961626364656630313233343536373839616263646566deadbeefafca3c1e53f866c6384fb186e2396664"
    "b62d7acd0fee6d9445b96e283d1c09f4921000000000000001";

} // namespace fleet_vectors
