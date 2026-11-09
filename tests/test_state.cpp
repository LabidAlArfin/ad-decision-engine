#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "catalog/catalog.h"
#include "dev/mini_redis.h"
#include "state/redis_client.h"
#include "state/runtime_state.h"
#include "test_framework.h"
#include "test_helpers.h"

using namespace ad;
using namespace adtest;

namespace {

constexpr u64 kCampaignId = 101;

struct BudgetFixture {
  CampaignSlots slots{8};
  std::shared_ptr<const CatalogSnapshot> snapshot;
  BudgetManager budget;
  u32 idx = 0;

  explicit BudgetFixture(i64 daily = 1000, i64 total = 5000, RedisClient* redis = nullptr)
      : budget(slots, redis, 5) {
    CampaignSpec spec;
    spec.id = kCampaignId;
    spec.daily = daily;
    spec.total = total;
    snapshot = buildSnapshot(slots, simpleCatalog({spec}));
    budget.applyCatalog(*snapshot);
    idx = slots.find(kCampaignId).value();
  }

  void spend(i64 price, i64 reserved = 0) {
    Event event;
    event.campaignId = kCampaignId;
    event.priceMicros = price;
    event.reservedMicros = reserved;
    budget.commit(event);
  }
};

struct PacingFixture {
  CampaignSlots slots{8};
  std::shared_ptr<const CatalogSnapshot> snapshot;
  PacingController pacing{8};
  u32 idx = 0;
  Time start = fromUnixMs(kRequestTimeMs);

  // 86,400 micros per day refills at exactly one token per second.
  explicit PacingFixture(const std::string& mode = "EVEN", i64 daily = 86'400) {
    CampaignSpec spec;
    spec.id = kCampaignId;
    spec.daily = daily;
    spec.pacing = mode;
    snapshot = buildSnapshot(slots, simpleCatalog({spec}));
    pacing.applyCatalog(*snapshot, start);
    idx = slots.find(kCampaignId).value();
  }

