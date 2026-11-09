#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catalog/catalog.h"
#include "common/url.h"
#include "events/events.h"
#include "state/runtime_state.h"
#include "test_framework.h"
#include "test_helpers.h"

using namespace ad;
using namespace adtest;

namespace {

constexpr u64 kCampaignId = 101;

class FakeTransport : public KafkaTransport {
 public:
  explicit FakeTransport(int failures = 0) : failures_(failures) {}

  Status send(std::string_view topic, std::string_view key, std::string_view value) override {
    std::lock_guard lock(mutex_);
    ++attempts_;
    if (failures_ > 0) {
      --failures_;
      return Status::error("broker unavailable");
    }
    topic_ = std::string(topic);
    key_ = std::string(key);
    values_.emplace_back(value);
    return Status::ok();
  }

  std::size_t delivered() {
    std::lock_guard lock(mutex_);
    return values_.size();
  }
  int attempts() {
    std::lock_guard lock(mutex_);
    return attempts_;
  }
  std::vector<std::string> values() {
    std::lock_guard lock(mutex_);
    return values_;
  }
  std::string topic() {
    std::lock_guard lock(mutex_);
    return topic_;
  }
  std::string key() {
    std::lock_guard lock(mutex_);
    return key_;
  }

 private:
  std::mutex mutex_;
  int failures_;
  int attempts_ = 0;
  std::string topic_;
  std::string key_;
  std::vector<std::string> values_;
};

Event sampleEvent(u64 requestId = 1) {
  Event event;
  event.type = EventType::Click;
  event.requestId = requestId;
  event.campaignId = kCampaignId;
  event.userId = 42;
  event.priceMicros = 1234;
  event.ts = fromUnixMs(kRequestTimeMs);
  event.slotId = 7;
  event.device = 2;
  return event;
}

struct HandlerFixture {
  CampaignSlots slots{8};
  BudgetManager budget{slots, nullptr, 5};
  FrequencyCapStore freqCap{nullptr, 1024, 3600};
  UrlSigner signer{"secret"};
  MetricsRegistry metrics;
  EventQueue queue;
  EventHandler handler;
  std::shared_ptr<const CatalogSnapshot> snapshot;
  u32 idx = 0;

  explicit HandlerFixture(std::size_t queueCapacity = 64)
      : queue(queueCapacity), handler(queue, budget, freqCap, signer, metrics, std::chrono::hours(1)) {
    CampaignSpec spec;
    spec.id = kCampaignId;
    snapshot = buildSnapshot(slots, simpleCatalog({spec}));
    budget.applyCatalog(*snapshot);
    idx = slots.find(kCampaignId).value();
  }

  std::string query(u64 rid, i64 price, i64 reserved, i64 expiry) const {
    return "rid=" + std::to_string(rid) + "&cid=" + std::to_string(kCampaignId) + "&uid=42&slot=7&dev=2&price=" +
           std::to_string(price) + "&rsv=" + std::to_string(reserved) + "&exp=" + std::to_string(expiry);
  }

