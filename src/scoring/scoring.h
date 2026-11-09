#pragma once

#include <array>
#include <cstddef>
#include <shared_mutex>
#include <span>

#include "common/containers.h"
#include "domain/model.h"

namespace ad {

// Bayesian-smoothed click-through rate per (campaign, slot, device).
class CtrTable {
 public:
  struct Stat {
    float impressions = 0.f;
    float clicks = 0.f;
  };

  CtrTable(float alpha, float beta) : alpha_(alpha), beta_(beta) {}

  float lookup(u64 campaign, u32 slot, DeviceId device) const;
  void update(const Event& event);
  Stat stat(u64 campaign, u32 slot, DeviceId device) const;

  void decay(float factor);

 private:
  struct Key {
    u64 campaign;
    u32 slot;
    DeviceId device;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    std::size_t operator()(const Key& k) const {
      return static_cast<std::size_t>(
          combineHash(k.campaign, (static_cast<u64>(k.slot) << 32) | k.device));
    }
  };
  static constexpr std::size_t kShards = 16;
  struct Shard {
    mutable std::shared_mutex mutex;
    flat_hash_map<Key, Stat, KeyHash> stats;
  };
  const Shard& shardFor(const Key& key) const { return shards_[KeyHash{}(key) % kShards]; }
  Shard& shardFor(const Key& key) { return shards_[KeyHash{}(key) % kShards]; }

  std::array<Shard, kShards> shards_;
  float alpha_;
  float beta_;
};

// Sets c.pctr and c.ecpmMicros, and returns the ranking score.
class Scorer {
 public:
  virtual ~Scorer() = default;
  virtual float score(Candidate& c, const AdRequest& req, const UserContext& user) = 0;
};

// Ranks on the bid alone. CPC bids use one assumed click-through rate.
class BidScorer : public Scorer {
 public:
  explicit BidScorer(float assumedCtr = 0.01f) : assumedCtr_(assumedCtr) {}
  float score(Candidate& c, const AdRequest& req, const UserContext& user) override;

 private:
  float assumedCtr_;
};

class CtrScorer : public Scorer {
 public:
  CtrScorer(CtrTable& ctr, float epsilon, float priorCtr)
      : ctr_(ctr), epsilon_(epsilon), priorCtr_(priorCtr) {}
  float score(Candidate& c, const AdRequest& req, const UserContext& user) override;

 private:
  float explore(const Candidate& c) const;

  CtrTable& ctr_;
  float epsilon_;
  float priorCtr_;
};

// `scored` must have score and ecpmMicros set. Prices are CPM micros.
class Auction {
 public:
  virtual ~Auction() = default;
  virtual AuctionResult run(std::span<const Candidate> scored, i64 floorMicros) = 0;
};

class FirstPriceAuction : public Auction {
 public:
  AuctionResult run(std::span<const Candidate> scored, i64 floorMicros) override;
};

enum class TieBreakRule : u8 { HigherBid, LowerCampaignIdx };

class SecondPriceAuction : public Auction {
 public:
  explicit SecondPriceAuction(TieBreakRule tieBreak = TieBreakRule::HigherBid,
                              i64 minClearingMicros = 1000)
      : tieBreak_(tieBreak), minClearingMicros_(minClearingMicros) {}

  AuctionResult run(std::span<const Candidate> scored, i64 floorMicros) override;

 private:
  bool beats(const Candidate& a, const Candidate& b) const;
  i64 clearingPrice(const Candidate& winner, const Candidate* runnerUp, i64 floorMicros) const;

  TieBreakRule tieBreak_;
  i64 minClearingMicros_;  // keeps a lone bidder with no floor from serving free
};

}
