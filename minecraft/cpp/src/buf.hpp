#pragma once

#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

namespace ckmc {

// Minecraft's unsigned VarInt and the handful of field types the flying client writes.
class Buf {
 public:
  std::vector<uint8_t> b;
  size_t r = 0;
  bool ok = true;

  void wU8(uint8_t v) { b.push_back(v); }
  void wBool(bool v) { wU8(v ? 1 : 0); }

  void wU16(uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
  }

  void wI32(int32_t v) {
    auto u = static_cast<uint32_t>(v);
    b.push_back(static_cast<uint8_t>(u >> 24));
    b.push_back(static_cast<uint8_t>(u >> 16));
    b.push_back(static_cast<uint8_t>(u >> 8));
    b.push_back(static_cast<uint8_t>(u));
  }

  void wI64(int64_t v) {
    auto u = static_cast<uint64_t>(v);
    for (int shift = 56; shift >= 0; shift -= 8) {
      b.push_back(static_cast<uint8_t>(u >> shift));
    }
  }

  void wF32(float v) {
    uint32_t u;
    static_assert(sizeof(float) == 4);
    std::memcpy(&u, &v, 4);
    wI32(static_cast<int32_t>(u));
  }

  void wF64(double v) {
    uint64_t u;
    static_assert(sizeof(double) == 8);
    std::memcpy(&u, &v, 8);
    wI64(static_cast<int64_t>(u));
  }

  void wVar(uint32_t v) {
    while (v >= 0x80) {
      b.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
      v >>= 7;
    }
    b.push_back(static_cast<uint8_t>(v));
  }

  void wUtf(std::string_view s) {
    wVar(static_cast<uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
  }

  void wRaw(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }

  size_t left() const { return ok && r <= b.size() ? b.size() - r : 0; }

  uint8_t rU8() {
    if (r >= b.size()) {
      ok = false;
      return 0;
    }
    return b[r++];
  }

  bool rBool() { return rU8() != 0; }

  int32_t rI32() {
    uint32_t u = 0;
    for (int i = 0; i < 4; i++) u = (u << 8) | rU8();
    return static_cast<int32_t>(u);
  }

  int64_t rI64() {
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u = (u << 8) | rU8();
    return static_cast<int64_t>(u);
  }

  float rF32() {
    auto u = static_cast<uint32_t>(rI32());
    float v;
    std::memcpy(&v, &u, 4);
    return v;
  }

  double rF64() {
    auto u = static_cast<uint64_t>(rI64());
    double v;
    std::memcpy(&v, &u, 8);
    return v;
  }

  uint32_t rVar() {
    uint32_t v = 0;
    for (int shift = 0; shift < 35; shift += 7) {
      uint8_t byte = rU8();
      if (!ok) return 0;
      v |= static_cast<uint32_t>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0) return v;
    }
    ok = false;
    return 0;
  }

  std::string rUtf(uint32_t cap = 32767) {
    uint32_t n = rVar();
    if (!ok || n > cap || n > left()) {
      ok = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(b.data() + r), n);
    r += n;
    return s;
  }

  void rRaw(uint8_t* p, size_t n) {
    if (n > left()) {
      ok = false;
      return;
    }
    std::memcpy(p, b.data() + r, n);
    r += n;
  }
};

inline Buf bufFrom(const uint8_t* p, size_t n) {
  Buf b;
  b.b.assign(p, p + n);
  return b;
}

// Try to peel one VarInt off the front of a TCP buffer. Incomplete returns false.
inline bool peelVar(const uint8_t* p, size_t n, uint32_t& value, size_t& used) {
  value = 0;
  used = 0;
  for (int shift = 0; shift < 35; shift += 7) {
    if (used >= n) return false;
    uint8_t byte = p[used++];
    value |= static_cast<uint32_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) return true;
  }
  return false;
}

inline constexpr size_t kMaxPacket = 8 * 1024 * 1024;

inline bool zlibCompress(const uint8_t* in, size_t n, std::vector<uint8_t>& out) {
  uLongf bound = compressBound(static_cast<uLong>(n));
  out.resize(bound);
  uLongf got = bound;
  if (compress2(out.data(), &got, in, static_cast<uLong>(n), Z_DEFAULT_COMPRESSION) != Z_OK) return false;
  out.resize(got);
  return true;
}

