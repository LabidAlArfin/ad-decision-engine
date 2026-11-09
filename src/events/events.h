#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "common/containers.h"
#include "common/crypto.h"
#include "domain/model.h"
#include "observability/observability.h"
#include "scoring/scoring.h"
#include "server/http_server.h"
#include "state/runtime_state.h"

namespace ad {

class EventQueue {
 public:
  explicit EventQueue(std::size_t capacity) : ring_(capacity) {}
  bool push(const Event& event) { return ring_.tryPush(event); }
  std::vector<Event> popBatch(std::size_t max);

 private:
  MpmcRingBuffer<Event> ring_;
};

// Remembers (requestId, event type) pairs for at least `ttl`. An impression and
// its click share one request id, so the type has to be part of the key.
class DedupCache {
 public:
  explicit DedupCache(std::chrono::milliseconds ttl) : ttlMs_(ttl.count()) {}

  bool insertIfAbsent(u64 requestId, EventType type, Time now);

 private:
  struct Key {
    u64 requestId;
    EventType type;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    std::size_t operator()(const Key& k) const {
      return static_cast<std::size_t>(combineHash(k.requestId, static_cast<u64>(k.type)));
    }
  };
  static constexpr std::size_t kShards = 64;
  // Two generations: rotating drops only entries that are at least ttl old.
  struct Shard {
    std::mutex mutex;
    std::unordered_set<Key, KeyHash> current;
    std::unordered_set<Key, KeyHash> previous;
    i64 rotatedAtMs = 0;
  };
  std::array<Shard, kShards> shards_;
  i64 ttlMs_;
};

// The boundary to a message broker. The diagram's rd_kafka_t handle goes behind
// this interface; InProcessKafka is the implementation used for now.
class KafkaTransport {
 public:
  virtual ~KafkaTransport() = default;
  virtual Status send(std::string_view topic, std::string_view key, std::string_view value) = 0;
};

// Delivers messages to one consumer on a background thread, in order.
class InProcessKafka : public KafkaTransport {
 public:
  using Consumer = std::function<void(std::vector<std::string>)>;

  explicit InProcessKafka(std::size_t maxQueued = 1 << 20) : maxQueued_(maxQueued) {}
  ~InProcessKafka() override { stop(); }

  void subscribe(Consumer consumer) { consumer_ = std::move(consumer); }
  Status send(std::string_view topic, std::string_view key, std::string_view value) override;
  void start();
  void stop();  // delivers everything already queued before returning

 private:
  std::size_t maxQueued_;
  Consumer consumer_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;
  bool running_ = false;
  std::thread thread_;
};

class KafkaProducer {
 public:
  KafkaProducer(std::string topic, KafkaTransport& transport)
      : topic_(std::move(topic)), transport_(&transport) {}

  // `sent`, if given, receives how many events went out before any failure, so
  // a retry can resume there without duplicating the ones already delivered.
  Status produce(std::span<const Event> batch, std::size_t* sent = nullptr);

  static std::string encode(const Event& event);
  static std::optional<Event> decode(std::string_view payload);

 private:
  std::string topic_;
  KafkaTransport* transport_;
};

class EventWriter {
 public:
  EventWriter(EventQueue& queue, KafkaProducer kafka, int batchSize, int flushMs,
              MetricsRegistry* metrics = nullptr)
      : queue_(queue), kafka_(std::move(kafka)), batchSize_(batchSize), flushMs_(flushMs), metrics_(metrics) {}
  ~EventWriter() { stop(); }

  void start();
  void stop();  // drains the queue before returning
  void runLoop();

 private:
  void flush(std::vector<Event>& batch);

  EventQueue& queue_;
  KafkaProducer kafka_;
  int batchSize_;
  int flushMs_;
  MetricsRegistry* metrics_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

class StatsUpdater {
 public:
  explicit StatsUpdater(CtrTable& ctr) : ctr_(ctr) {}
  void consume(std::span<const Event> events);
  void recomputeNightly();

 private:
  CtrTable& ctr_;
};

class EventHandler {
 public:
  EventHandler(EventQueue& queue, BudgetManager& budget, FrequencyCapStore& freqCap,
               const UrlSigner& signer, MetricsRegistry& metrics, std::chrono::milliseconds dedupTtl)
      : queue_(queue),
        budget_(budget),
        freqCap_(freqCap),
        signer_(signer),
        metrics_(metrics),
        seenIds_(dedupTtl) {}

  HttpResponse handleImpression(const HttpRequest& http);
  HttpResponse handleClick(const HttpRequest& http);

 private:
  HttpResponse handle(const HttpRequest& http, EventType type);
  bool verifySignature(const std::string& target) const { return signer_.verify(target); }
  bool isDuplicate(u64 requestId, EventType type, Time now);

  EventQueue& queue_;
  BudgetManager& budget_;
  FrequencyCapStore& freqCap_;
  const UrlSigner& signer_;
  MetricsRegistry& metrics_;
  DedupCache seenIds_;
};

}
