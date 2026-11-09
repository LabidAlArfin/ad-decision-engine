#pragma once

#include <vector>

#include "common/types.h"

namespace ad {

// A compact Roaring bitmap: values are split on their high 16 bits, and each
// chunk is a sorted array while sparse and a 65,536-bit bitmap once dense.
// The method names follow CRoaring closely enough to swap it in later.
class Roaring {
 public:
  void add(u32 value);
  bool contains(u32 value) const;
  u64 cardinality() const;
  bool isEmpty() const { return chunks_.empty(); }

  Roaring& operator|=(const Roaring& other);
  Roaring& operator&=(const Roaring& other);
  Roaring& operator-=(const Roaring& other);

  // Appends every value in ascending order.
  void appendTo(std::vector<u32>& out) const;

 private:
  struct Chunk {
    u16 key = 0;
    u32 count = 0;
    std::vector<u16> sorted;
    std::vector<u64> bits;
    bool isBitmap() const { return !bits.empty(); }
  };

  static void toBitmap(Chunk& c);
  static void normalize(Chunk& c);
  static void mergeOr(Chunk& a, const Chunk& b);
  static void mergeAnd(Chunk& a, const Chunk& b);
  static void mergeAndNot(Chunk& a, const Chunk& b);

  std::vector<Chunk> chunks_;  // sorted by key, never holds an empty chunk
};

}