  Time at(i64 offsetMs) const { return fromUnixMs(kRequestTimeMs + offsetMs); }
};

u16 unusedPort() {
  MiniRedis server;
  server.start();
  const u16 port = server.port();
  server.stop();
  return port;
}

i64 elapsedMs(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

std::string dayKey() { return "budget:day:" + std::to_string(utcDay(nowTime())) + ":" + std::to_string(kCampaignId); }
std::string totalKey() { return "budget:total:" + std::to_string(kCampaignId); }

}

TEST(budget_remaining_is_the_tighter_of_daily_and_total) {
  BudgetFixture dailyBound(1000, 5000);
  EXPECT_EQ(dailyBound.budget.remaining(dailyBound.idx), 1000);
  BudgetFixture totalBound(1000, 400);
  EXPECT_EQ(totalBound.budget.remaining(totalBound.idx), 400);
}

TEST(budget_reserve_holds_money_until_committed) {
  BudgetFixture f;
  EXPECT_TRUE(f.budget.reserve(f.idx, 400));
  EXPECT_EQ(f.budget.remaining(f.idx), 600);
  EXPECT_EQ(f.budget.reserved(f.idx), 400);
  EXPECT_EQ(f.budget.spentToday(f.idx), 0);

  f.spend(400, 400);
  EXPECT_EQ(f.budget.remaining(f.idx), 600);
  EXPECT_EQ(f.budget.reserved(f.idx), 0);
  EXPECT_EQ(f.budget.spentToday(f.idx), 400);
}

TEST(budget_reserve_refuses_overdraft_and_changes_nothing) {
  BudgetFixture f;
  EXPECT_TRUE(f.budget.reserve(f.idx, 1000));
  EXPECT_FALSE(f.budget.reserve(f.idx, 1));
  EXPECT_EQ(f.budget.remaining(f.idx), 0);
  EXPECT_EQ(f.budget.reserved(f.idx), 1000);
}

TEST(budget_zero_cost_win_needs_no_hold) {
  BudgetFixture f;
  EXPECT_TRUE(f.budget.reserve(f.idx, 0));
  EXPECT_EQ(f.budget.reserved(f.idx), 0);
}

TEST(budget_release_returns_money_and_cannot_go_negative) {
  BudgetFixture f;
  f.budget.reserve(f.idx, 300);
  f.budget.release(f.idx, 100);
  EXPECT_EQ(f.budget.reserved(f.idx), 200);
  f.budget.release(f.idx, 9999);
  EXPECT_EQ(f.budget.reserved(f.idx), 0);
  EXPECT_EQ(f.budget.remaining(f.idx), 1000);
}

TEST(budget_commit_ignores_unknown_campaigns) {
  BudgetFixture f;
  Event event;
  event.campaignId = 999;
  event.priceMicros = 500;
  f.budget.commit(event);
  EXPECT_EQ(f.budget.spentToday(f.idx), 0);
}

TEST(budget_total_limit_outlives_a_day_rollover) {
  BudgetFixture f(1000, 1500);
  f.spend(1000);
  EXPECT_EQ(f.budget.remaining(f.idx), 0);

  const Time tomorrow = fromUnixMs(toUnixMs(nowTime()) + kMsPerDay);
  f.budget.rollDay(tomorrow);
  EXPECT_EQ(f.budget.spentToday(f.idx), 0);
  EXPECT_EQ(f.budget.remaining(f.idx), 500);
}

TEST(budget_roll_day_is_a_no_op_within_the_same_day) {
  BudgetFixture f;
  f.spend(100);
  f.budget.rollDay(nowTime());
  EXPECT_EQ(f.budget.spentToday(f.idx), 100);
}

TEST(budget_abandoned_holds_expire_after_two_ticks) {
  BudgetFixture f;
  f.budget.reserve(f.idx, 100);
  f.budget.expireReservations();
  EXPECT_EQ(f.budget.reserved(f.idx), 100);
  f.budget.expireReservations();
  EXPECT_EQ(f.budget.reserved(f.idx), 0);
  EXPECT_EQ(f.budget.remaining(f.idx), 1000);
}

TEST(budget_expiry_spares_holds_younger_than_a_full_tick) {
  BudgetFixture f;
  f.budget.reserve(f.idx, 100);
  f.budget.expireReservations();
  f.budget.reserve(f.idx, 50);
  f.budget.expireReservations();
  EXPECT_EQ(f.budget.reserved(f.idx), 50);
  f.budget.expireReservations();
  EXPECT_EQ(f.budget.reserved(f.idx), 0);
}

TEST(budget_committed_hold_is_not_expired_a_second_time) {
  BudgetFixture f;
  f.budget.reserve(f.idx, 100);
  f.budget.expireReservations();
  f.spend(100, 100);
  f.budget.reserve(f.idx, 70);
  f.budget.expireReservations();
  EXPECT_EQ(f.budget.reserved(f.idx), 70);
}

TEST(budget_concurrent_winners_cannot_overspend) {
  constexpr i64 kLimit = 100'000;
  constexpr i64 kCost = 7;
  BudgetFixture f(kLimit, kLimit);
  std::atomic<i64> granted{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 20'000; ++i) {
        if (f.budget.reserve(f.idx, kCost)) granted.fetch_add(kCost);
      }
    });
  }
  for (std::thread& t : threads) t.join();
  EXPECT_LE(granted.load(), kLimit);
  EXPECT_EQ(f.budget.reserved(f.idx), granted.load());
  EXPECT_GT(granted.load(), kLimit - 8 * kCost);
}

TEST(budget_sync_publishes_spend_once) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  RedisClient client("127.0.0.1", server.port(), 500);
  BudgetFixture f(1000, 5000, &client);
  f.spend(300);
  f.budget.syncWithRedis();
  EXPECT_EQ(server.get(dayKey()), "300");
  EXPECT_EQ(server.get(totalKey()), "300");
  EXPECT_EQ(f.budget.spentToday(f.idx), 300);

  f.budget.syncWithRedis();
  EXPECT_EQ(server.get(dayKey()), "300");
  EXPECT_EQ(f.budget.spentToday(f.idx), 300);
}

TEST(budget_sync_recovers_spend_after_a_restart) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  RedisClient client("127.0.0.1", server.port(), 500);
  {
    BudgetFixture before(1000, 5000, &client);
    before.spend(700);
    before.budget.syncWithRedis();
  }
  BudgetFixture after(1000, 5000, &client);
  EXPECT_EQ(after.budget.spentToday(after.idx), 0);
  after.budget.syncWithRedis();
  EXPECT_EQ(after.budget.spentToday(after.idx), 700);
  EXPECT_EQ(after.budget.remaining(after.idx), 300);
}

