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
//
// Inputs: app 7, fleet gameTokenId 7000000001, player gameTokenId 9000000002,
// epoch millis 1795000000123, sequence 9, and the two 64-octet tokens below.
// The reconnect vector is the one the public wire-format reference prints.

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

// The session's own heartbeat (26), 109 bytes.
inline constexpr const char* HEARTBEAT_HEX =
    "1a07000000000000000000000000000000000000000000000000000000000000000000010000000000000000"
    "000000000000000000000000000000000000000000000000e97f85782bec125273972cbc8ce369461dbbc50d"
    "8d331b2640974ea4397b023301863ba10100000009";

// CLIENT_CAPABILITIES (29) with BUNDLE_SIGNED | FLEET_OWN_STATE (flags 9), 113 bytes.
inline constexpr const char* CAPABILITIES_HEX =
    "1d07000000000000000000000000000000000000000000000000000000000000000000010000000000000000"
    "00000000000000000000000000000000000000000000000009000000a9ee0ccf4d7c09515ca6e263ba47931d"
    "c6897c658fafe8cb054aa1678738037601863ba10100000009";

// A carried player's ACTOR_UPDATE_REQUEST, 197 bytes.
inline constexpr const char* UPDATE_HEX =
    "8007000000000000000100000000000000feffffffffffffff03000000000000000801013031323334353637"
    "3839414243444546303132333435363738394142434445460200000000000000000000000020594000000000"
    "000869c00000000000004ec00000000000000000000000000000000000000000008046400000000000c06240"
    "000000000000000000000000000000000000000000000000164b43357848a1cef78a2d2f8d4a59d45595450f"
    "1c4e701c4edaec4c22568c1d021a71180200000009";

// COMMAND_RECONNECT (22) and COMMAND_SESSION_RELEASED (28, reason 3) for this token.
inline constexpr const char* CONTROL_TOKEN =
    "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+/";
inline constexpr const char* RECONNECT_HEX =
    "16c80d2cee8977578129f7295001cd1bea65dd11be3f518560529cb298ce5fc42b";
inline constexpr const char* SESSION_RELEASED_HEX =
    "1c03754b04b29c2d18087bd77fc3b7b5e204ff806bcb2dbcf36c38d4e1bf5ed774a5";

} // namespace fleet_vectors
