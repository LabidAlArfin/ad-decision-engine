#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>

#include "common/types.h"
#include "observability/debug_trace.h"

namespace ad {

// Records why candidates fell out of one request. State is per thread, so one
// Tracer serves all request threads as long as begin/end stay on one thread.
class Tracer {
 public:
  explicit Tracer(bool enabled) : enabled_(enabled) {}
  TraceId begin(u64 requestId);
  void stageDrop(std::string_view stage, std::string_view reason);
  DebugTrace end();

 private:
  bool enabled_;
};

class MetricsRegistry {
 public:
  // `labels` is preformatted, for example: filter="budget".
  void inc(std::string_view name, std::string_view labels = {}, u64 delta = 1);
  void observeLatency(std::string_view stage, double ms);
  std::string expose() const;
  u64 counterValue(std::string_view name, std::string_view labels = {}) const;

 private:
  static constexpr std::array<double, 10> kBucketsMs = {0.25, 0.5, 1, 2, 5, 10, 25, 50, 100, 250};
  struct Histogram {
    std::array<std::atomic<u64>, kBucketsMs.size() + 1> buckets{};  // last one is +Inf
    std::atomic<u64> sumMicros{0};
    std::atomic<u64> count{0};
  };

  mutable std::shared_mutex mutex_;
  std::map<std::string, std::unique_ptr<std::atomic<u64>>, std::less<>> counters_;
  std::map<std::string, std::unique_ptr<Histogram>, std::less<>> histograms_;
};

}