TEST(budget_sync_recovers_lifetime_spend_too) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  RedisClient client("127.0.0.1", server.port(), 500);
  {
    BudgetFixture before(10'000, 1000, &client);
    before.spend(700);
    before.budget.syncWithRedis();
  }
  BudgetFixture after(10'000, 1000, &client);
  after.budget.syncWithRedis();
  EXPECT_EQ(after.budget.remaining(after.idx), 300);
}

TEST(budget_sync_shares_spend_between_instances) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  RedisClient client("127.0.0.1", server.port(), 500);
  BudgetFixture a(1000, 5000, &client);
  BudgetFixture b(1000, 5000, &client);
  a.spend(100);
  b.spend(50);
  a.budget.syncWithRedis();
  b.budget.syncWithRedis();
  a.budget.syncWithRedis();
  EXPECT_EQ(a.budget.spentToday(a.idx), 150);
  EXPECT_EQ(b.budget.spentToday(b.idx), 150);
}

TEST(budget_sync_keeps_unpushed_spend_during_an_outage) {
  const u16 port = unusedPort();
  RedisClient client("127.0.0.1", port, 100);
  BudgetFixture f(1000, 5000, &client);
  f.spend(250);
  f.budget.syncWithRedis();
  EXPECT_EQ(f.budget.spentToday(f.idx), 250);

  MiniRedis server;
  ASSERT_TRUE(server.start(port));
  f.spend(50);
  f.budget.syncWithRedis();
  EXPECT_EQ(server.get(totalKey()), "300");
  EXPECT_EQ(f.budget.spentToday(f.idx), 300);
}

TEST(budget_outage_across_midnight_does_not_charge_the_new_day) {
  const u16 port = unusedPort();
  RedisClient client("127.0.0.1", port, 100);
  BudgetFixture f(1000, 5000, &client);
  f.spend(700);
  const Time tomorrow = fromUnixMs(toUnixMs(nowTime()) + kMsPerDay);
  f.budget.rollDay(tomorrow);

  MiniRedis server;
  ASSERT_TRUE(server.start(port));
  f.budget.syncWithRedis();
  EXPECT_EQ(f.budget.spentToday(f.idx), 0);
  EXPECT_EQ(server.get(totalKey()), "700");
  EXPECT_EQ(f.budget.remaining(f.idx), 1000);
}

TEST(pacing_asap_is_never_throttled) {
  PacingFixture f("ASAP");
  f.pacing.consume(f.idx, 1'000'000);
  EXPECT_TRUE(f.pacing.allowed(f.idx, f.start));
}

TEST(pacing_even_blocks_until_refilled) {
  PacingFixture f;
  EXPECT_TRUE(f.pacing.allowed(f.idx, f.start));
  f.pacing.consume(f.idx, 2);
  EXPECT_NEAR(f.pacing.tokens(f.idx), -1.0, 1e-3);
  EXPECT_FALSE(f.pacing.allowed(f.idx, f.start));
  EXPECT_FALSE(f.pacing.allowed(f.idx, f.at(1000)));
  EXPECT_NEAR(f.pacing.tokens(f.idx), 0.0, 1e-3);
  EXPECT_TRUE(f.pacing.allowed(f.idx, f.at(2000)));
  EXPECT_NEAR(f.pacing.tokens(f.idx), 1.0, 1e-3);
}

TEST(pacing_bucket_is_capped_after_idle_time) {
  PacingFixture f;
  f.pacing.refill(f.at(10 * kMsPerHour));
  EXPECT_NEAR(f.pacing.tokens(f.idx), 60.0, 1e-3);
}

TEST(pacing_does_not_refill_more_often_than_its_minimum_interval) {
  PacingFixture f;
  f.pacing.refill(f.at(49));
  EXPECT_NEAR(f.pacing.tokens(f.idx), 1.0, 1e-3);
  f.pacing.refill(f.at(500));
  EXPECT_NEAR(f.pacing.tokens(f.idx), 1.5, 1e-3);
}