inline bool zlibUncompress(const uint8_t* in, size_t n, size_t rawLen, std::vector<uint8_t>& out) {
  if (rawLen > kMaxPacket) return false;
  out.resize(rawLen);
  uLongf got = static_cast<uLongf>(rawLen);
  if (uncompress(out.data(), &got, in, static_cast<uLong>(n)) != Z_OK) return false;
  if (got != rawLen) return false;
  return true;
}

// compression < 0: classic length prefix. Otherwise the 1.8+ compressed frame.
inline std::vector<uint8_t> encodeFrame(int compression, int packetId, const Buf& body) {
  Buf raw;
  raw.wVar(static_cast<uint32_t>(packetId));
  raw.wRaw(body.b.data(), body.b.size());
  Buf frame;
  if (compression < 0) {
    frame.wVar(static_cast<uint32_t>(raw.b.size()));
    frame.wRaw(raw.b.data(), raw.b.size());
    return frame.b;
  }
  if (raw.b.size() < static_cast<size_t>(compression)) {
    Buf inner;
    inner.wVar(0);
    inner.wRaw(raw.b.data(), raw.b.size());
    frame.wVar(static_cast<uint32_t>(inner.b.size()));
    frame.wRaw(inner.b.data(), inner.b.size());
    return frame.b;
  }
  std::vector<uint8_t> z;
  if (!zlibCompress(raw.b.data(), raw.b.size(), z)) {
    throw std::runtime_error("zlib compress failed");
  }
  Buf inner;
  inner.wVar(static_cast<uint32_t>(raw.b.size()));
  inner.wRaw(z.data(), z.size());
  frame.wVar(static_cast<uint32_t>(inner.b.size()));
  frame.wRaw(inner.b.data(), inner.b.size());
  return frame.b;
}

// On success, `packet` is the uncompressed id+body and those bytes are consumed from `in`.
inline bool decodeFrame(std::vector<uint8_t>& in, int compression, std::vector<uint8_t>& packet, std::string& err) {
  uint32_t len = 0;
  size_t used = 0;
  if (!peelVar(in.data(), in.size(), len, used)) return false;
  if (len > kMaxPacket) {
    err = "frame too large";
    return false;
  }
  if (in.size() < used + len) return false;
  const uint8_t* payload = in.data() + used;
  if (compression < 0) {
    packet.assign(payload, payload + len);
  } else {
    uint32_t dataLen = 0;
    size_t dused = 0;
    if (!peelVar(payload, len, dataLen, dused)) {
      err = "truncated compression header";
      in.erase(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(used + len));
      return false;
    }
    if (dataLen == 0) {
      packet.assign(payload + dused, payload + len);
    } else if (!zlibUncompress(payload + dused, len - dused, dataLen, packet)) {
      err = "zlib inflate failed";
      in.erase(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(used + len));
      return false;
    }
  }
  in.erase(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(used + len));
  return true;
}

inline int packetIdOf(const std::vector<uint8_t>& packet, Buf& body) {
  body = bufFrom(packet.data(), packet.size());
  auto id = static_cast<int>(body.rVar());
  if (!body.ok) return -1;
  Buf rest;
  rest.b.assign(body.b.begin() + static_cast<std::ptrdiff_t>(body.r), body.b.end());
  body = std::move(rest);
  return id;
}

// Java UUID.nameUUIDFromBytes("OfflinePlayer:" + name): MD5, version 3, IETF variant.
inline std::array<uint8_t, 16> offlineUuid(std::string_view name) {
  std::string text = "OfflinePlayer:";
  text.append(name);
  std::array<uint8_t, 16> out{};
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) throw std::runtime_error("md5 context");
  unsigned int n = 0;
  bool good = EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, text.data(), text.size()) == 1 &&
              EVP_DigestFinal_ex(ctx, out.data(), &n) == 1 && n == 16;
  EVP_MD_CTX_free(ctx);
  if (!good) throw std::runtime_error("md5");
  out[6] = static_cast<uint8_t>((out[6] & 0x0F) | 0x30);
  out[8] = static_cast<uint8_t>((out[8] & 0x3F) | 0x80);
  return out;
}

template <typename E>
inline int pid(E e) {
  return static_cast<int>(e);
}

}  // namespace ckmc
