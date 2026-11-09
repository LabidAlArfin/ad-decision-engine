#include "catalog/catalog.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "observability/observability.h"

namespace ad {

std::optional<u32> CampaignSlots::assign(u64 campaignId) {
  if (auto existing = find(campaignId)) return existing;
  std::unique_lock lock(mutex_);
  auto it = slots_.find(campaignId);
  if (it != slots_.end()) return it->second;
  if (slots_.size() >= capacity_) return std::nullopt;
  const u32 slot = static_cast<u32>(slots_.size());
  slots_.emplace(campaignId, slot);
  return slot;
}

std::optional<u32> CampaignSlots::find(u64 campaignId) const {
  std::shared_lock lock(mutex_);
  auto it = slots_.find(campaignId);
  if (it == slots_.end()) return std::nullopt;
  return it->second;
}

void TargetingIndex::build(const std::vector<Campaign>& campaigns, const CreativesFn& creativesFor) {
  const auto addDimension = [&](Postings& postings, Dimension dim, const auto& values, u32 idx) {
    if (values.empty()) {
      anyBuckets_[static_cast<std::size_t>(dim)].add(idx);
      return;
    }
    for (u32 value : values) postings[value].add(idx);
  };

  for (u32 idx = 0; idx < campaigns.size(); ++idx) {
    const Campaign& c = campaigns[idx];
    const std::span<const Creative> creatives = creativesFor(idx);
    if (c.status != EntityStatus::Active || creatives.empty()) continue;

    addDimension(geo_, Dimension::Geo, c.targeting.geos, idx);
    addDimension(device_, Dimension::Device, c.targeting.devices, idx);
    addDimension(os_, Dimension::Os, c.targeting.os, idx);
    addDimension(language_, Dimension::Language, c.targeting.languages, idx);
    addDimension(segment_, Dimension::Segment, c.targeting.segments, idx);
    for (const Creative& creative : creatives) slotSize_[creative.size].add(idx);
    for (const Rule& rule : c.targeting.excludes) {
      excludes_[static_cast<std::size_t>(rule.dim)][rule.value].add(idx);
    }
  }
}

Roaring TargetingIndex::andAny(const Postings& dim, u32 key, const Roaring& any) {
  Roaring out = any;
  if (auto it = dim.find(key); it != dim.end()) out |= it->second;
  return out;
}

void TargetingIndex::subtractExcluded(Roaring& result, Dimension dim, u32 value) const {
  const Postings& postings = excludes_[static_cast<std::size_t>(dim)];
  if (auto it = postings.find(value); it != postings.end()) result -= it->second;
}

Roaring TargetingIndex::query(const AdRequest& req, const UserContext& user) const {
  // A campaign must have a creative that fits the slot, so slot size is the
  // only dimension with no "any" bucket. It is usually the most selective.
  auto slot = slotSize_.find(req.slotSize);
  if (slot == slotSize_.end()) return Roaring();
  Roaring result = slot->second;

  const auto any = [&](Dimension d) -> const Roaring& {
    return anyBuckets_[static_cast<std::size_t>(d)];
  };
  result &= andAny(geo_, req.geo, any(Dimension::Geo));
  result &= andAny(device_, req.device, any(Dimension::Device));
  result &= andAny(os_, req.os, any(Dimension::Os));
  result &= andAny(language_, req.language, any(Dimension::Language));

  Roaring segments = any(Dimension::Segment);
  for (SegId s : user.segments) {
    if (auto it = segment_.find(s); it != segment_.end()) segments |= it->second;
  }
  result &= segments;

  subtractExcluded(result, Dimension::Geo, req.geo);
  subtractExcluded(result, Dimension::Device, req.device);
  subtractExcluded(result, Dimension::Os, req.os);
  subtractExcluded(result, Dimension::Language, req.language);
  subtractExcluded(result, Dimension::SlotSize, req.slotSize);
  for (SegId s : user.segments) subtractExcluded(result, Dimension::Segment, s);
  return result;
}

std::vector<Json> SeedFilePgConnection::fetchRows(std::string_view table) {
  std::ifstream in(path_, std::ios::binary);
  if (!in) throw std::runtime_error("catalog: cannot open " + path_);
  std::ostringstream text;
  text << in.rdbuf();
  const Json doc = Json::parse(text.str());
  const Json* rows = doc.find(table);
  // As a missing table is in Postgres. Treating it as empty would publish a
  // catalog with nothing in it and stop all serving.
  if (rows == nullptr) throw JsonError("catalog: missing table '" + std::string(table) + "'");
  return rows->asArray();
}

namespace {

// $1,000,000. Keeps a CPC bid inside i64 once it is converted to CPM.
constexpr i64 kMaxBidMicros = 1'000'000'000'000;
// Below this an impression costs less than one micro, so it would serve free.
constexpr i64 kMinCpmBidMicros = 1000;
// Encoding can triple a URL, and the click link has to fit the HTTP header limit.
constexpr std::size_t kMaxClickUrl = 4096;

template <class T>
T checkedUnsigned(const Json& value, std::string_view field) {
  const u64 raw = value.asU64();
  if (raw > std::numeric_limits<T>::max()) {
    throw JsonError("field '" + std::string(field) + "' is out of range");
  }
  return static_cast<T>(raw);
}

EntityStatus parseEntityStatus(const std::string& s) {
  if (s == "active") return EntityStatus::Active;
  if (s == "paused") return EntityStatus::Paused;
  if (s == "archived") return EntityStatus::Archived;
  throw JsonError("unknown status '" + s + "'");
}

BidType parseBidType(const std::string& s) {
  if (s == "CPM") return BidType::CPM;
  if (s == "CPC") return BidType::CPC;
  throw JsonError("unknown bidType '" + s + "'");
}

PacingMode parsePacing(const std::string& s) {
  if (s == "ASAP") return PacingMode::Asap;
  if (s == "EVEN") return PacingMode::Even;
  throw JsonError("unknown pacing '" + s + "'");
}

Format parseFormat(const std::string& s) {
  if (s == "banner") return Format::Banner;
  if (s == "video") return Format::Video;
  if (s == "native") return Format::Native;
  throw JsonError("unknown format '" + s + "'");
}

Dimension parseDimension(const std::string& s) {
  if (s == "geo") return Dimension::Geo;
  if (s == "device") return Dimension::Device;
  if (s == "os") return Dimension::Os;
  if (s == "language") return Dimension::Language;
  if (s == "segment") return Dimension::Segment;
  if (s == "slotSize") return Dimension::SlotSize;
  throw JsonError("unknown dimension '" + s + "'");
}

std::vector<u32> parseIds(const Json& parent, std::string_view key) {
  std::vector<u32> out;
  const Json* list = parent.find(key);
  if (list == nullptr) return out;
  for (const Json& v : list->asArray()) out.push_back(checkedUnsigned<u32>(v, key));
  return out;
}

Targeting parseTargeting(const Json* json) {
  Targeting t;
  if (json == nullptr) return t;
  t.geos = parseIds(*json, "geos");
  t.devices = parseIds(*json, "devices");
  t.os = parseIds(*json, "os");
  t.languages = parseIds(*json, "languages");
  t.segments = parseIds(*json, "segments");
  if (const Json* hours = json->find("hoursOfDay"); hours != nullptr && !hours->asArray().empty()) {
    t.hoursOfDay.reset();
    for (const Json& h : hours->asArray()) {
      const u64 hour = h.asU64();
      if (hour >= 24) throw JsonError("hoursOfDay out of range");
      t.hoursOfDay.set(hour);
    }
  }
  if (const Json* excludes = json->find("excludes")) {
    for (const Json& rule : excludes->asArray()) {
      t.excludes.push_back(Rule{parseDimension(rule.at("dim").asString()),
                                checkedUnsigned<u32>(rule.at("value"), "excludes.value")});
    }
  }
  return t;
}

Campaign parseCampaign(const Json& row) {
  Campaign c;
  c.id = row.at("id").asU64();
  c.advertiserId = row.at("advertiserId").asU64();
  c.bidType = parseBidType(row.at("bidType").asString());
  c.bidMicros = row.at("bidMicros").asI64();
  c.dailyBudgetMicros = row.at("dailyBudgetMicros").asI64();
  c.totalBudgetMicros = row.at("totalBudgetMicros").asI64();
  c.startTs = fromUnixMs(row.at("startTs").asI64());
  c.endTs = fromUnixMs(row.at("endTs").asI64());
  if (const Json* cap = row.find("maxPerUserPerDay")) {
    c.maxPerUserPerDay = checkedUnsigned<u16>(*cap, "maxPerUserPerDay");
  }
  c.pacing = parsePacing(row.at("pacing").asString());
  c.status = parseEntityStatus(row.at("status").asString());
  c.targeting = parseTargeting(row.find("targeting"));
  if (c.id == 0 || c.bidMicros <= 0 || c.bidMicros > kMaxBidMicros) {
    throw JsonError("invalid campaign id or bid");
  }
  if (c.bidType == BidType::CPM && c.bidMicros < kMinCpmBidMicros) {
    throw JsonError("CPM bid is below one micro per impression");
  }
  if (c.dailyBudgetMicros < 0 || c.totalBudgetMicros < 0) throw JsonError("negative budget");
  return c;
}

bool isHttpUrl(const std::string& url) {
  return url.starts_with("http://") || url.starts_with("https://");
}

Creative parseCreative(const Json& row) {
  Creative c;
  c.id = row.at("id").asU64();
  c.campaignId = row.at("campaignId").asU64();
  c.size = checkedUnsigned<SizeId>(row.at("size"), "size");
  c.format = parseFormat(row.at("format").asString());
  c.assetUrl = row.at("assetUrl").asString();
  c.clickUrl = row.at("clickUrl").asString();
  // The click endpoint redirects here, so nothing but http(s) may reach it.
  if (!isHttpUrl(c.clickUrl)) throw JsonError("clickUrl must be http(s)");
  if (c.clickUrl.size() > kMaxClickUrl) throw JsonError("clickUrl is too long");
  return c;
}

template <class T, class Parse>
std::vector<T> parseRows(const std::vector<Json>& rows, const char* table, Parse parse) {
  std::vector<T> out;
  out.reserve(rows.size());
  for (const Json& row : rows) {
    try {
      out.push_back(parse(row));
    } catch (const JsonError& e) {
      std::cerr << "catalog: skipping bad row in " << table << ": " << e.what() << "\n";
    }
  }
  // One bad row is skipped, but all of them means something is wrong with the
  // source. Failing keeps the last good catalog serving.
  if (!rows.empty() && out.empty()) {
    throw JsonError("catalog: every row in " + std::string(table) + " is invalid");
  }
  return out;
}

}

std::vector<Campaign> CampaignLoader::loadCampaigns() {
  std::vector<Campaign> campaigns =
      parseRows<Campaign>(db_->fetchRows("campaigns"), "campaigns", parseCampaign);

  std::unordered_map<u64, EntityStatus> advertisers;
  const std::vector<Json> advertiserRows = db_->fetchRows("advertisers");
  for (const Json& row : advertiserRows) {
    try {
      advertisers[row.at("id").asU64()] = parseEntityStatus(row.at("status").asString());
    } catch (const JsonError& e) {
      std::cerr << "catalog: skipping bad row in advertisers: " << e.what() << "\n";
    }
  }
  if (!advertiserRows.empty() && advertisers.empty()) {
    throw JsonError("catalog: every row in advertisers is invalid");
  }
  for (Campaign& c : campaigns) {
    auto it = advertisers.find(c.advertiserId);
    if ((it == advertisers.end() || it->second != EntityStatus::Active) &&
        c.status == EntityStatus::Active) {
      c.status = EntityStatus::Paused;
    }
  }
  return campaigns;
}

std::vector<Creative> CampaignLoader::loadCreatives() {
  return parseRows<Creative>(db_->fetchRows("creatives"), "creatives", parseCreative);
}

CatalogSnapshot CampaignLoader::buildSnapshot() {
  std::vector<Campaign> campaigns = loadCampaigns();
  std::vector<Creative> creatives = loadCreatives();

  CatalogSnapshot snap;
  for (Campaign& c : campaigns) {
    const std::optional<u32> slot = slots_.assign(c.id);
    if (!slot) {
      std::cerr << "catalog: campaign capacity reached, dropping campaign " << c.id << "\n";
      continue;
    }
    if (*slot >= snap.campaigns.size()) snap.campaigns.resize(*slot + 1);
    snap.idToIdx[c.id] = *slot;
    snap.campaigns[*slot] = std::move(c);
  }

  std::vector<std::vector<Creative>> bySlot(snap.campaigns.size());
  for (Creative& creative : creatives) {
    auto it = snap.idToIdx.find(creative.campaignId);
    if (it != snap.idToIdx.end()) bySlot[it->second].push_back(std::move(creative));
  }
  snap.creativeOffsets.reserve(bySlot.size() + 1);
  snap.creativeOffsets.push_back(0);
  for (std::vector<Creative>& group : bySlot) {
    for (Creative& creative : group) snap.creatives.push_back(std::move(creative));
    snap.creativeOffsets.push_back(static_cast<u32>(snap.creatives.size()));
  }

  snap.index.build(snap.campaigns, [&snap](u32 idx) { return snap.creativesFor(idx); });
  snap.version = nextVersion_++;
  return snap;
}

void SnapshotManager::refreshNow() {
  std::lock_guard lock(refreshMutex_);
  auto next = std::make_shared<const CatalogSnapshot>(loader_.buildSnapshot());
  swap(std::move(next));
}

void SnapshotManager::swap(std::shared_ptr<const CatalogSnapshot> next) {
  for (const Listener& listener : listeners_) listener(*next);
  current_.store(std::move(next));
  if (metrics_ != nullptr) metrics_->inc("ad_catalog_swaps_total");
}

void SnapshotManager::startRefreshThread() {
  thread_ = std::thread([this] {
    std::unique_lock lock(stopMutex_);
    while (!stopCv_.wait_for(lock, std::chrono::seconds(refreshSec_), [this] { return stopping_; })) {
      lock.unlock();
      try {
        refreshNow();
      } catch (const std::exception& e) {
        // Keep serving the last good catalog.
        std::cerr << "catalog: refresh failed: " << e.what() << "\n";
        if (metrics_ != nullptr) metrics_->inc("ad_catalog_refresh_failures_total");
      }
      lock.lock();
    }
  });
}

void SnapshotManager::stop() {
  {
    std::lock_guard lock(stopMutex_);
    stopping_ = true;
  }
  stopCv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

}