TEST(pacing_a_refund_restores_tokens) {
  PacingFixture f;
  f.pacing.consume(f.idx, 5);
  f.pacing.consume(f.idx, -5);
  EXPECT_NEAR(f.pacing.tokens(f.idx), 1.0, 1e-3);
}

TEST(pacing_adjust_rate_is_bounded) {
  PacingFixture f;
  f.pacing.adjustRate(f.idx, 1.f);
  EXPECT_NEAR(f.pacing.rateMultiplier(f.idx), 1.25, 1e-4);
  for (int i = 0; i < 100; ++i) f.pacing.adjustRate(f.idx, 50.f);
  EXPECT_NEAR(f.pacing.rateMultiplier(f.idx), 5.0, 1e-4);
  for (int i = 0; i < 100; ++i) f.pacing.adjustRate(f.idx, -50.f);
  EXPECT_NEAR(f.pacing.rateMultiplier(f.idx), 0.2, 1e-4);
}

TEST(pacing_rate_multiplier_speeds_up_refill) {
  PacingFixture f;
  f.pacing.adjustRate(f.idx, 1.f);
  f.pacing.refill(f.at(1000));
  EXPECT_NEAR(f.pacing.tokens(f.idx), 2.25, 1e-3);
}

TEST(pacing_catalog_refresh_keeps_earned_tokens) {
  PacingFixture f;
  f.pacing.consume(f.idx, 3);
  f.pacing.applyCatalog(*f.snapshot, f.at(100));
  EXPECT_NEAR(f.pacing.tokens(f.idx), -2.0, 1e-3);
}

TEST(pacing_with_no_daily_budget_never_serves) {
  PacingFixture f("EVEN", 0);
  EXPECT_FALSE(f.pacing.allowed(f.idx, f.at(100'000)));
}

TEST(redis_mget_keeps_positions_and_reports_missing_keys) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  server.set("a", "1");
  server.set("c", "3");
  RedisClient client("127.0.0.1", server.port(), 500);
  const auto values = client.mget({"a", "b", "c"});
  ASSERT_EQ(values.size(), 3);
  EXPECT_EQ(values[0].value(), "1");
  EXPECT_FALSE(values[1].has_value());
  EXPECT_EQ(values[2].value(), "3");
}

TEST(redis_incr_and_hget) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  server.hset("h", "f", "v");
  RedisClient client("127.0.0.1", server.port(), 500);
  EXPECT_EQ(client.incr("n", 60), 1);
  EXPECT_EQ(client.incr("n", 60), 2);
  EXPECT_EQ(client.hget("h", "f").value(), "v");
  EXPECT_EQ(client.hget("h", "missing").value(), "");
  EXPECT_EQ(client.hget("missing", "f").value(), "");
}

TEST(redis_values_can_contain_protocol_characters) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  const std::string tricky = "line1\r\n$5\r\n*2\r\n";
  server.set("k", tricky);
  RedisClient client("127.0.0.1", server.port(), 500);
  EXPECT_EQ(client.mget({"k"})[0].value(), tricky);
}

TEST(redis_pipeline_returns_one_reply_per_command) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  RedisClient client("127.0.0.1", server.port(), 500);
  const Replies replies = client.pipeline({{"INCRBY", "x", "5"}, {"PING"}, {"BOGUS"}, {"INCR", "x"}});
  ASSERT_EQ(replies.size(), 4);
  EXPECT_EQ(replies[0].integer, 5);
  EXPECT_EQ(replies[1].str, "PONG");
  EXPECT_TRUE(replies[2].type == RedisReply::Type::Error);
  EXPECT_EQ(replies[3].integer, 6);
}

TEST(redis_incr_reports_failure_on_a_non_integer) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  server.set("word", "hello");
  RedisClient client("127.0.0.1", server.port(), 500);
  EXPECT_EQ(client.incr("word", 60), -1);
}

TEST(redis_large_multi_key_replies_are_read_completely) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  std::vector<std::string> keys;
  const std::string value(1024, 'z');
  for (int i = 0; i < 200; ++i) {
    keys.push_back("k" + std::to_string(i));
    server.set(keys.back(), value);
  }
  RedisClient client("127.0.0.1", server.port(), 2000);
  const auto values = client.mget(keys);
  ASSERT_EQ(values.size(), 200);
  EXPECT_EQ(values[199].value(), value);
}

