#include "common/roaring.h"

#include <algorithm>
#include <bit>
#include <iterator>

namespace ad {
namespace {

constexpr u32 kArrayMax = 4096;
constexpr std::size_t kWords = 1024;

inline bool testBit(const std::vector<u64>& bits, u16 v) {
  return (bits[v >> 6] >> (v & 63)) & 1;
}
inline void setBit(std::vector<u64>& bits, u16 v) { bits[v >> 6] |= u64{1} << (v & 63); }
inline void clearBit(std::vector<u64>& bits, u16 v) { bits[v >> 6] &= ~(u64{1} << (v & 63)); }

}

void Roaring::toBitmap(Chunk& c) {
  if (c.isBitmap()) return;
  c.bits.assign(kWords, 0);
  for (u16 v : c.sorted) setBit(c.bits, v);
  c.sorted = {};
}

// Recounts a chunk and falls back to the array form when it has become sparse.
void Roaring::normalize(Chunk& c) {
  if (!c.isBitmap()) {
    c.count = static_cast<u32>(c.sorted.size());
    return;
  }
  u32 n = 0;
  for (u64 w : c.bits) n += static_cast<u32>(std::popcount(w));
  c.count = n;
  if (n > kArrayMax) return;
  c.sorted.clear();
  c.sorted.reserve(n);
  for (std::size_t i = 0; i < kWords; ++i) {
    u64 w = c.bits[i];
    while (w != 0) {
      c.sorted.push_back(static_cast<u16>(i * 64 + static_cast<std::size_t>(std::countr_zero(w))));
      w &= w - 1;
    }
  }
  c.bits = {};
}

void Roaring::mergeOr(Chunk& a, const Chunk& b) {
  if (!a.isBitmap() && !b.isBitmap() && a.sorted.size() + b.sorted.size() <= kArrayMax) {
    std::vector<u16> out;
    out.reserve(a.sorted.size() + b.sorted.size());
    std::set_union(a.sorted.begin(), a.sorted.end(), b.sorted.begin(), b.sorted.end(),
                   std::back_inserter(out));
    a.sorted = std::move(out);
    normalize(a);
    return;
  }
  toBitmap(a);
  if (b.isBitmap()) {
    for (std::size_t i = 0; i < kWords; ++i) a.bits[i] |= b.bits[i];
  } else {
    for (u16 v : b.sorted) setBit(a.bits, v);
  }
  normalize(a);
}

void Roaring::mergeAnd(Chunk& a, const Chunk& b) {
  if (a.isBitmap() && b.isBitmap()) {
    for (std::size_t i = 0; i < kWords; ++i) a.bits[i] &= b.bits[i];
  } else if (a.isBitmap()) {
    std::vector<u16> out;
    for (u16 v : b.sorted) {
      if (testBit(a.bits, v)) out.push_back(v);
    }
    a.bits = {};
    a.sorted = std::move(out);
  } else if (b.isBitmap()) {
    std::erase_if(a.sorted, [&](u16 v) { return !testBit(b.bits, v); });
  } else {
    std::vector<u16> out;
    std::set_intersection(a.sorted.begin(), a.sorted.end(), b.sorted.begin(), b.sorted.end(),
                          std::back_inserter(out));
    a.sorted = std::move(out);
  }
  normalize(a);
}

void Roaring::mergeAndNot(Chunk& a, const Chunk& b) {
  if (a.isBitmap()) {
    if (b.isBitmap()) {
      for (std::size_t i = 0; i < kWords; ++i) a.bits[i] &= ~b.bits[i];
    } else {
      for (u16 v : b.sorted) clearBit(a.bits, v);
    }
  } else if (b.isBitmap()) {
    std::erase_if(a.sorted, [&](u16 v) { return testBit(b.bits, v); });
  } else {
    std::vector<u16> out;
    std::set_difference(a.sorted.begin(), a.sorted.end(), b.sorted.begin(), b.sorted.end(),
                        std::back_inserter(out));
    a.sorted = std::move(out);
  }
  normalize(a);
}

void Roaring::add(u32 value) {
  const u16 key = static_cast<u16>(value >> 16);
  const u16 low = static_cast<u16>(value & 0xFFFF);
  auto it = std::lower_bound(chunks_.begin(), chunks_.end(), key,
                             [](const Chunk& c, u16 k) { return c.key < k; });
  if (it == chunks_.end() || it->key != key) {
    Chunk c;
    c.key = key;
    it = chunks_.insert(it, std::move(c));
  }
  Chunk& c = *it;
  if (c.isBitmap()) {
    if (!testBit(c.bits, low)) {
      setBit(c.bits, low);
      ++c.count;
    }
    return;
  }
  auto pos = std::lower_bound(c.sorted.begin(), c.sorted.end(), low);
  if (pos != c.sorted.end() && *pos == low) return;
  c.sorted.insert(pos, low);
  ++c.count;
  if (c.sorted.size() > kArrayMax) toBitmap(c);
}

bool Roaring::contains(u32 value) const {
  const u16 key = static_cast<u16>(value >> 16);
  const u16 low = static_cast<u16>(value & 0xFFFF);
  auto it = std::lower_bound(chunks_.begin(), chunks_.end(), key,
                             [](const Chunk& c, u16 k) { return c.key < k; });
  if (it == chunks_.end() || it->key != key) return false;
  if (it->isBitmap()) return testBit(it->bits, low);
  return std::binary_search(it->sorted.begin(), it->sorted.end(), low);
}

u64 Roaring::cardinality() const {
  u64 total = 0;
  for (const Chunk& c : chunks_) total += c.count;
  return total;
}

Roaring& Roaring::operator|=(const Roaring& other) {
  std::vector<Chunk> out;
  out.reserve(chunks_.size() + other.chunks_.size());
  std::size_t i = 0, j = 0;
  while (i < chunks_.size() || j < other.chunks_.size()) {
    if (j == other.chunks_.size() || (i < chunks_.size() && chunks_[i].key < other.chunks_[j].key)) {
      out.push_back(std::move(chunks_[i++]));
    } else if (i == chunks_.size() || other.chunks_[j].key < chunks_[i].key) {
      out.push_back(other.chunks_[j++]);
    } else {
      mergeOr(chunks_[i], other.chunks_[j]);
      out.push_back(std::move(chunks_[i]));
      ++i;
      ++j;
    }
  }
  chunks_ = std::move(out);
  return *this;
}

Roaring& Roaring::operator&=(const Roaring& other) {
  std::vector<Chunk> out;
  std::size_t i = 0, j = 0;
  while (i < chunks_.size() && j < other.chunks_.size()) {
    if (chunks_[i].key < other.chunks_[j].key) {
      ++i;
    } else if (other.chunks_[j].key < chunks_[i].key) {
      ++j;
    } else {
      mergeAnd(chunks_[i], other.chunks_[j]);
      if (chunks_[i].count > 0) out.push_back(std::move(chunks_[i]));
      ++i;
      ++j;
    }
  }
  chunks_ = std::move(out);
  return *this;
}

Roaring& Roaring::operator-=(const Roaring& other) {
  std::vector<Chunk> out;
  out.reserve(chunks_.size());
  std::size_t j = 0;
  for (std::size_t i = 0; i < chunks_.size(); ++i) {
    while (j < other.chunks_.size() && other.chunks_[j].key < chunks_[i].key) ++j;
    if (j < other.chunks_.size() && other.chunks_[j].key == chunks_[i].key) {
      mergeAndNot(chunks_[i], other.chunks_[j]);
      if (chunks_[i].count == 0) continue;
    }
    out.push_back(std::move(chunks_[i]));
  }
  chunks_ = std::move(out);
  return *this;
}

void Roaring::appendTo(std::vector<u32>& out) const {
  out.reserve(out.size() + cardinality());
  for (const Chunk& c : chunks_) {
    const u32 high = static_cast<u32>(c.key) << 16;
    if (c.isBitmap()) {
      for (std::size_t i = 0; i < kWords; ++i) {
        u64 w = c.bits[i];
        while (w != 0) {
          out.push_back(high | static_cast<u32>(i * 64 + static_cast<std::size_t>(std::countr_zero(w))));
          w &= w - 1;
        }
      }
    } else {
      for (u16 v : c.sorted) out.push_back(high | v);
    }
  }
}

}
