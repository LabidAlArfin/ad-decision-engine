#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>
#include <version>

#include "common/types.h"

namespace ad {

// Alias so that absl::flat_hash_map can be dropped in without touching callers.
template <class K, class V, class Hash = std::hash<K>, class Eq = std::equal_to<K>>
using flat_hash_map = std::unordered_map<K, V, Hash, Eq>;

template <class K, class V>
class small_map {
 public:
  const V* find(const K& key) const {
    auto it = lowerBound(key);
    return it != items_.end() && it->first == key ? &it->second : nullptr;
  }
  void set(const K& key, V value) {
    auto it = lowerBound(key);
    if (it != items_.end() && it->first == key) {
      it->second = std::move(value);
    } else {
      items_.insert(it, {key, std::move(value)});
    }
  }
  // Bulk replacement, avoiding quadratic inserts when many keys arrive at once.
  void assign(std::vector<std::pair<K, V>> items) {
    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    items_ = std::move(items);
  }
  std::size_t size() const { return items_.size(); }

 private:
  auto lowerBound(const K& key) const {
    return std::lower_bound(items_.begin(), items_.end(), key,
                            [](const auto& item, const K& k) { return item.first < k; });
  }
  auto lowerBound(const K& key) {
    return std::lower_bound(items_.begin(), items_.end(), key,
                            [](const auto& item, const K& k) { return item.first < k; });
  }
  std::vector<std::pair<K, V>> items_;
};

// Sharded, thread-safe LRU with an optional per-entry time-to-live.
template <class K, class V, class Hash = std::hash<K>>
class LruCache {
 public:
  explicit LruCache(std::size_t capacity,
                    std::chrono::milliseconds ttl = std::chrono::milliseconds::zero())
      : perShard_(std::max<std::size_t>(1, capacity / kShards)), ttl_(ttl) {}

  std::optional<V> get(const K& key) {
    Shard& s = shardFor(key);
    std::lock_guard lock(s.m);
    auto it = s.map.find(key);
    if (it == s.map.end()) return std::nullopt;
    if (expired(*it->second)) {
      s.order.erase(it->second);
      s.map.erase(it);
      return std::nullopt;
    }
    s.order.splice(s.order.begin(), s.order, it->second);
    return it->second->value;
  }

  void put(const K& key, V value) {
    upsert(key, [&](std::optional<V>) { return std::move(value); });
  }

  // Atomically replaces the entry with fn(current-or-nullopt).
  template <class F>
  V upsert(const K& key, F&& fn) {
    Shard& s = shardFor(key);
    std::lock_guard lock(s.m);
    auto it = s.map.find(key);
    if (it != s.map.end()) {
      std::optional<V> current;
      if (!expired(*it->second)) current = std::move(it->second->value);
      it->second->value = fn(std::move(current));
      it->second->storedAt = Clock::now();
      s.order.splice(s.order.begin(), s.order, it->second);
      return it->second->value;
    }
    s.order.push_front(Entry{key, fn(std::optional<V>{}), Clock::now()});
    s.map.emplace(key, s.order.begin());
    if (s.order.size() > perShard_) {
      s.map.erase(s.order.back().key);
      s.order.pop_back();
    }
    return s.order.front().value;
  }

 private:
  using Clock = std::chrono::steady_clock;
  static constexpr std::size_t kShards = 16;
  struct Entry {
    K key;
    V value;
    Clock::time_point storedAt;
  };
  struct Shard {
    std::mutex m;
    std::list<Entry> order;
    std::unordered_map<K, typename std::list<Entry>::iterator, Hash> map;
  };

  bool expired(const Entry& e) const {
    return ttl_.count() > 0 && Clock::now() - e.storedAt >= ttl_;
  }
  Shard& shardFor(const K& key) { return shards_[mixHash(Hash{}(key)) % kShards]; }

  std::array<Shard, kShards> shards_;
  std::size_t perShard_;
  std::chrono::milliseconds ttl_;
};

// Dmitry Vyukov's bounded multi-producer multi-consumer queue.
template <class T>
class MpmcRingBuffer {
 public:
  explicit MpmcRingBuffer(std::size_t minCapacity)
      : mask_(std::bit_ceil(std::max<std::size_t>(minCapacity, 2)) - 1), cells_(mask_ + 1) {
    for (std::size_t i = 0; i <= mask_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }

  bool tryPush(const T& value) {
    std::size_t pos = head_.load(std::memory_order_relaxed);
    Cell* cell;
    for (;;) {
      cell = &cells_[pos & mask_];
      const std::size_t seq = cell->seq.load(std::memory_order_acquire);
      const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
      if (diff == 0) {
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
      } else if (diff < 0) {
        return false;  // full
      } else {
        pos = head_.load(std::memory_order_relaxed);
      }
    }
    cell->value = value;
    cell->seq.store(pos + 1, std::memory_order_release);
    return true;
  }

  bool tryPop(T& out) {
    std::size_t pos = tail_.load(std::memory_order_relaxed);
    Cell* cell;
    for (;;) {
      cell = &cells_[pos & mask_];
      const std::size_t seq = cell->seq.load(std::memory_order_acquire);
      const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
      if (diff == 0) {
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
      } else if (diff < 0) {
        return false;  // empty
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
    out = std::move(cell->value);
    cell->seq.store(pos + mask_ + 1, std::memory_order_release);
    return true;
  }

  std::size_t capacity() const { return mask_ + 1; }

 private:
  struct Cell {
    std::atomic<std::size_t> seq{0};
    T value{};
  };
  std::size_t mask_;
  std::vector<Cell> cells_;
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

// std::atomic<std::shared_ptr<T>> where the standard library has it, otherwise
// the older free functions, which give the same guarantee.
template <class T>
class AtomicSharedPtr {
 public:
#if defined(__cpp_lib_atomic_shared_ptr)
  std::shared_ptr<T> load() const { return p_.load(std::memory_order_acquire); }
  void store(std::shared_ptr<T> next) { p_.store(std::move(next), std::memory_order_release); }

 private:
  std::atomic<std::shared_ptr<T>> p_;
#else
  std::shared_ptr<T> load() const { return std::atomic_load(&p_); }
  void store(std::shared_ptr<T> next) { std::atomic_store(&p_, std::move(next)); }

 private:
  std::shared_ptr<T> p_;
#endif
};

}