// A timed-out connection still has a reply in flight. If it went back into the
// pool, the next caller would read that stale reply as its own answer.
TEST(redis_timeout_does_not_poison_later_calls) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  server.set("first", "wrong");
  server.set("second", "right");
  RedisClient client("127.0.0.1", server.port(), 100);
  ASSERT_EQ(client.mget({"second"})[0].value(), "right");

  server.setReplyDelayMs(300);
  const auto started = std::chrono::steady_clock::now();
  EXPECT_TRUE(client.mget({"first"}).empty());
  EXPECT_LT(elapsedMs(started), 280);

  server.setReplyDelayMs(0);
  const auto values = client.mget({"second"});
  ASSERT_EQ(values.size(), 1);
  EXPECT_EQ(values[0].value(), "right");
}

TEST(redis_circuit_breaker_fails_fast_during_an_outage) {
  RedisClient client("127.0.0.1", unusedPort(), 50);
  for (int i = 0; i < 3; ++i) EXPECT_TRUE(client.mget({"a"}).empty());
  const auto started = std::chrono::steady_clock::now();
  for (int i = 0; i < 1000; ++i) EXPECT_EQ(client.incr("a", 60), -1);
  EXPECT_LT(elapsedMs(started), 200);
}

TEST(redis_recovers_when_the_server_returns) {
  const u16 port = unusedPort();
  RedisClient client("127.0.0.1", port, 50);
  for (int i = 0; i < 5; ++i) EXPECT_EQ(client.incr("a", 60), -1);

  MiniRedis server;
  ASSERT_TRUE(server.start(port));
  EXPECT_TRUE(waitUntil([&] { return client.incr("a", 60) > 0; }, 3000));
}

TEST(redis_reconnects_after_the_server_closes_pooled_sockets) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  const u16 port = server.port();
  RedisClient client("127.0.0.1", port, 200);
  ASSERT_EQ(client.incr("a", 60), 1);
  server.stop();

  MiniRedis replacement;
  ASSERT_TRUE(replacement.start(port));
  EXPECT_TRUE(waitUntil([&] { return client.incr("b", 60) > 0; }, 3000));
}

TEST(redis_concurrent_callers_receive_their_own_answers) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  for (int i = 0; i < 16; ++i) server.set("key" + std::to_string(i), "value" + std::to_string(i));
  RedisClient client("127.0.0.1", server.port(), 2000, 4);

  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 500; ++i) {
        const int n = (t + i) % 16;
        const auto values = client.mget({"key" + std::to_string(n)});
        if (values.size() != 1 || values[0].value_or("") != "value" + std::to_string(n)) wrong.fetch_add(1);
      }
    });
  }
  for (std::thread& t : threads) t.join();
  EXPECT_EQ(wrong.load(), 0);
}

TEST(mini_redis_survives_an_absurd_array_length) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  net::Socket attacker = net::Socket::connectTcp("127.0.0.1", server.port(), 500);
  ASSERT_TRUE(attacker.valid());
  ASSERT_TRUE(attacker.sendAll("*99999999999999\r\n", 500));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  RedisClient client("127.0.0.1", server.port(), 500);
  EXPECT_EQ(client.incr("still-alive", 60), 1);
}

TEST(mini_redis_reports_integer_overflow_like_redis) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  server.set("n", "9223372036854775807");
  RedisClient client("127.0.0.1", server.port(), 500);
  const Replies replies = client.pipeline({{"INCRBY", "n", "1"}});
  ASSERT_EQ(replies.size(), 1);
  EXPECT_TRUE(replies[0].type == RedisReply::Type::Error);
  EXPECT_EQ(server.get("n"), "9223372036854775807");
}

TEST(frequency_counts_per_user_and_campaign) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  RedisClient client("127.0.0.1", server.port(), 500);
  FrequencyCapStore store(&client, 1024, 3600);
  EXPECT_EQ(store.count(1, 100), 0);
  store.increment(1, 100);
  store.increment(1, 100);
  EXPECT_EQ(store.count(1, 100), 2);
  EXPECT_EQ(store.count(2, 100), 0);
  EXPECT_EQ(store.count(1, 200), 0);
}

