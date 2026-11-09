#include "state/runtime_state.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string>

namespace ad {
namespace {

constexpr std::size_t kSyncChunk = 500;
constexpr int kSyncTimeoutMs = 250;
constexpr int kDayKeyTtlSec = 2 * 86'400;

constexpr i64 kMinRefillMs = 50;
constexpr float kBurstSeconds = 60.f;
constexpr float kMinMultiplier = 0.2f;
constexpr float kMaxMultiplier = 5.f;
constexpr float kPacingGain = 0.25f;

// A late or duplicate release must not push a counter below zero.
void subtractFloorZero(std::atomic<i64>& counter, i64 amount) {
  i64 current = counter.load(std::memory_order_relaxed);
  while (!counter.compare_exchange_weak(current, std::max<i64>(current - amount, 0),
                                        std::memory_order_acq_rel)) {
  }
}

u16 clampU16(i64 value) {
  return static_cast<u16>(std::clamp<i64>(value, 0, std::numeric_limits<u16>::max()));
}

}

BudgetManager::BudgetManager(CampaignSlots& slots, RedisClient* redis, int syncSec)
    : slots_(slots),
      redis_(redis),
      syncSec_(syncSec),
      size_(slots.capacity()),
      spent_(size_),
      lifetime_(size_),
      reserved_(size_),
      expiring_(size_),
      unsynced_(size_),
      unsyncedToday_(size_),
      dailyLimit_(size_),
      totalLimit_(size_),
      campaignIds_(size_),
      currentDay_(utcDay(nowTime())) {}

void BudgetManager::applyCatalog(const CatalogSnapshot& snapshot) {
  const std::size_t n = std::min(snapshot.campaigns.size(), size_);
  for (std::size_t idx = 0; idx < n; ++idx) {
    const Campaign& c = snapshot.campaigns[idx];
    if (c.id == 0) continue;
    dailyLimit_[idx].store(c.dailyBudgetMicros, std::memory_order_relaxed);
    totalLimit_[idx].store(c.totalBudgetMicros, std::memory_order_relaxed);
    campaignIds_[idx].store(c.id, std::memory_order_relaxed);
  }
}

i64 BudgetManager::remaining(u32 idx) const {
  const i64 daily = dailyLimit_[idx].load(std::memory_order_relaxed) -
                    spent_[idx].load(std::memory_order_relaxed);
  const i64 total = totalLimit_[idx].load(std::memory_order_relaxed) -
                    lifetime_[idx].load(std::memory_order_relaxed);
  return std::min(daily, total) - reserved_[idx].load(std::memory_order_acquire);
}

bool BudgetManager::reserve(u32 idx, i64 amount) {
  if (amount <= 0) return true;
  // Add first and check after, so that concurrent winners cannot all see the
  // same headroom. Near exhaustion this can reject a win that would have fit,
  // which errs on the side of not overspending.
  reserved_[idx].fetch_add(amount, std::memory_order_acq_rel);
  if (remaining(idx) < 0) {
    reserved_[idx].fetch_sub(amount, std::memory_order_acq_rel);
    return false;
  }
  return true;
}

void BudgetManager::release(u32 idx, i64 amount) {
  if (amount <= 0) return;
  subtractFloorZero(reserved_[idx], amount);
  subtractFloorZero(expiring_[idx], amount);
}

void BudgetManager::commit(const Event& event) {
  const std::optional<u32> idx = slots_.find(event.campaignId);
  if (!idx) return;
  if (event.priceMicros > 0) {
    spent_[*idx].fetch_add(event.priceMicros, std::memory_order_relaxed);
    lifetime_[*idx].fetch_add(event.priceMicros, std::memory_order_relaxed);
    unsynced_[*idx].fetch_add(event.priceMicros, std::memory_order_relaxed);
    unsyncedToday_[*idx].fetch_add(event.priceMicros, std::memory_order_relaxed);
  }
  release(*idx, event.reservedMicros);
}

void BudgetManager::expireReservations() {
  for (std::size_t idx = 0; idx < size_; ++idx) {
    const i64 stale = expiring_[idx].load(std::memory_order_acquire);
    if (stale > 0) subtractFloorZero(reserved_[idx], stale);
    expiring_[idx].store(reserved_[idx].load(std::memory_order_acquire), std::memory_order_release);
  }
}

void BudgetManager::syncWithRedis() {
  if (redis_ == nullptr) return;
  const std::string day = std::to_string(currentDay_.load(std::memory_order_relaxed));

  struct Entry {
    std::size_t idx;
    i64 delta;
    i64 dayDelta;
  };
  std::vector<Entry> entries;
  std::vector<RedisCommand> commands;

  const auto flush = [&] {
    if (entries.empty()) return;
    Replies replies = redis_->pipeline(commands, kSyncTimeoutMs);
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const Entry& entry = entries[i];
      const bool ok = replies.size() == commands.size() &&
                      replies[i * 3].type == RedisReply::Type::Integer &&
                      replies[i * 3 + 2].type == RedisReply::Type::Integer;
      if (!ok) {
        // Retry on the next sync. If Redis applied the write but its reply was
        // lost, spend is counted twice: the safe direction for a budget.
        unsynced_[entry.idx].fetch_add(entry.delta, std::memory_order_relaxed);
        unsyncedToday_[entry.idx].fetch_add(entry.dayDelta, std::memory_order_relaxed);
        continue;
      }
      // Local spend committed since the exchange is still unsynced.
      const i64 pendingToday = unsyncedToday_[entry.idx].load(std::memory_order_relaxed);
      const i64 pendingTotal = unsynced_[entry.idx].load(std::memory_order_relaxed);
      spent_[entry.idx].store(replies[i * 3].integer + pendingToday, std::memory_order_relaxed);
      lifetime_[entry.idx].store(replies[i * 3 + 2].integer + pendingTotal, std::memory_order_relaxed);
    }
    entries.clear();
    commands.clear();
  };

