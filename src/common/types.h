#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace ad {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

using GeoId = u32;
using DeviceId = u32;
using OsId = u32;
using LangId = u32;
using SegId = u32;
using SizeId = u32;

// Wall-clock timestamp. All money values in the engine are integer micros
// (1 USD = 1,000,000).
using Time = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>;

inline Time nowTime() {
  return std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
}
inline i64 toUnixMs(Time t) { return t.time_since_epoch().count(); }
inline Time fromUnixMs(i64 ms) { return Time(std::chrono::milliseconds(ms)); }

inline constexpr i64 kMsPerHour = 3'600'000;
inline constexpr i64 kMsPerDay = 86'400'000;

// Days since the Unix epoch (UTC). Budgets and frequency caps roll over on it.
inline i64 utcDay(Time t) {
  const i64 ms = toUnixMs(t);
  return (ms >= 0 ? ms : ms - kMsPerDay + 1) / kMsPerDay;
}
inline int utcHour(Time t) {
  const i64 intoDay = toUnixMs(t) - utcDay(t) * kMsPerDay;
  return static_cast<int>(intoDay / kMsPerHour);
}

// Result of an operation that can fail. Not to be confused with EntityStatus
// (active / paused), which the diagram also labels "Status".
class Status {
 public:
  Status() = default;
  static Status ok() { return Status(); }
  static Status error(std::string message) {
    Status s;
    s.ok_ = false;
    s.message_ = std::move(message);
    return s;
  }
  bool isOk() const { return ok_; }
  const std::string& message() const { return message_; }

 private:
  bool ok_ = true;
  std::string message_;
};

// Finalizer from MurmurHash3. std::hash<u64> is the identity on common
// standard libraries, which would put sequential ids in adjacent shards.
inline u64 mixHash(u64 x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}
inline u64 combineHash(u64 a, u64 b) { return mixHash(a * 0x9E3779B97F4A7C15ULL + b); }

}