TEST(frequency_batch_preserves_order) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  RedisClient client("127.0.0.1", server.port(), 500);
  FrequencyCapStore store(&client, 1024, 3600);
  store.increment(1, 20);
  store.increment(1, 30);
  store.increment(1, 30);
  const std::vector<u64> campaigns = {30, 10, 20};
  const std::vector<u16> counts = store.countBatch(1, campaigns);
  ASSERT_EQ(counts.size(), 3);
  EXPECT_EQ(counts[0], 2);
  EXPECT_EQ(counts[1], 0);
  EXPECT_EQ(counts[2], 1);
}

TEST(frequency_counts_are_shared_through_redis) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  RedisClient client("127.0.0.1", server.port(), 500);
  FrequencyCapStore writer(&client, 1024, 3600);
  writer.increment(1, 100);
  writer.increment(1, 100);
  FrequencyCapStore reader(&client, 1024, 3600);
  EXPECT_EQ(reader.count(1, 100), 2);
}

TEST(frequency_local_cache_refreshes_after_its_freshness_window) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  RedisClient client("127.0.0.1", server.port(), 500);
  FrequencyCapStore writer(&client, 1024, 3600);
  FrequencyCapStore reader(&client, 1024, 3600, std::chrono::milliseconds(250));
  writer.increment(1, 100);
  EXPECT_EQ(reader.count(1, 100), 1);
  writer.increment(1, 100);
  EXPECT_EQ(reader.count(1, 100), 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(reader.count(1, 100), 2);
}

TEST(frequency_falls_back_to_local_counts_during_an_outage) {
  RedisClient client("127.0.0.1", unusedPort(), 50);
  FrequencyCapStore store(&client, 1024, 3600);
  store.increment(1, 100);
  store.increment(1, 100);
  EXPECT_EQ(store.count(1, 100), 2);
  EXPECT_EQ(store.count(1, 200), 0);
}

TEST(frequency_works_without_redis) {
  FrequencyCapStore store(nullptr, 1024, 3600);
  store.increment(1, 100);
  EXPECT_EQ(store.count(1, 100), 1);
}

TEST(frequency_ignores_a_corrupt_counter) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  server.set("fc:" + std::to_string(utcDay(nowTime())) + ":1:100", "garbage");
  RedisClient client("127.0.0.1", server.port(), 500);
  FrequencyCapStore store(&client, 1024, 3600);
  EXPECT_EQ(store.count(1, 100), 0);
}

TEST(frequency_clamps_huge_counters) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  server.set("fc:" + std::to_string(utcDay(nowTime())) + ":1:100", "4000000000");
  RedisClient client("127.0.0.1", server.port(), 500);
  FrequencyCapStore store(&client, 1024, 3600);
  EXPECT_EQ(store.count(1, 100), 65535);
}

TEST(profile_parses_segments_and_skips_garbage) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  server.hset("user:42", "segments", "10,20,abc,,30x,40");
  RedisClient client("127.0.0.1", server.port(), 500);
  UserProfileStore store(&client, 1024);
  const std::vector<SegId> expected = {10, 20, 40};
  EXPECT_TRUE(store.segments(42) == expected);
}

TEST(profile_for_an_unknown_user_is_empty) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  RedisClient client("127.0.0.1", server.port(), 500);
  UserProfileStore store(&client, 1024);
  EXPECT_TRUE(store.segments(1).empty());
}

TEST(profile_is_cached) {
  MiniRedis server;
  ASSERT_TRUE(server.start());
  server.hset("user:1", "segments", "5");
  RedisClient client("127.0.0.1", server.port(), 500);
  UserProfileStore store(&client, 1024);
  EXPECT_EQ(store.segments(1).size(), 1);
  server.hset("user:1", "segments", "5,6");
  EXPECT_EQ(store.segments(1).size(), 1);
}

TEST(profile_outage_is_not_remembered_as_no_segments) {
  const u16 port = unusedPort();
  RedisClient client("127.0.0.1", port, 100);
  UserProfileStore store(&client, 1024);
  EXPECT_TRUE(store.segments(1).empty());

  MiniRedis server;
  ASSERT_TRUE(server.start(port));
  server.hset("user:1", "segments", "7");
  EXPECT_EQ(store.segments(1).size(), 1);
}

TEST(profile_works_without_redis) {
  UserProfileStore store(nullptr, 1024);
  EXPECT_TRUE(store.segments(1).empty());
}
