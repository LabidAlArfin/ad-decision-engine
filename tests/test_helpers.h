#pragma once

#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "catalog/catalog.h"
#include "common/json.h"
#include "domain/model.h"

namespace adtest {

inline constexpr ad::u32 kSlotSize = 300250;
inline constexpr ad::i64 kFarFuture = 4102444800000;  // the year 2100, unix ms
inline constexpr ad::i64 kRequestTimeMs = 1'700'000'000'000;

struct DbState {
  std::mutex mutex;
  std::string json;
  bool failing = false;
};

class MemoryDb : public ad::PgConnection {
 public:
  explicit MemoryDb(std::shared_ptr<DbState> state) : state_(std::move(state)) {}

  std::vector<ad::Json> fetchRows(std::string_view table) override {
    std::lock_guard lock(state_->mutex);
    if (state_->failing) throw std::runtime_error("db: connection refused");
    const ad::Json doc = ad::Json::parse(state_->json);
    const ad::Json* rows = doc.find(table);
    return rows == nullptr ? std::vector<ad::Json>{} : rows->asArray();
  }

 private:
  std::shared_ptr<DbState> state_;
};

struct CampaignSpec {
  ad::u64 id = 1;
  ad::u64 advertiser = 1;  // 1 is active and 2 is paused in catalogJson()
  std::string bidType = "CPM";
  ad::i64 bid = 1'000'000;
  ad::i64 daily = 1'000'000'000;
  ad::i64 total = 10'000'000'000;
  ad::i64 start = 0;
  ad::i64 end = kFarFuture;
  int cap = 0;
  std::string pacing = "ASAP";
  std::string status = "active";
  std::string targeting = "{}";

  std::string json() const {
    using std::to_string;
    return "{\"id\":" + to_string(id) + ",\"advertiserId\":" + to_string(advertiser) +
           ",\"bidType\":\"" + bidType + "\",\"bidMicros\":" + to_string(bid) +
           ",\"dailyBudgetMicros\":" + to_string(daily) + ",\"totalBudgetMicros\":" + to_string(total) +
           ",\"startTs\":" + to_string(start) + ",\"endTs\":" + to_string(end) +
           ",\"maxPerUserPerDay\":" + to_string(cap) + ",\"pacing\":\"" + pacing + "\",\"status\":\"" +
           status + "\",\"targeting\":" + targeting + "}";
  }
};

struct CreativeSpec {
  ad::u64 id = 1;
  ad::u64 campaign = 1;
  ad::u32 size = kSlotSize;
  std::string format = "banner";
  std::string asset = "https://cdn.example.com/a.png";
  std::string click = "https://example.com/landing";

  std::string json() const {
    using std::to_string;
    return "{\"id\":" + to_string(id) + ",\"campaignId\":" + to_string(campaign) +
           ",\"size\":" + to_string(size) + ",\"format\":\"" + format + "\",\"assetUrl\":\"" + asset +
           "\",\"clickUrl\":\"" + click + "\"}";
  }
};

inline std::string catalogJson(const std::vector<CampaignSpec>& campaigns,
                               const std::vector<CreativeSpec>& creatives) {
  std::string out =
      "{\"advertisers\":[{\"id\":1,\"name\":\"Active\",\"status\":\"active\"},"
      "{\"id\":2,\"name\":\"Paused\",\"status\":\"paused\"}],\"campaigns\":[";
  for (std::size_t i = 0; i < campaigns.size(); ++i) out += (i ? "," : "") + campaigns[i].json();
  out += "],\"creatives\":[";
  for (std::size_t i = 0; i < creatives.size(); ++i) out += (i ? "," : "") + creatives[i].json();
  out += "]}";
  return out;
}

// One standard-size creative for each campaign, with id = campaign id * 10.
inline std::string simpleCatalog(const std::vector<CampaignSpec>& campaigns) {
  std::vector<CreativeSpec> creatives;
  for (const CampaignSpec& c : campaigns) {
    CreativeSpec creative;
    creative.id = c.id * 10;
    creative.campaign = c.id;
    creatives.push_back(creative);
  }
  return catalogJson(campaigns, creatives);
}

inline std::shared_ptr<DbState> dbWith(std::string json) {
  auto db = std::make_shared<DbState>();
  db->json = std::move(json);
  return db;
}

inline std::shared_ptr<const ad::CatalogSnapshot> buildSnapshot(ad::CampaignSlots& slots, std::string json) {
  ad::CampaignLoader loader(std::make_unique<MemoryDb>(dbWith(std::move(json))), slots);
  return std::make_shared<const ad::CatalogSnapshot>(loader.buildSnapshot());
}

inline ad::AdRequest makeRequest(ad::u64 requestId = 1) {
  ad::AdRequest req;
  req.requestId = requestId;
  req.userId = 42;
  req.geo = 840;
  req.device = 1;
  req.os = 1;
  req.language = 1;
  req.slotId = 7;
  req.slotSize = kSlotSize;
  req.ts = ad::fromUnixMs(kRequestTimeMs);
  return req;
}

inline void writeFile(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot write " + path);
  out << content;
}

}
