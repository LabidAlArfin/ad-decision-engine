#include "events/events.h"

#include <charconv>
#include <iostream>

#include "common/json.h"

namespace ad {
namespace {

constexpr int kProduceAttempts = 3;

template <class T>
std::optional<T> parseNumber(const std::unordered_map<std::string, std::string>& query, const char* name) {
  auto it = query.find(name);
  if (it == query.end()) return std::nullopt;
  T value{};
  const std::string& text = it->second;
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}

HttpResponse successResponse(EventType type, const std::string& landingUrl) {
  HttpResponse response;
  if (type == EventType::Click) {
    response.status = 302;
    response.contentType = "text/plain; charset=utf-8";
    response.headers.emplace_back("Location", landingUrl);
  } else {
    response.status = 204;
  }
  return response;
}

}

std::vector<Event> EventQueue::popBatch(std::size_t max) {
  std::vector<Event> out;
  out.reserve(std::min<std::size_t>(max, 1024));
  Event event;
  while (out.size() < max && ring_.tryPop(event)) out.push_back(event);
  return out;
}

bool DedupCache::insertIfAbsent(u64 requestId, EventType type, Time now) {
  const Key key{requestId, type};
  Shard& shard = shards_[KeyHash{}(key) % kShards];
  const i64 nowMs = toUnixMs(now);

  std::lock_guard lock(shard.mutex);
  if (shard.rotatedAtMs == 0) shard.rotatedAtMs = nowMs;
  if (nowMs - shard.rotatedAtMs >= ttlMs_) {
    shard.previous = std::move(shard.current);
    shard.current.clear();
    shard.rotatedAtMs = nowMs;
  }
  if (shard.previous.count(key) != 0) return false;
  return shard.current.insert(key).second;
}

Status InProcessKafka::send(std::string_view, std::string_view, std::string_view value) {
  {
    std::lock_guard lock(mutex_);
    if (queue_.size() >= maxQueued_) return Status::error("in-process kafka: queue full");
    queue_.emplace_back(value);
  }
  cv_.notify_one();
  return Status::ok();
}

void InProcessKafka::start() {
  {
    std::lock_guard lock(mutex_);
    running_ = true;
  }
  thread_ = std::thread([this] {
    for (;;) {
      std::vector<std::string> batch;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return !running_ || !queue_.empty(); });
        if (!running_ && queue_.empty()) return;
        batch.assign(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
        queue_.clear();
      }
      if (consumer_) consumer_(std::move(batch));
    }
  });
}