  for (std::size_t idx = 0; idx < size_; ++idx) {
    const u64 id = campaignIds_[idx].load(std::memory_order_relaxed);
    if (id == 0) continue;
    const i64 delta = unsynced_[idx].exchange(0, std::memory_order_relaxed);
    const i64 dayDelta = unsyncedToday_[idx].exchange(0, std::memory_order_relaxed);
    const std::string idText = std::to_string(id);
    const std::string dayKey = "budget:day:" + day + ":" + idText;
    // INCRBY with a delta of 0 still returns the total, which is how spend made
    // by other instances, or before a restart, reaches this process.
    commands.push_back({"INCRBY", dayKey, std::to_string(dayDelta)});
    commands.push_back({"EXPIRE", dayKey, std::to_string(kDayKeyTtlSec)});
    commands.push_back({"INCRBY", "budget:total:" + idText, std::to_string(delta)});
    entries.push_back({idx, delta, dayDelta});
    if (entries.size() == kSyncChunk) flush();
  }
  flush();
}

void BudgetManager::rollDay(Time now) {
  const i64 day = utcDay(now);
  if (day == currentDay_.load(std::memory_order_relaxed)) return;
  syncWithRedis();  // yesterday's spend belongs under yesterday's key
  currentDay_.store(day, std::memory_order_relaxed);
  for (std::size_t idx = 0; idx < size_; ++idx) {
    spent_[idx].store(0, std::memory_order_relaxed);
    // If that sync failed, yesterday's spend must not land on today's key. It
    // is still in unsynced_, so the lifetime total is not lost.
    unsyncedToday_[idx].store(0, std::memory_order_relaxed);
  }
}

PacingController::PacingController(u32 capacity)
    : size_(capacity),
      tokens_(size_),
      lastRefill_(size_),
      ratePerSec_(size_),
      multiplier_(size_),
      even_(size_) {
  for (auto& m : multiplier_) m.store(1.f, std::memory_order_relaxed);
}

void PacingController::applyCatalog(const CatalogSnapshot& snapshot, Time now) {
  const std::size_t n = std::min(snapshot.campaigns.size(), size_);
  for (std::size_t idx = 0; idx < n; ++idx) {
    const Campaign& c = snapshot.campaigns[idx];
    if (c.id == 0) continue;
    const float rate = static_cast<float>(c.dailyBudgetMicros) / 86'400.f;
    ratePerSec_[idx].store(rate, std::memory_order_relaxed);
    even_[idx].store(c.pacing == PacingMode::Even, std::memory_order_relaxed);
    i64 expected = 0;
    if (lastRefill_[idx].compare_exchange_strong(expected, toUnixMs(now))) {
      tokens_[idx].store(rate, std::memory_order_relaxed);  // new campaign: one second's worth
    }
  }
}

void PacingController::addTokens(u32 idx, float delta, float cap) {
  float current = tokens_[idx].load(std::memory_order_relaxed);
  float next;
  do {
    next = cap > 0.f ? std::min(current + delta, cap) : current + delta;
  } while (!tokens_[idx].compare_exchange_weak(current, next, std::memory_order_relaxed));
}

void PacingController::refillOne(u32 idx, i64 nowMs) {
  i64 last = lastRefill_[idx].load(std::memory_order_relaxed);
  if (nowMs - last < kMinRefillMs) return;
  if (!lastRefill_[idx].compare_exchange_strong(last, nowMs, std::memory_order_relaxed)) return;
  const float rate = ratePerSec_[idx].load(std::memory_order_relaxed) *
                     multiplier_[idx].load(std::memory_order_relaxed);
  addTokens(idx, rate * static_cast<float>(nowMs - last) / 1000.f, rate * kBurstSeconds);
}

