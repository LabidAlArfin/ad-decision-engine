#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/containers.h"
#include "common/json.h"
#include "common/roaring.h"
#include "domain/model.h"

namespace ad {

class MetricsRegistry;

// Maps campaign ids to dense indexes that never change for the process
// lifetime. BudgetManager and PacingController keep their counters in arrays
// indexed this way, so a catalog refresh must not renumber campaigns.
class CampaignSlots {
 public:
  explicit CampaignSlots(u32 capacity) : capacity_(capacity) {}
  std::optional<u32> assign(u64 campaignId);  // nullopt once capacity is reached
  std::optional<u32> find(u64 campaignId) const;
  u32 capacity() const { return capacity_; }

 private:
  mutable std::shared_mutex mutex_;
  flat_hash_map<u64, u32> slots_;
  u32 capacity_;
};

class TargetingIndex {
 public:
  using CreativesFn = std::function<std::span<const Creative>(u32)>;

  void build(const std::vector<Campaign>& campaigns, const CreativesFn& creativesFor);
  Roaring query(const AdRequest& req, const UserContext& user) const;

 private:
  using Postings = std::map<u32, Roaring>;

  static Roaring andAny(const Postings& dim, u32 key, const Roaring& any);
  void subtractExcluded(Roaring& result, Dimension dim, u32 value) const;

  Postings geo_, device_, os_, language_, segment_, slotSize_;
  std::array<Roaring, kDimensionCount> anyBuckets_;
  std::array<Postings, kDimensionCount> excludes_;
};

struct CatalogSnapshot {
  std::vector<Campaign> campaigns;  // indexed by stable slot; unused slots are Archived
  flat_hash_map<u64, u32> idToIdx;
  TargetingIndex index;
  u64 version = 0;
  std::vector<Creative> creatives;  // grouped by campaign slot
  std::vector<u32> creativeOffsets;

  const Campaign& campaign(u32 idx) const { return campaigns[idx]; }
  std::span<const Creative> creativesFor(u32 idx) const {
    return std::span<const Creative>(creatives).subspan(
        creativeOffsets[idx], creativeOffsets[idx + 1] - creativeOffsets[idx]);
  }
};

// Reads catalog rows. A libpq implementation can satisfy this with
// `SELECT row_to_json(t) FROM <table> t`, which yields the same row shape.
class PgConnection {
 public:
  virtual ~PgConnection() = default;
  virtual std::vector<Json> fetchRows(std::string_view table) = 0;
};

// Development stand-in for Postgres: tables are arrays in one JSON file, which
// is re-read on every refresh, so edits appear on the next catalog swap.
class SeedFilePgConnection : public PgConnection {
 public:
  explicit SeedFilePgConnection(std::string path) : path_(std::move(path)) {}
  std::vector<Json> fetchRows(std::string_view table) override;

 private:
  std::string path_;
};

class CampaignLoader {
 public:
  CampaignLoader(std::unique_ptr<PgConnection> db, CampaignSlots& slots)
      : db_(std::move(db)), slots_(slots) {}

  std::vector<Campaign> loadCampaigns();
  std::vector<Creative> loadCreatives();
  CatalogSnapshot buildSnapshot();

 private:
  std::unique_ptr<PgConnection> db_;
  CampaignSlots& slots_;
  u64 nextVersion_ = 1;
};

class SnapshotManager {
 public:
  using Listener = std::function<void(const CatalogSnapshot&)>;

  SnapshotManager(CampaignLoader loader, int refreshSec, MetricsRegistry* metrics = nullptr)
      : loader_(std::move(loader)), refreshSec_(refreshSec), metrics_(metrics) {}
  ~SnapshotManager() { stop(); }

  std::shared_ptr<const CatalogSnapshot> get() const { return current_.load(); }

  // Listeners run before the new snapshot is published, so budgets and pacing
  // know about a campaign before any request can select it.
  void addListener(Listener listener) { listeners_.push_back(std::move(listener)); }

  // Builds and publishes a snapshot. Throws on failure and leaves the current
  // snapshot in place.
  void refreshNow();
  void startRefreshThread();
  void stop();

 private:
  void swap(std::shared_ptr<const CatalogSnapshot> next);

  AtomicSharedPtr<const CatalogSnapshot> current_;
  CampaignLoader loader_;
  int refreshSec_;
  MetricsRegistry* metrics_;
  std::vector<Listener> listeners_;
  std::mutex refreshMutex_;

  std::thread thread_;
  std::mutex stopMutex_;
  std::condition_variable stopCv_;
  bool stopping_ = false;
};

}
