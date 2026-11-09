#include "common/crypto.h"

#include <bit>
#include <cstring>
#include <random>
#include <vector>

namespace ad {
namespace {

constexpr std::array<u32, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void compress(std::array<u32, 8>& state, const u8* block) {
  std::array<u32, 64> w{};
  for (int i = 0; i < 16; ++i) {
    w[i] = (u32{block[i * 4]} << 24) | (u32{block[i * 4 + 1]} << 16) |
           (u32{block[i * 4 + 2]} << 8) | u32{block[i * 4 + 3]};
  }
  for (int i = 16; i < 64; ++i) {
    const u32 s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const u32 s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  auto [a, b, c, d, e, f, g, h] = state;
  for (int i = 0; i < 64; ++i) {
    const u32 s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const u32 ch = (e & f) ^ (~e & g);
    const u32 t1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const u32 s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const u32 maj = (a & b) ^ (a & c) ^ (b & c);
    const u32 t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}

Sha256Digest sha256(std::string_view data) {
  std::array<u32, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const auto* bytes = reinterpret_cast<const u8*>(data.data());
  std::size_t offset = 0;
  for (; offset + 64 <= data.size(); offset += 64) compress(state, bytes + offset);

  std::array<u8, 128> tail{};
  const std::size_t rest = data.size() - offset;
  if (rest != 0) std::memcpy(tail.data(), bytes + offset, rest);
  tail[rest] = 0x80;
  const std::size_t tailLen = rest + 9 <= 64 ? 64 : 128;
  const u64 bitLen = static_cast<u64>(data.size()) * 8;
  for (int i = 0; i < 8; ++i) tail[tailLen - 1 - i] = static_cast<u8>(bitLen >> (8 * i));
  compress(state, tail.data());
  if (tailLen == 128) compress(state, tail.data() + 64);

  Sha256Digest out{};
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<u8>(state[i] >> 24);
    out[i * 4 + 1] = static_cast<u8>(state[i] >> 16);
    out[i * 4 + 2] = static_cast<u8>(state[i] >> 8);
    out[i * 4 + 3] = static_cast<u8>(state[i]);
  }
  return out;
}

Sha256Digest hmacSha256(std::string_view key, std::string_view message) {
  std::array<u8, 64> block{};
  if (key.size() > block.size()) {
    const Sha256Digest hashed = sha256(key);
    std::memcpy(block.data(), hashed.data(), hashed.size());
  } else if (!key.empty()) {
    std::memcpy(block.data(), key.data(), key.size());
  }
  std::string inner(64, '\0');
  std::string outer(64, '\0');
  for (std::size_t i = 0; i < 64; ++i) {
    inner[i] = static_cast<char>(block[i] ^ 0x36);
    outer[i] = static_cast<char>(block[i] ^ 0x5c);
  }
  inner.append(message);
  const Sha256Digest innerDigest = sha256(inner);
  outer.append(reinterpret_cast<const char*>(innerDigest.data()), innerDigest.size());
  return sha256(outer);
}

std::string toHex(const Sha256Digest& digest) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(digest.size() * 2);
  for (u8 b : digest) {
    out += kDigits[b >> 4];
    out += kDigits[b & 0xF];
  }
  return out;
}

bool constantTimeEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  u8 diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<u8>(a[i]) ^ static_cast<u8>(b[i]);
  }
  return diff == 0;
}

std::string randomHex(std::size_t bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::random_device rd;
  std::string out;
  out.reserve(bytes * 2);
  for (std::size_t i = 0; i < bytes; ++i) {
    const unsigned v = rd() & 0xFF;
    out += kDigits[v >> 4];
    out += kDigits[v & 0xF];
  }
  return out;
}

std::string UrlSigner::sign(std::string_view path, std::string_view query) const {
  std::string target;
  target.reserve(path.size() + query.size() + 72);
  target.append(path).append("?").append(query);
  const std::string sig = toHex(hmacSha256(secret_, target));
  target.append("&sig=").append(sig);
  return target;
}

bool UrlSigner::verify(std::string_view target) const {
  static constexpr std::string_view kMarker = "&sig=";
  const std::size_t pos = target.rfind(kMarker);
  if (pos == std::string_view::npos) return false;
  const std::string_view given = target.substr(pos + kMarker.size());
  const std::string expected = toHex(hmacSha256(secret_, target.substr(0, pos)));
  return constantTimeEquals(given, expected);
}

}
