#pragma once

#include <bitset>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/containers.h"
#include "common/types.h"
#include "observability/debug_trace.h"

namespace ad {

struct CatalogSnapshot;

enum class EntityStatus : u8 { Active, Paused, Archived };
enum class BidType : u8 { CPM, CPC };
enum class PacingMode : u8 { Asap, Even };
enum class Format : u8 { Banner, Video, Native };
enum class EventType : u8 { Impression, Click };
enum class Dimension : u8 { Geo, Device, Os, Language, Segment, SlotSize };
inline constexpr std::size_t kDimensionCount = 6;

// A negative targeting rule: never serve when `dim` equals `value`.
struct Rule {
  Dimension dim = Dimension::Geo;
  u32 value = 0;
};

struct Advertiser {
  u64 id = 0;
  std::string name;
  EntityStatus status = EntityStatus::Active;
};

// An empty list means "any value" for that dimension.
struct Targeting {
  std::vector<GeoId> geos;
  std::vector<DeviceId> devices;
  std::vector<OsId> os;
  std::vector<LangId> languages;
  std::vector<SegId> segments;
  std::bitset<24> hoursOfDay{0xFFFFFF};
  std::vector<Rule> excludes;
};

struct Campaign {
  u64 id = 0;
  u64 advertiserId = 0;
  BidType bidType = BidType::CPM;
  i64 bidMicros = 0;  // CPM: per 1000 impressions. CPC: per click.
  i64 dailyBudgetMicros = 0;
  i64 totalBudgetMicros = 0;
  Time startTs{};
  Time endTs{};
  u16 maxPerUserPerDay = 0;  // 0 means no frequency cap
  PacingMode pacing = PacingMode::Asap;
  EntityStatus status = EntityStatus::Archived;
  Targeting targeting;
};

struct Creative {
  u64 id = 0;
  u64 campaignId = 0;
  SizeId size = 0;
  Format format = Format::Banner;
  std::string assetUrl;
  std::string clickUrl;
};

struct AdRequest {
  u64 requestId = 0;
  u64 userId = 0;
  GeoId geo = 0;
  DeviceId device = 0;
  OsId os = 0;
  LangId language = 0;
  std::string pageUrl;
  u32 slotId = 0;
  u32 slotSize = 0;
  i64 floorMicros = 0;
  Time ts{};
};

struct UserContext {
  u64 userId = 0;
  std::vector<SegId> segments;
  small_map<u64, u16> recentImpressions;  // campaign id -> impressions today
};

struct Candidate {
  u32 campaignIdx = 0;
  u64 creativeId = 0;
  i64 bidMicros = 0;
  float pctr = 0.f;
  float score = 0.f;
  i64 ecpmMicros = 0;  // bid normalized to CPM, set by the Scorer
  const Campaign* campaign = nullptr;  // owned by the snapshot pinned for the request
};

struct AuctionResult {
  std::optional<Candidate> winner;
  i64 priceMicros = 0;  // clearing price, CPM
  std::string noFillReason;
  DebugTrace trace;
  // Keeps winner.campaign valid after decide() returns.
  std::shared_ptr<const CatalogSnapshot> snapshot;
};

struct Event {
  EventType type = EventType::Impression;
  u64 requestId = 0;
  u64 campaignId = 0;
  u64 userId = 0;
  i64 priceMicros = 0;
  Time ts{};
  u32 slotId = 0;
  DeviceId device = 0;
  i64 reservedMicros = 0;
};

inline i64 impressionCostMicros(i64 cpmPriceMicros) { return cpmPriceMicros / 1000; }

}
