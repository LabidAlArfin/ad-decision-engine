#include "scoring/scoring.h"

#include <algorithm>
#include <mutex>
#include <random>

namespace ad {
namespace {

std::minstd_rand& rng() {
  thread_local std::minstd_rand engine{std::random_device{}()};
  return engine;
}

float unitRandom() { return std::uniform_real_distribution<float>(0.f, 1.f)(rng()); }

// A bid worth nothing cannot win, even with no floor.
bool canBid(const Candidate& c, i64 floorMicros) {
  return c.ecpmMicros > 0 && c.ecpmMicros >= floorMicros;
}

float finishScore(Candidate& c, float pctr) {
  c.pctr = pctr;
  c.ecpmMicros = c.campaign->bidType == BidType::CPM
                     ? c.bidMicros
                     : static_cast<i64>(static_cast<double>(c.bidMicros) * pctr * 1000.0);
  return static_cast<float>(c.ecpmMicros);
}

}

float CtrTable::lookup(u64 campaign, u32 slot, DeviceId device) const {
  const Stat s = stat(campaign, slot, device);
  return std::min(1.f, (s.clicks + alpha_) / (s.impressions + alpha_ + beta_));
}

CtrTable::Stat CtrTable::stat(u64 campaign, u32 slot, DeviceId device) const {
  const Key key{campaign, slot, device};
  const Shard& shard = shardFor(key);
  std::shared_lock lock(shard.mutex);
  auto it = shard.stats.find(key);
  return it == shard.stats.end() ? Stat{} : it->second;
}

void CtrTable::update(const Event& event) {
  const Key key{event.campaignId, event.slotId, event.device};
  Shard& shard = shardFor(key);
  std::unique_lock lock(shard.mutex);
  Stat& s = shard.stats[key];
  if (event.type == EventType::Impression) {
    s.impressions += 1.f;
  } else {
    s.clicks += 1.f;
  }
}

void CtrTable::decay(float factor) {
  for (Shard& shard : shards_) {
    std::unique_lock lock(shard.mutex);
    for (auto& [key, stat] : shard.stats) {
      stat.impressions *= factor;
      stat.clicks *= factor;
    }
  }
}

float BidScorer::score(Candidate& c, const AdRequest&, const UserContext&) {
  return finishScore(c, assumedCtr_);
}

float CtrScorer::score(Candidate& c, const AdRequest& req, const UserContext&) {
  float pctr = ctr_.lookup(c.campaign->id, req.slotId, req.device);
  if (epsilon_ > 0.f && unitRandom() < epsilon_) pctr = explore(c);
  return finishScore(c, pctr);
}

// An optimistic draw around the prior, which gives campaigns with little data
// a chance to earn real statistics.
float CtrScorer::explore(const Candidate&) const {
  return std::min(1.f, priorCtr_ * (1.f + 2.f * unitRandom()));
}

AuctionResult FirstPriceAuction::run(std::span<const Candidate> scored, i64 floorMicros) {
  AuctionResult result;
  const Candidate* best = nullptr;
  for (const Candidate& c : scored) {
    if (!canBid(c, floorMicros)) continue;
    // A float score cannot separate every bid, so compare the bids on a tie.
    if (best == nullptr || c.score > best->score ||
        (c.score == best->score &&
         (c.ecpmMicros > best->ecpmMicros ||
          (c.ecpmMicros == best->ecpmMicros && c.campaignIdx < best->campaignIdx)))) {
      best = &c;
    }
  }
  if (best == nullptr) {
    result.noFillReason = scored.empty() ? "no_candidates" : "below_floor";
    return result;
  }
  result.winner = *best;
  result.priceMicros = best->ecpmMicros;
  return result;
}

bool SecondPriceAuction::beats(const Candidate& a, const Candidate& b) const {
  if (a.score != b.score) return a.score > b.score;
  if (tieBreak_ == TieBreakRule::HigherBid && a.ecpmMicros != b.ecpmMicros) {
    return a.ecpmMicros > b.ecpmMicros;
  }
  return a.campaignIdx < b.campaignIdx;
}

i64 SecondPriceAuction::clearingPrice(const Candidate& winner, const Candidate* runnerUp,
                                      i64 floorMicros) const {
  const i64 runnerUpBid = runnerUp != nullptr ? runnerUp->ecpmMicros : 0;
  return std::min(winner.ecpmMicros, std::max({runnerUpBid, floorMicros, minClearingMicros_}));
}

AuctionResult SecondPriceAuction::run(std::span<const Candidate> scored, i64 floorMicros) {
  AuctionResult result;
  const Candidate* best = nullptr;
  const Candidate* second = nullptr;
  for (const Candidate& c : scored) {
    if (!canBid(c, floorMicros)) continue;
    if (best == nullptr || beats(c, *best)) {
      second = best;
      best = &c;
    } else if (second == nullptr || beats(c, *second)) {
      second = &c;
    }
  }
  if (best == nullptr) {
    result.noFillReason = scored.empty() ? "no_candidates" : "below_floor";
    return result;
  }
  result.winner = *best;
  result.priceMicros = clearingPrice(*best, second, floorMicros);
  return result;
}

}