bool PacingController::allowed(u32 idx, Time now) {
  if (!even_[idx].load(std::memory_order_relaxed)) return true;
  refillOne(idx, toUnixMs(now));
  return tokens_[idx].load(std::memory_order_relaxed) > 0.f;
}

void PacingController::refill(Time now) {
  const i64 nowMs = toUnixMs(now);
  for (std::size_t idx = 0; idx < size_; ++idx) {
    if (even_[idx].load(std::memory_order_relaxed)) refillOne(static_cast<u32>(idx), nowMs);
  }
}

void PacingController::adjustRate(u32 idx, float err) {
  float current = multiplier_[idx].load(std::memory_order_relaxed);
  float next;
  do {
    next = std::clamp(current * (1.f + kPacingGain * std::clamp(err, -1.f, 1.f)), kMinMultiplier,
                      kMaxMultiplier);
  } while (!multiplier_[idx].compare_exchange_weak(current, next, std::memory_order_relaxed));
}

void PacingController::consume(u32 idx, i64 amountMicros) {
  if (!even_[idx].load(std::memory_order_relaxed)) return;
  // No cap: a win may take the bucket negative, which then has to be refilled.
  addTokens(idx, -static_cast<float>(amountMicros), 0.f);
}

FrequencyCapStore::FrequencyCapStore(RedisClient* redis, std::size_t localCapacity, int ttlSec,
                                     std::chrono::milliseconds localFreshness)
    : redis_(redis), local_(localCapacity, localFreshness), ttlSec_(ttlSec) {}

std::string FrequencyCapStore::redisKey(const Key& key) {
  return "fc:" + std::to_string(key.day) + ":" + std::to_string(key.user) + ":" +
         std::to_string(key.campaign);
}

u16 FrequencyCapStore::count(u64 user, u64 campaign) {
  return countBatch(user, std::span<const u64>(&campaign, 1))[0];
}

std::vector<u16> FrequencyCapStore::countBatch(u64 user, std::span<const u64> campaigns) {
  const i64 day = utcDay(nowTime());
  std::vector<u16> out(campaigns.size(), 0);
  std::vector<std::string> keys;
  std::vector<std::size_t> missing;
  for (std::size_t i = 0; i < campaigns.size(); ++i) {
    const Key key{user, campaigns[i], day};
    if (const std::optional<u16> cached = local_.get(key)) {
      out[i] = *cached;
    } else {
      keys.push_back(redisKey(key));
      missing.push_back(i);
    }
  }
  if (keys.empty() || redis_ == nullptr) return out;

  const auto values = redis_->mget(keys);
  if (values.size() != keys.size()) return out;
  for (std::size_t j = 0; j < keys.size(); ++j) {
    u64 parsed = 0;
    if (values[j]) {
      const std::string& text = *values[j];
      std::from_chars(text.data(), text.data() + text.size(), parsed);
    }
    const u16 n = clampU16(static_cast<i64>(std::min<u64>(parsed, 65535)));
    out[missing[j]] = n;
    local_.put(Key{user, campaigns[missing[j]], day}, n);
  }
  return out;
}

void FrequencyCapStore::increment(u64 user, u64 campaign) {
  const Key key{user, campaign, utcDay(nowTime())};
  const i64 updated = redis_ != nullptr ? redis_->incr(redisKey(key), ttlSec_) : -1;
  if (updated >= 0) {
    local_.put(key, clampU16(updated));
  } else {
    local_.upsert(key, [](std::optional<u16> v) {
      return v ? clampU16(i64{*v} + 1) : u16{1};
    });
  }
}

UserProfileStore::UserProfileStore(RedisClient* redis, std::size_t capacity,
                                   std::chrono::milliseconds freshness)
    : redis_(redis), cache_(capacity, freshness) {}

std::vector<SegId> UserProfileStore::segments(u64 userId) {
  if (auto cached = cache_.get(userId)) return std::move(cached->segments);
  if (redis_ == nullptr) return {};

  const std::optional<std::string> raw = redis_->hget("user:" + std::to_string(userId), "segments");
  if (!raw) return {};  // Not cached: an outage must not stick as "no segments".

  Profile profile;
  std::string_view rest = *raw;
  while (!rest.empty()) {
    const std::size_t comma = rest.find(',');
    const std::string_view item = rest.substr(0, comma);
    SegId id = 0;
    auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), id);
    if (ec == std::errc{} && end == item.data() + item.size()) profile.segments.push_back(id);
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
  }
  cache_.put(userId, profile);
  return profile.segments;
}

}
