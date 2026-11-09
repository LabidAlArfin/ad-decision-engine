#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <span>
#include <vector>

#include "catalog/catalog.h"
#include "common/containers.h"
#include "domain/model.h"
#include "state/redis_client.h"

namespace ad {

class BudgetManager {
 public:
  BudgetManager(CampaignSlots& slots, RedisClient* redis, int syncSec);

  void applyCatalog(const CatalogSnapshot& snapshot);

  // Spendable micros after what is spent and what is held for recent wins.
  i64 remaining(u32 idx) const;

  bool reserve(u32 idx, i64 amount);
  void commit(const Event& event);
  void release(u32 idx, i64 amount);

  // Pushes local spend to Redis and pulls the fleet-wide totals back.
  void syncWithRedis();

  // Drops holds for impressions that never arrived. Each hold survives between
  // one and two calls, so call at half the desired hold lifetime.
  void expireReservations();

  void rollDay(Time now);

  i64 spentToday(u32 idx) const { return spent_[idx].load(std::memory_order_relaxed); }
  i64 reserved(u32 idx) const { return reserved_[idx].load(std::memory_order_relaxed); }
  int syncSec() const { return syncSec_; }

 private:
  CampaignSlots& slots_;
  RedisClient* redis_;
  int syncSec_;
  std::size_t size_;
  std::vector<std::atomic<i64>> spent_;     // today, fleet-wide as of the last sync
  std::vector<std::atomic<i64>> lifetime_;
  std::vector<std::atomic<i64>> reserved_;
  std::vector<std::atomic<i64>> expiring_;  // part of reserved_ that the next tick drops
  std::vector<std::atomic<i64>> unsynced_;  // local spend not yet pushed to the total key
  std::vector<std::atomic<i64>> unsyncedToday_;  // the same, for the current day's key
  std::vector<std::atomic<i64>> dailyLimit_;
  std::vector<std::atomic<i64>> totalLimit_;
  std::vector<std::atomic<u64>> campaignIds_;
  std::atomic<i64> currentDay_;
};

class PacingController {
 public:
  explicit PacingController(u32 capacity);

  void applyCatalog(const CatalogSnapshot& snapshot, Time now);

  bool allowed(u32 idx, Time now);
  void refill(Time now);

  // err > 0 means under-delivering; the spend rate is nudged up. Bounded.
  void adjustRate(u32 idx, float err);

  // Spends tokens for a win. A negative amount refunds them.
  void consume(u32 idx, i64 amountMicros);

  float tokens(u32 idx) const { return tokens_[idx].load(std::memory_order_relaxed); }
  float rateMultiplier(u32 idx) const { return multiplier_[idx].load(std::memory_order_relaxed); }

 private:
  void refillOne(u32 idx, i64 nowMs);
  void addTokens(u32 idx, float delta, float cap);

  std::size_t size_;
  std::vector<std::atomic<float>> tokens_;       // micros of spend
  std::vector<std::atomic<i64>> lastRefill_;     // unix ms
  std::vector<std::atomic<float>> ratePerSec_;   // micros per second
  std::vector<std::atomic<float>> multiplier_;
  std::vector<std::atomic<bool>> even_;
};

class FrequencyCapStore {
 public:
  FrequencyCapStore(RedisClient* redis, std::size_t localCapacity, int ttlSec,
                    std::chrono::milliseconds localFreshness = std::chrono::seconds(5));

  u16 count(u64 user, u64 campaign);

  // One MGET for everything the local cache cannot answer. Fails open: during
  // a Redis outage only local counts apply.
  std::vector<u16> countBatch(u64 user, std::span<const u64> campaigns);
  void increment(u64 user, u64 campaign);

 private:
  struct Key {
    u64 user;
    u64 campaign;
    i64 day;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    std::size_t operator()(const Key& k) const {
      return static_cast<std::size_t>(
          combineHash(combineHash(k.user, k.campaign), static_cast<u64>(k.day)));
    }
  };
  static std::string redisKey(const Key& key);

  RedisClient* redis_;
  LruCache<Key, u16, KeyHash> local_;
  int ttlSec_;
};

class UserProfileStore {
 public:
  UserProfileStore(RedisClient* redis, std::size_t capacity,
                   std::chrono::milliseconds freshness = std::chrono::seconds(60));

  // Reads the "segments" field of hash user:<id>, a comma-separated id list.
  std::vector<SegId> segments(u64 userId);

 private:
  struct Profile {
    std::vector<SegId> segments;
  };
  RedisClient* redis_;
  LruCache<u64, Profile> cache_;
};

}