  static i64 future() { return toUnixMs(nowTime()) + 60'000; }

  static HttpRequest fromTarget(std::string target, std::string method = "GET") {
    HttpRequest request;
    request.method = std::move(method);
    request.target = std::move(target);
    const std::size_t question = request.target.find('?');
    request.path = request.target.substr(0, question);
    if (question != std::string::npos) {
      request.rawQuery = request.target.substr(question + 1);
      request.query = parseQuery(request.rawQuery);
    }
    return request;
  }

  HttpRequest impression(u64 rid = 1, i64 price = 500, i64 reserved = 500) const {
    return fromTarget(signer.sign("/impression", query(rid, price, reserved, future())));
  }

  HttpRequest click(u64 rid = 1, i64 price = 900) const {
    return fromTarget(
        signer.sign("/click", query(rid, price, 0, future()) + "&to=" + percentEncode("https://shop.example/a?b=1&c=2")));
  }

  std::string locationOf(const HttpResponse& response) const {
    for (const auto& [name, value] : response.headers) {
      if (name == "Location") return value;
    }
    return {};
  }
};

}

TEST(event_queue_pops_in_order_up_to_the_limit) {
  EventQueue queue(16);
  for (u64 i = 1; i <= 5; ++i) EXPECT_TRUE(queue.push(sampleEvent(i)));
  const std::vector<Event> first = queue.popBatch(3);
  ASSERT_EQ(first.size(), 3);
  EXPECT_EQ(first[0].requestId, 1);
  EXPECT_EQ(first[2].requestId, 3);
  EXPECT_EQ(queue.popBatch(10).size(), 2);
  EXPECT_TRUE(queue.popBatch(10).empty());
}

TEST(event_queue_refuses_events_when_full) {
  EventQueue queue(2);
  EXPECT_TRUE(queue.push(sampleEvent(1)));
  EXPECT_TRUE(queue.push(sampleEvent(2)));
  EXPECT_FALSE(queue.push(sampleEvent(3)));
}

TEST(dedup_remembers_a_request_and_type) {
  DedupCache cache(std::chrono::seconds(10));
  const Time t = fromUnixMs(kRequestTimeMs);
  EXPECT_TRUE(cache.insertIfAbsent(1, EventType::Impression, t));
  EXPECT_FALSE(cache.insertIfAbsent(1, EventType::Impression, t));
  EXPECT_TRUE(cache.insertIfAbsent(1, EventType::Click, t));
  EXPECT_TRUE(cache.insertIfAbsent(2, EventType::Impression, t));
}

TEST(dedup_never_forgets_before_the_ttl) {
  DedupCache cache(std::chrono::milliseconds(1000));
  const i64 start = kRequestTimeMs;
  EXPECT_TRUE(cache.insertIfAbsent(1, EventType::Impression, fromUnixMs(start)));
  for (i64 offset = 100; offset < 1000; offset += 100) {
    EXPECT_FALSE(cache.insertIfAbsent(1, EventType::Impression, fromUnixMs(start + offset)));
  }
}

TEST(dedup_entry_that_lands_just_before_a_rotation_survives_it) {
  DedupCache cache(std::chrono::milliseconds(1000));
  const i64 start = kRequestTimeMs;
  EXPECT_TRUE(cache.insertIfAbsent(1, EventType::Impression, fromUnixMs(start)));
  EXPECT_TRUE(cache.insertIfAbsent(9, EventType::Impression, fromUnixMs(start + 999)));
  EXPECT_FALSE(cache.insertIfAbsent(9, EventType::Impression, fromUnixMs(start + 1000)));
  EXPECT_FALSE(cache.insertIfAbsent(9, EventType::Impression, fromUnixMs(start + 1998)));
}

TEST(dedup_memory_is_reclaimed_eventually) {
  DedupCache cache(std::chrono::milliseconds(1000));
  const i64 start = kRequestTimeMs;
  EXPECT_TRUE(cache.insertIfAbsent(1, EventType::Impression, fromUnixMs(start)));
  EXPECT_FALSE(cache.insertIfAbsent(1, EventType::Impression, fromUnixMs(start + 1000)));
  EXPECT_TRUE(cache.insertIfAbsent(1, EventType::Impression, fromUnixMs(start + 2000)));
}

TEST(dedup_admits_each_id_once_across_threads) {
  DedupCache cache(std::chrono::minutes(1));
  const Time t = nowTime();
  std::atomic<int> admitted{0};
  std::vector<std::thread> threads;
  for (int n = 0; n < 8; ++n) {
    threads.emplace_back([&] {
      for (u64 id = 0; id < 10'000; ++id) {
        if (cache.insertIfAbsent(id, EventType::Impression, t)) admitted.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  EXPECT_EQ(admitted.load(), 10'000);
}

TEST(kafka_encoding_round_trips) {
  const Event original = sampleEvent(987654321012345ULL);
  const std::optional<Event> decoded = KafkaProducer::decode(KafkaProducer::encode(original));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->type == EventType::Click);
  EXPECT_EQ(decoded->requestId, original.requestId);
  EXPECT_EQ(decoded->campaignId, original.campaignId);
  EXPECT_EQ(decoded->userId, original.userId);
  EXPECT_EQ(decoded->priceMicros, original.priceMicros);
  EXPECT_EQ(toUnixMs(decoded->ts), toUnixMs(original.ts));
  EXPECT_EQ(decoded->slotId, original.slotId);
  EXPECT_EQ(decoded->device, original.device);
}

TEST(kafka_decode_rejects_bad_messages) {
  EXPECT_FALSE(KafkaProducer::decode("not json").has_value());
  EXPECT_FALSE(KafkaProducer::decode("{}").has_value());
  EXPECT_FALSE(KafkaProducer::decode(R"({"type":"view","requestId":1})").has_value());
}

TEST(kafka_producer_keys_messages_by_campaign) {
  FakeTransport transport;
  KafkaProducer producer("ad-events", transport);
  const std::vector<Event> batch = {sampleEvent(1), sampleEvent(2)};
  EXPECT_TRUE(producer.produce(batch).isOk());
  EXPECT_EQ(transport.delivered(), 2);
  EXPECT_EQ(transport.topic(), "ad-events");
  EXPECT_EQ(transport.key(), std::to_string(kCampaignId));
}

TEST(kafka_producer_reports_transport_errors) {
  FakeTransport transport(1);
  KafkaProducer producer("t", transport);
  const std::vector<Event> batch = {sampleEvent()};
  const Status status = producer.produce(batch);
  EXPECT_FALSE(status.isOk());
  EXPECT_EQ(status.message(), "broker unavailable");
}

TEST(in_process_kafka_delivers_in_order_and_drains_on_stop) {
  InProcessKafka kafka;
  std::vector<std::string> received;
  kafka.subscribe([&](std::vector<std::string> batch) {
    for (std::string& message : batch) received.push_back(std::move(message));
  });
  kafka.start();
  for (int i = 0; i < 2000; ++i) EXPECT_TRUE(kafka.send("t", "k", std::to_string(i)).isOk());
  kafka.stop();
  ASSERT_EQ(received.size(), 2000);
  for (int i = 0; i < 2000; ++i) ASSERT_EQ(received[static_cast<std::size_t>(i)], std::to_string(i));
}

TEST(in_process_kafka_applies_backpressure) {
  InProcessKafka kafka(2);
  EXPECT_TRUE(kafka.send("t", "k", "a").isOk());
  EXPECT_TRUE(kafka.send("t", "k", "b").isOk());
  EXPECT_FALSE(kafka.send("t", "k", "c").isOk());
}

TEST(kafka_producer_reports_how_far_it_got) {
  FakeTransport transport(0);
  KafkaProducer producer("t", transport);
  const std::vector<Event> batch = {sampleEvent(1), sampleEvent(2)};
  std::size_t sent = 99;
  EXPECT_TRUE(producer.produce(batch, &sent).isOk());
  EXPECT_EQ(sent, 2);
}

TEST(event_writer_flushes_when_a_batch_is_full) {
  EventQueue queue(64);
  FakeTransport transport;
  EventWriter writer(queue, KafkaProducer("t", transport), 3, 3'600'000);
  writer.start();
  for (u64 i = 1; i <= 3; ++i) queue.push(sampleEvent(i));
  EXPECT_TRUE(waitUntil([&] { return transport.delivered() == 3; }));
}

TEST(event_writer_flushes_a_partial_batch_after_the_interval) {
  EventQueue queue(64);
  FakeTransport transport;
  EventWriter writer(queue, KafkaProducer("t", transport), 1000, 10);
  writer.start();
  queue.push(sampleEvent());
  EXPECT_TRUE(waitUntil([&] { return transport.delivered() == 1; }));
}

TEST(event_writer_drains_the_queue_on_stop) {
  EventQueue queue(4096);
  FakeTransport transport;
  EventWriter writer(queue, KafkaProducer("t", transport), 50, 3'600'000);
  for (u64 i = 1; i <= 1234; ++i) queue.push(sampleEvent(i));
  writer.start();
  writer.stop();
  EXPECT_EQ(transport.delivered(), 1234);
  const std::vector<std::string> values = transport.values();
  EXPECT_EQ(KafkaProducer::decode(values.front())->requestId, 1);
  EXPECT_EQ(KafkaProducer::decode(values.back())->requestId, 1234);
}

TEST(event_writer_retries_a_failed_batch) {
  EventQueue queue(64);
  FakeTransport transport(2);
  MetricsRegistry metrics;
  EventWriter writer(queue, KafkaProducer("t", transport), 1, 10, &metrics);
  writer.start();
  queue.push(sampleEvent());
  EXPECT_TRUE(waitUntil([&] { return transport.delivered() == 1; }));
  EXPECT_EQ(metrics.counterValue("ad_events_dropped_total", "stage=\"produce\""), 0);
}

class FailsOnce : public KafkaTransport {
 public:
  explicit FailsOnce(int failAt) : failAt_(failAt) {}
  Status send(std::string_view, std::string_view, std::string_view value) override {
    std::lock_guard lock(mutex_);
    if (++count_ == failAt_) return Status::error("broker hiccup");
    values_.emplace_back(value);
    return Status::ok();
  }
  std::vector<std::string> values() {
    std::lock_guard lock(mutex_);
    return values_;
  }

 private:
  std::mutex mutex_;
  int failAt_;
  int count_ = 0;
  std::vector<std::string> values_;
};

TEST(event_writer_retry_does_not_duplicate_what_already_went_out) {
  EventQueue queue(64);
  FailsOnce transport(2);
  EventWriter writer(queue, KafkaProducer("t", transport), 3, 3'600'000);
  for (u64 i = 1; i <= 3; ++i) queue.push(sampleEvent(i));
  writer.start();
  EXPECT_TRUE(waitUntil([&] { return transport.values().size() >= 3; }));
  writer.stop();
  const std::vector<std::string> values = transport.values();
  ASSERT_EQ(values.size(), 3);
  for (u64 i = 1; i <= 3; ++i) EXPECT_EQ(KafkaProducer::decode(values[i - 1])->requestId, i);
}

TEST(event_writer_counts_batches_it_has_to_drop) {
  EventQueue queue(64);
  FakeTransport transport(1000);
  MetricsRegistry metrics;
  EventWriter writer(queue, KafkaProducer("t", transport), 1, 10, &metrics);
  writer.start();
  queue.push(sampleEvent());
  EXPECT_TRUE(waitUntil([&] { return metrics.counterValue("ad_events_dropped_total", "stage=\"produce\"") == 1; }));
  EXPECT_EQ(transport.attempts(), 3);
}

TEST(stats_updater_feeds_the_ctr_table) {
  CtrTable table(0.f, 1.f);
  StatsUpdater stats(table);
  Event impression = sampleEvent();
  impression.type = EventType::Impression;
  std::vector<Event> events(99, impression);
  events.push_back(sampleEvent());
  stats.consume(events);
  const CtrTable::Stat stat = table.stat(kCampaignId, 7, 2);
  EXPECT_NEAR(stat.impressions, 99.0, 1e-6);
  EXPECT_NEAR(stat.clicks, 1.0, 1e-6);
  EXPECT_NEAR(table.lookup(kCampaignId, 7, 2), 0.01, 1e-6);
}

TEST(stats_updater_nightly_recompute_ages_old_data) {
  CtrTable table(1.f, 1.f);
  StatsUpdater stats(table);
  Event impression = sampleEvent();
  impression.type = EventType::Impression;
  const std::vector<Event> events(10, impression);
  stats.consume(events);
  stats.recomputeNightly();
  EXPECT_NEAR(table.stat(kCampaignId, 7, 2).impressions, 5.0, 1e-6);
}

TEST(handler_impression_commits_spend_and_counts_the_user) {
  HandlerFixture f;
  ASSERT_TRUE(f.budget.reserve(f.idx, 500));
  const HttpResponse response = f.handler.handleImpression(f.impression());
  EXPECT_EQ(response.status, 204);
  EXPECT_EQ(f.budget.spentToday(f.idx), 500);
  EXPECT_EQ(f.budget.reserved(f.idx), 0);
  EXPECT_EQ(f.freqCap.count(42, kCampaignId), 1);
  const std::vector<Event> queued = f.queue.popBatch(10);
  ASSERT_EQ(queued.size(), 1);
  EXPECT_TRUE(queued[0].type == EventType::Impression);
  EXPECT_EQ(queued[0].slotId, 7);
  EXPECT_EQ(queued[0].device, 2);
  EXPECT_EQ(f.metrics.counterValue("ad_events_total", "type=\"impression\""), 1);
}

TEST(handler_duplicate_impression_is_not_charged_twice) {
  HandlerFixture f;
  const HttpRequest request = f.impression();
  EXPECT_EQ(f.handler.handleImpression(request).status, 204);
  EXPECT_EQ(f.handler.handleImpression(request).status, 204);
  EXPECT_EQ(f.budget.spentToday(f.idx), 500);
  EXPECT_EQ(f.freqCap.count(42, kCampaignId), 1);
  EXPECT_EQ(f.queue.popBatch(10).size(), 1);
  EXPECT_EQ(f.metrics.counterValue("ad_events_rejected_total", "reason=\"duplicate\""), 1);
}

TEST(handler_treats_a_reused_request_id_for_another_user_as_a_new_event) {
  HandlerFixture f;
  const std::string exp = std::to_string(HandlerFixture::future());
  const std::string prefix = "rid=7&cid=" + std::to_string(kCampaignId);
  const HttpRequest a = HandlerFixture::fromTarget(
      f.signer.sign("/impression", prefix + "&uid=10&slot=7&dev=2&price=500&rsv=0&exp=" + exp));
  const HttpRequest b = HandlerFixture::fromTarget(
      f.signer.sign("/impression", prefix + "&uid=20&slot=7&dev=2&price=500&rsv=0&exp=" + exp));
  EXPECT_EQ(f.handler.handleImpression(a).status, 204);
  EXPECT_EQ(f.handler.handleImpression(b).status, 204);
  EXPECT_EQ(f.budget.spentToday(f.idx), 1000);
  EXPECT_EQ(f.handler.handleImpression(a).status, 204);
  EXPECT_EQ(f.budget.spentToday(f.idx), 1000);
}

TEST(handler_rejects_a_forged_or_altered_link) {
  HandlerFixture f;
  HttpRequest request = f.impression();
  const std::size_t pos = request.target.find("price=500");
  request.target.replace(pos, 9, "price=999");
  request.query = parseQuery(request.target.substr(request.target.find('?') + 1));
  EXPECT_EQ(f.handler.handleImpression(request).status, 403);
  EXPECT_EQ(f.budget.spentToday(f.idx), 0);
  EXPECT_EQ(f.queue.popBatch(10).size(), 0);
  EXPECT_EQ(f.metrics.counterValue("ad_events_rejected_total", "reason=\"bad_signature\""), 1);
}

TEST(handler_rejects_a_link_signed_with_another_key) {
  HandlerFixture f;
  const UrlSigner other("attacker");
  const HttpRequest request = HandlerFixture::fromTarget(other.sign("/impression", f.query(1, 500, 500, f.future())));
  EXPECT_EQ(f.handler.handleImpression(request).status, 403);
}

TEST(handler_rejects_an_expired_link) {
  HandlerFixture f;
  const HttpRequest request =
      HandlerFixture::fromTarget(f.signer.sign("/impression", f.query(1, 500, 500, toUnixMs(nowTime()) - 1)));
  EXPECT_EQ(f.handler.handleImpression(request).status, 410);
  EXPECT_EQ(f.budget.spentToday(f.idx), 0);
  EXPECT_EQ(f.metrics.counterValue("ad_events_rejected_total", "reason=\"expired\""), 1);
}

TEST(handler_rejects_signed_links_with_missing_or_bad_fields) {
  HandlerFixture f;
  const HttpRequest missing = HandlerFixture::fromTarget(f.signer.sign("/impression", "rid=1&cid=2"));
  EXPECT_EQ(f.handler.handleImpression(missing).status, 400);
  const HttpRequest bad = HandlerFixture::fromTarget(
      f.signer.sign("/impression", "rid=abc&cid=2&uid=3&slot=4&dev=5&price=6&rsv=7&exp=8"));
  EXPECT_EQ(f.handler.handleImpression(bad).status, 400);
}

TEST(handler_only_accepts_get) {
  HandlerFixture f;
  for (const char* method : {"POST", "HEAD", "PUT"}) {
    HttpRequest request = f.impression();
    request.method = method;
    EXPECT_EQ(f.handler.handleImpression(request).status, 405);
  }
  EXPECT_EQ(f.budget.spentToday(f.idx), 0);
}

TEST(handler_click_charges_and_redirects) {
  HandlerFixture f;
  const HttpResponse response = f.handler.handleClick(f.click());
  EXPECT_EQ(response.status, 302);
  EXPECT_EQ(f.locationOf(response), "https://shop.example/a?b=1&c=2");
  EXPECT_EQ(f.budget.spentToday(f.idx), 900);
  EXPECT_EQ(f.freqCap.count(42, kCampaignId), 0);
  EXPECT_EQ(f.queue.popBatch(10).size(), 1);
}

TEST(handler_repeat_click_still_lands_but_is_not_charged) {
  HandlerFixture f;
  const HttpRequest request = f.click();
  EXPECT_EQ(f.handler.handleClick(request).status, 302);
  const HttpResponse again = f.handler.handleClick(request);
  EXPECT_EQ(again.status, 302);
  EXPECT_EQ(f.locationOf(again), "https://shop.example/a?b=1&c=2");
  EXPECT_EQ(f.budget.spentToday(f.idx), 900);
}

TEST(handler_impression_and_click_of_one_request_both_count) {
  HandlerFixture f;
  EXPECT_EQ(f.handler.handleImpression(f.impression(5, 500, 0)).status, 204);
  EXPECT_EQ(f.handler.handleClick(f.click(5, 900)).status, 302);
  EXPECT_EQ(f.budget.spentToday(f.idx), 1400);
}

TEST(handler_click_link_cannot_be_replayed_as_an_impression) {
  HandlerFixture f;
  HttpRequest request = f.click();
  const std::size_t pos = request.target.find("/click");
  request.target.replace(pos, 6, "/impression");
  EXPECT_EQ(f.handler.handleImpression(request).status, 403);
}

TEST(handler_click_needs_a_destination) {
  HandlerFixture f;
  const HttpRequest request = HandlerFixture::fromTarget(f.signer.sign("/click", f.query(1, 1, 0, f.future())));
  EXPECT_EQ(f.handler.handleClick(request).status, 400);
}

TEST(handler_keeps_the_charge_when_the_stats_queue_is_full) {
  HandlerFixture f(2);
  EXPECT_EQ(f.handler.handleImpression(f.impression(1)).status, 204);
  EXPECT_EQ(f.handler.handleImpression(f.impression(2)).status, 204);
  EXPECT_EQ(f.handler.handleImpression(f.impression(3)).status, 204);
  EXPECT_EQ(f.budget.spentToday(f.idx), 1500);
  EXPECT_EQ(f.metrics.counterValue("ad_events_dropped_total", "stage=\"queue\""), 1);
}

TEST(handler_charges_once_when_the_same_link_races) {
  HandlerFixture f;
  const HttpRequest request = f.impression();
  std::vector<std::thread> threads;
  for (int n = 0; n < 8; ++n) {
    threads.emplace_back([&] {
      for (int i = 0; i < 200; ++i) f.handler.handleImpression(request);
    });
  }
  for (std::thread& thread : threads) thread.join();
  EXPECT_EQ(f.budget.spentToday(f.idx), 500);
}