void InProcessKafka::stop() {
  {
    std::lock_guard lock(mutex_);
    running_ = false;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

std::string KafkaProducer::encode(const Event& event) {
  Json out;
  out.set("type", event.type == EventType::Impression ? "impression" : "click");
  out.set("requestId", event.requestId);
  out.set("campaignId", event.campaignId);
  out.set("userId", event.userId);
  out.set("priceMicros", event.priceMicros);
  out.set("ts", toUnixMs(event.ts));
  out.set("slotId", event.slotId);
  out.set("device", event.device);
  return out.dump();
}

std::optional<Event> KafkaProducer::decode(std::string_view payload) {
  try {
    const Json in = Json::parse(payload);
    Event event;
    const std::string& type = in.at("type").asString();
    if (type == "impression") {
      event.type = EventType::Impression;
    } else if (type == "click") {
      event.type = EventType::Click;
    } else {
      return std::nullopt;
    }
    event.requestId = in.at("requestId").asU64();
    event.campaignId = in.at("campaignId").asU64();
    event.userId = in.at("userId").asU64();
    event.priceMicros = in.at("priceMicros").asI64();
    event.ts = fromUnixMs(in.at("ts").asI64());
    event.slotId = static_cast<u32>(in.at("slotId").asU64());
    event.device = static_cast<DeviceId>(in.at("device").asU64());
    return event;
  } catch (const JsonError&) {
    return std::nullopt;
  }
}

Status KafkaProducer::produce(std::span<const Event> batch, std::size_t* sent) {
  if (sent != nullptr) *sent = 0;
  for (const Event& event : batch) {
    const Status status = transport_->send(topic_, std::to_string(event.campaignId), encode(event));
    if (!status.isOk()) return status;
    if (sent != nullptr) ++*sent;
  }
  return Status::ok();
}

void EventWriter::start() {
  running_.store(true);
  thread_ = std::thread([this] { runLoop(); });
}

void EventWriter::stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

void EventWriter::flush(std::vector<Event>& batch) {
  if (batch.empty()) return;
  Status status;
  std::span<const Event> unsent(batch);
  for (int attempt = 0; attempt < kProduceAttempts; ++attempt) {
    std::size_t sent = 0;
    status = kafka_.produce(unsent, &sent);
    unsent = unsent.subspan(sent);
    if (status.isOk()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10 << attempt));
  }
  if (!status.isOk()) {
    // Spend was committed before queueing, so only click-through stats lose this.
    std::cerr << "events: dropping " << unsent.size() << " events: " << status.message() << "\n";
    if (metrics_ != nullptr) metrics_->inc("ad_events_dropped_total", "stage=\"produce\"", unsent.size());
  }
  batch.clear();
}

void EventWriter::runLoop() {
  using Clock = std::chrono::steady_clock;
  std::vector<Event> batch;
  batch.reserve(static_cast<std::size_t>(batchSize_));
  auto lastFlush = Clock::now();

  for (;;) {
    const bool stopping = !running_.load();
    const std::vector<Event> got = queue_.popBatch(static_cast<std::size_t>(batchSize_) - batch.size());
    batch.insert(batch.end(), got.begin(), got.end());

    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastFlush);
    if (static_cast<int>(batch.size()) >= batchSize_ || waited.count() >= flushMs_ || stopping) {
      flush(batch);
      lastFlush = Clock::now();
    }
    if (got.empty()) {
      if (stopping) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

void StatsUpdater::consume(std::span<const Event> events) {
  for (const Event& event : events) ctr_.update(event);
}

void StatsUpdater::recomputeNightly() { ctr_.decay(0.5f); }

bool EventHandler::isDuplicate(u64 requestId, EventType type, Time now) {
  return !seenIds_.insertIfAbsent(requestId, type, now);
}

HttpResponse EventHandler::handleImpression(const HttpRequest& http) {
  return handle(http, EventType::Impression);
}

HttpResponse EventHandler::handleClick(const HttpRequest& http) {
  return handle(http, EventType::Click);
}

HttpResponse EventHandler::handle(const HttpRequest& http, EventType type) {
  // HEAD is refused too: link checkers use it, and it must not spend money.
  if (http.method != "GET") return HttpResponse::text(405, "GET required\n");
  if (!verifySignature(http.target)) {
    metrics_.inc("ad_events_rejected_total", "reason=\"bad_signature\"");
    return HttpResponse::text(403, "invalid signature\n");
  }

  const auto requestId = parseNumber<u64>(http.query, "rid");
  const auto campaignId = parseNumber<u64>(http.query, "cid");
  const auto userId = parseNumber<u64>(http.query, "uid");
  const auto slotId = parseNumber<u32>(http.query, "slot");
  const auto device = parseNumber<u32>(http.query, "dev");
  const auto price = parseNumber<i64>(http.query, "price");
  const auto reserved = parseNumber<i64>(http.query, "rsv");
  const auto expiry = parseNumber<i64>(http.query, "exp");
  if (!requestId || !campaignId || !userId || !slotId || !device || !price || !reserved || !expiry) {
    return HttpResponse::text(400, "malformed tracking url\n");
  }

  std::string landingUrl;
  if (type == EventType::Click) {
    auto to = http.query.find("to");
    if (to == http.query.end()) return HttpResponse::text(400, "malformed tracking url\n");
    landingUrl = to->second;
  }

  const Time now = nowTime();
  // Expiry has to be no longer than the dedup window, or an old link could be
  // replayed for a second charge once its id had been forgotten.
  if (toUnixMs(now) > *expiry) {
    metrics_.inc("ad_events_rejected_total", "reason=\"expired\"");
    return HttpResponse::text(410, "tracking url expired\n");
  }

  // The request id comes from the caller and two callers may reuse one, so on
  // its own it would drop another campaign's event as a duplicate.
  if (isDuplicate(combineHash(combineHash(*requestId, *campaignId), *userId), type, now)) {
    metrics_.inc("ad_events_rejected_total", "reason=\"duplicate\"");
    return successResponse(type, landingUrl);  // a repeat click still has to land
  }

  Event event;
  event.type = type;
  event.requestId = *requestId;
  event.campaignId = *campaignId;
  event.userId = *userId;
  event.priceMicros = *price;
  event.reservedMicros = *reserved;
  event.slotId = *slotId;
  event.device = *device;
  event.ts = now;

  budget_.commit(event);
  if (type == EventType::Impression) freqCap_.increment(event.userId, event.campaignId);
  if (!queue_.push(event)) metrics_.inc("ad_events_dropped_total", "stage=\"queue\"");
  metrics_.inc("ad_events_total", type == EventType::Impression ? "type=\"impression\"" : "type=\"click\"");
  return successResponse(type, landingUrl);
}

}
