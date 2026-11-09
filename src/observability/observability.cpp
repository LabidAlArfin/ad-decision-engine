#include "observability/observability.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <utility>

namespace ad {
namespace {

thread_local DebugTrace tlsTrace;

std::string counterKey(std::string_view name, std::string_view labels) {
  std::string key;
  key.reserve(name.size() + labels.size() + 2);
  key.append(name);
  if (!labels.empty()) key.append("{").append(labels).append("}");
  return key;
}

}

TraceId Tracer::begin(u64 requestId) {
  if (!enabled_) return 0;
  tlsTrace = DebugTrace{requestId, {}};
  return requestId;
}

void Tracer::stageDrop(std::string_view stage, std::string_view reason) {
  if (!enabled_) return;
  tlsTrace.drops.push_back(StageDrop{std::string(stage), std::string(reason)});
}

DebugTrace Tracer::end() {
  if (!enabled_) return {};
  return std::exchange(tlsTrace, DebugTrace{});
}

void MetricsRegistry::inc(std::string_view name, std::string_view labels, u64 delta) {
  const std::string key = counterKey(name, labels);
  {
    std::shared_lock lock(mutex_);
    auto it = counters_.find(key);
    if (it != counters_.end()) {
      it->second->fetch_add(delta, std::memory_order_relaxed);
      return;
    }
  }
  std::unique_lock lock(mutex_);
  auto& slot = counters_[key];
  if (!slot) slot = std::make_unique<std::atomic<u64>>(0);
  slot->fetch_add(delta, std::memory_order_relaxed);
}

void MetricsRegistry::observeLatency(std::string_view stage, double ms) {
  Histogram* h = nullptr;
  {
    std::shared_lock lock(mutex_);
    auto it = histograms_.find(stage);
    if (it != histograms_.end()) h = it->second.get();
  }
  if (h == nullptr) {
    std::unique_lock lock(mutex_);
    auto& slot = histograms_[std::string(stage)];
    if (!slot) slot = std::make_unique<Histogram>();
    h = slot.get();
  }
  const auto bucket = static_cast<std::size_t>(
      std::lower_bound(kBucketsMs.begin(), kBucketsMs.end(), ms) - kBucketsMs.begin());
  h->buckets[bucket].fetch_add(1, std::memory_order_relaxed);
  h->sumMicros.fetch_add(static_cast<u64>(std::max(ms, 0.0) * 1000.0), std::memory_order_relaxed);
  h->count.fetch_add(1, std::memory_order_relaxed);
}

u64 MetricsRegistry::counterValue(std::string_view name, std::string_view labels) const {
  const std::string key = counterKey(name, labels);
  std::shared_lock lock(mutex_);
  auto it = counters_.find(key);
  return it == counters_.end() ? 0 : it->second->load(std::memory_order_relaxed);
}

std::string MetricsRegistry::expose() const {
  std::shared_lock lock(mutex_);
  std::string out;
  std::string lastType;
  for (const auto& [key, value] : counters_) {
    const std::string name = key.substr(0, key.find('{'));
    if (name != lastType) {
      out += "# TYPE " + name + " counter\n";
      lastType = name;
    }
    out += key + " " + std::to_string(value->load(std::memory_order_relaxed)) + "\n";
  }
  if (!histograms_.empty()) out += "# TYPE ad_stage_latency_ms histogram\n";
  for (const auto& [stage, h] : histograms_) {
    u64 cumulative = 0;
    char le[32];
    for (std::size_t i = 0; i < kBucketsMs.size(); ++i) {
      cumulative += h->buckets[i].load(std::memory_order_relaxed);
      std::snprintf(le, sizeof le, "%g", kBucketsMs[i]);
      out += "ad_stage_latency_ms_bucket{stage=\"" + stage + "\",le=\"" + le + "\"} " +
             std::to_string(cumulative) + "\n";
    }
    cumulative += h->buckets[kBucketsMs.size()].load(std::memory_order_relaxed);
    out += "ad_stage_latency_ms_bucket{stage=\"" + stage + "\",le=\"+Inf\"} " +
           std::to_string(cumulative) + "\n";
    char sum[32];
    std::snprintf(sum, sizeof sum, "%.3f",
                  static_cast<double>(h->sumMicros.load(std::memory_order_relaxed)) / 1000.0);
    out += "ad_stage_latency_ms_sum{stage=\"" + stage + "\"} " + sum + "\n";
    out += "ad_stage_latency_ms_count{stage=\"" + stage + "\"} " +
           std::to_string(h->count.load(std::memory_order_relaxed)) + "\n";
  }
  return out;
}

}
