#include <algorithm>
#include <atomic>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "observability/observability.h"
#include "test_framework.h"
#include "test_helpers.h"

using namespace ad;
using namespace adtest;

namespace {

CampaignLoader loaderFor(std::shared_ptr<DbState> db, CampaignSlots& slots) {
  return CampaignLoader(std::make_unique<MemoryDb>(std::move(db)), slots);
}

std::vector<u32> query(const CatalogSnapshot& snap, const AdRequest& req, const UserContext& user = {}) {
  std::vector<u32> out;
  snap.index.query(req, user).appendTo(out);
  return out;
}

std::string replaced(std::string text, std::string_view from, std::string_view to) {
  const std::size_t pos = text.find(from);
  if (pos != std::string::npos) text.replace(pos, from.size(), to);
  return text;
}

bool matches(const CatalogSnapshot& snap, u64 campaignId, const AdRequest& req, const UserContext& user = {}) {
  const u32 idx = snap.idToIdx.at(campaignId);
  const std::vector<u32> found = query(snap, req, user);
  return std::find(found.begin(), found.end(), idx) != found.end();
}

std::shared_ptr<const CatalogSnapshot> targeted(CampaignSlots& slots, const std::string& targeting) {
  CampaignSpec spec;
  spec.id = 1;
  spec.targeting = targeting;
  return buildSnapshot(slots, simpleCatalog({spec}));
}

}

TEST(campaign_slots_are_stable_and_dense) {
  CampaignSlots slots(3);
  EXPECT_EQ(slots.assign(500).value(), 0);
  EXPECT_EQ(slots.assign(7).value(), 1);
  EXPECT_EQ(slots.assign(500).value(), 0);
  EXPECT_EQ(slots.find(7).value(), 1);
  EXPECT_FALSE(slots.find(8).has_value());
}

TEST(campaign_slots_refuse_new_ids_at_capacity) {
  CampaignSlots slots(2);
  slots.assign(1);
  slots.assign(2);
  EXPECT_FALSE(slots.assign(3).has_value());
  EXPECT_EQ(slots.assign(2).value(), 1);
}

TEST(loader_parses_every_field) {
  const std::string json = R"({
    "advertisers": [{"id": 9, "name": "A", "status": "active"}],
    "campaigns": [{
      "id": 5, "advertiserId": 9, "bidType": "CPC", "bidMicros": 1234,
      "dailyBudgetMicros": 111, "totalBudgetMicros": 222,
      "startTs": 1000, "endTs": 2000, "maxPerUserPerDay": 3,
      "pacing": "EVEN", "status": "active",
      "targeting": {"geos": [1, 2], "devices": [3], "os": [4], "languages": [5], "segments": [6, 7],
                    "hoursOfDay": [0, 23],
                    "excludes": [{"dim": "segment", "value": 8}]}
    }],
    "creatives": [{"id": 50, "campaignId": 5, "size": 77, "format": "video",
                   "assetUrl": "https://a/", "clickUrl": "http://b/"}]
  })";
  CampaignSlots slots(4);
  CampaignLoader loader = loaderFor(dbWith(json), slots);
  const std::vector<Campaign> campaigns = loader.loadCampaigns();
  ASSERT_EQ(campaigns.size(), 1);
  const Campaign& c = campaigns[0];
  EXPECT_EQ(c.id, 5);
  EXPECT_EQ(c.advertiserId, 9);
  EXPECT_TRUE(c.bidType == BidType::CPC);
  EXPECT_EQ(c.bidMicros, 1234);
  EXPECT_EQ(c.dailyBudgetMicros, 111);
  EXPECT_EQ(c.totalBudgetMicros, 222);
  EXPECT_EQ(toUnixMs(c.startTs), 1000);
  EXPECT_EQ(toUnixMs(c.endTs), 2000);
  EXPECT_EQ(c.maxPerUserPerDay, 3);
  EXPECT_TRUE(c.pacing == PacingMode::Even);
  EXPECT_TRUE(c.status == EntityStatus::Active);
  EXPECT_EQ(c.targeting.geos.size(), 2);
  EXPECT_EQ(c.targeting.os[0], 4);
  EXPECT_EQ(c.targeting.segments[1], 7);
  EXPECT_EQ(c.targeting.hoursOfDay.count(), 2);
  EXPECT_TRUE(c.targeting.hoursOfDay.test(23));
  EXPECT_FALSE(c.targeting.hoursOfDay.test(12));
  ASSERT_EQ(c.targeting.excludes.size(), 1);
  EXPECT_TRUE(c.targeting.excludes[0].dim == Dimension::Segment);
  EXPECT_EQ(c.targeting.excludes[0].value, 8);

  const std::vector<Creative> creatives = loader.loadCreatives();
  ASSERT_EQ(creatives.size(), 1);
  EXPECT_TRUE(creatives[0].format == Format::Video);
  EXPECT_EQ(creatives[0].size, 77);
  EXPECT_EQ(creatives[0].clickUrl, "http://b/");
}

TEST(loader_treats_missing_targeting_as_all_hours) {
  CampaignSpec spec;
  CampaignSlots slots(4);
  CampaignLoader loader = loaderFor(dbWith(simpleCatalog({spec})), slots);
  EXPECT_EQ(loader.loadCampaigns()[0].targeting.hoursOfDay.count(), 24);
}

TEST(loader_pauses_campaigns_of_inactive_or_unknown_advertisers) {
  CampaignSpec paused, unknown, active;
  paused.id = 1;
  paused.advertiser = 2;
  unknown.id = 2;
  unknown.advertiser = 999;
  active.id = 3;
  CampaignSlots slots(4);
  CampaignLoader loader = loaderFor(dbWith(simpleCatalog({paused, unknown, active})), slots);
  const std::vector<Campaign> campaigns = loader.loadCampaigns();
  ASSERT_EQ(campaigns.size(), 3);
  EXPECT_TRUE(campaigns[0].status == EntityStatus::Paused);
  EXPECT_TRUE(campaigns[1].status == EntityStatus::Paused);
  EXPECT_TRUE(campaigns[2].status == EntityStatus::Active);
}

TEST(loader_skips_bad_rows_and_keeps_the_rest) {
  CampaignSpec good, badBid, badPacing, zeroId, negativeBid, badHour;
  good.id = 1;
  badBid.id = 2;
  badBid.bidType = "NOPE";
  badPacing.id = 3;
  badPacing.pacing = "FAST";
  zeroId.id = 0;
  negativeBid.id = 4;
  negativeBid.bid = -1;
  badHour.id = 5;
  badHour.targeting = R"({"hoursOfDay":[24]})";
  CreativeSpec okCreative, dangerous;
  okCreative.id = 10;
  okCreative.campaign = 1;
  dangerous.id = 20;
  dangerous.campaign = 1;
  dangerous.click = "javascript:alert(1)";

  CampaignSlots slots(8);
  CampaignLoader loader =
      loaderFor(dbWith(catalogJson({good, badBid, badPacing, zeroId, negativeBid, badHour}, {okCreative, dangerous})), slots);
  const std::vector<Campaign> campaigns = loader.loadCampaigns();
  ASSERT_EQ(campaigns.size(), 1);
  EXPECT_EQ(campaigns[0].id, 1);
  const std::vector<Creative> creatives = loader.loadCreatives();
  ASSERT_EQ(creatives.size(), 1);
  EXPECT_EQ(creatives[0].id, 10);
}

TEST(loader_rejects_values_that_would_wrap_when_narrowed) {
  CampaignSpec good, cap, geo, exclude;
  good.id = 1;
  cap.id = 2;
  cap.cap = 65537;
  geo.id = 3;
  geo.targeting = R"({"geos":[4294968136]})";
  exclude.id = 4;
  exclude.targeting = R"({"excludes":[{"dim":"geo","value":4294968136}]})";
  CreativeSpec okCreative, hugeSize;
  okCreative.id = 10;
  okCreative.campaign = 1;
  hugeSize.id = 20;
  hugeSize.campaign = 1;
  // 2^32 + 300250. A cast to u32 would turn this into the standard size.
  const std::string json = replaced(catalogJson({good, cap, geo, exclude}, {okCreative, hugeSize}),
                                    "\"id\":20,\"campaignId\":1,\"size\":300250",
                                    "\"id\":20,\"campaignId\":1,\"size\":4295267546");
  CampaignSlots slots(8);
  CampaignLoader loader = loaderFor(dbWith(json), slots);
  const std::vector<Campaign> campaigns = loader.loadCampaigns();
  ASSERT_EQ(campaigns.size(), 1);
  EXPECT_EQ(campaigns[0].id, 1);
  const std::vector<Creative> creatives = loader.loadCreatives();
  ASSERT_EQ(creatives.size(), 1);
  EXPECT_EQ(creatives[0].id, 10);
}

TEST(loader_rejects_bids_and_budgets_outside_the_safe_range) {
  CampaignSpec good, zero, belowOneMicro, tooLarge, negativeDaily, negativeTotal, smallCpc;
  good.id = 1;
  good.bid = 1000;
  zero.id = 2;
  zero.bid = 0;
  belowOneMicro.id = 3;
  belowOneMicro.bid = 999;
  tooLarge.id = 4;
  tooLarge.bid = 1'000'000'000'001;
  negativeDaily.id = 5;
  negativeDaily.daily = -1;
  negativeTotal.id = 6;
  negativeTotal.total = -1;
  smallCpc.id = 7;
  smallCpc.bidType = "CPC";
  smallCpc.bid = 1;  // a click pays this much, so it is billable
  CampaignSlots slots(8);
  CampaignLoader loader = loaderFor(
      dbWith(simpleCatalog({good, zero, belowOneMicro, tooLarge, negativeDaily, negativeTotal, smallCpc})), slots);
  const std::vector<Campaign> campaigns = loader.loadCampaigns();
  ASSERT_EQ(campaigns.size(), 2);
  EXPECT_EQ(campaigns[0].id, 1);
  EXPECT_EQ(campaigns[1].id, 7);
}

TEST(loader_rejects_a_landing_url_too_long_for_its_click_link) {
  CampaignSpec spec;
  spec.id = 1;
  CreativeSpec fits, tooLong;
  fits.id = 10;
  fits.campaign = 1;
  fits.click = "https://e/" + std::string(4096 - 10, 'a');
  tooLong.id = 20;
  tooLong.campaign = 1;
  tooLong.click = "https://e/" + std::string(4097 - 10, 'a');
  CampaignSlots slots(4);
  CampaignLoader loader = loaderFor(dbWith(catalogJson({spec}, {fits, tooLong})), slots);
  const std::vector<Creative> creatives = loader.loadCreatives();
  ASSERT_EQ(creatives.size(), 1);
  EXPECT_EQ(creatives[0].id, 10);
}

TEST(loader_fails_when_every_row_of_a_table_is_invalid) {
  CampaignSpec bad;
  bad.id = 1;
  bad.bidType = "NOPE";
  CampaignSlots slots(4);
  CampaignLoader badCampaigns = loaderFor(dbWith(simpleCatalog({bad})), slots);
  EXPECT_THROW(badCampaigns.loadCampaigns(), JsonError);

  CampaignSpec good;
  good.id = 1;
  CreativeSpec dangerous;
  dangerous.id = 10;
  dangerous.campaign = 1;
  dangerous.click = "ftp://nope";
  CampaignLoader badCreatives = loaderFor(dbWith(catalogJson({good}, {dangerous})), slots);
  EXPECT_THROW(badCreatives.loadCreatives(), JsonError);

  CampaignLoader badAdvertisers = loaderFor(
      dbWith(R"({"advertisers":[{"id":"x","status":"active"}],"campaigns":[],"creatives":[]})"), slots);
  EXPECT_THROW(badAdvertisers.loadCampaigns(), JsonError);
}

TEST(snapshot_groups_creatives_by_campaign) {
  CampaignSpec a, b, none;
  a.id = 1;
  b.id = 2;
  none.id = 3;
  CreativeSpec ca1, ca2, cb1, orphan;
  ca1.id = 11;
  ca1.campaign = 1;
  cb1.id = 21;
  cb1.campaign = 2;
  ca2.id = 12;
  ca2.campaign = 1;
  orphan.id = 99;
  orphan.campaign = 404;

  CampaignSlots slots(8);
  const auto snap = buildSnapshot(slots, catalogJson({a, b, none}, {ca1, cb1, ca2, orphan}));
  const std::span<const Creative> first = snap->creativesFor(snap->idToIdx.at(1));
  ASSERT_EQ(first.size(), 2);
  EXPECT_EQ(first[0].id, 11);
  EXPECT_EQ(first[1].id, 12);
  EXPECT_EQ(snap->creativesFor(snap->idToIdx.at(2)).size(), 1);
  EXPECT_EQ(snap->creativesFor(snap->idToIdx.at(3)).size(), 0);
}

TEST(snapshot_slots_survive_refresh_and_reordering) {
  CampaignSlots slots(8);
  CampaignSpec a, b, c;
  a.id = 101;
  b.id = 102;
  c.id = 103;
  auto db = dbWith(simpleCatalog({a, b}));
  CampaignLoader loader = loaderFor(db, slots);
  const CatalogSnapshot first = loader.buildSnapshot();
  const u32 slotA = first.idToIdx.at(101);
  const u32 slotB = first.idToIdx.at(102);

  db->json = simpleCatalog({c, b});
  const CatalogSnapshot second = loader.buildSnapshot();
  EXPECT_EQ(second.idToIdx.at(102), slotB);
  EXPECT_NE(second.idToIdx.at(103), slotA);
  EXPECT_EQ(second.idToIdx.count(101), 0);
  EXPECT_TRUE(second.campaign(slotA).status == EntityStatus::Archived);
  EXPECT_EQ(second.creativesFor(slotA).size(), 0);
  EXPECT_EQ(second.version, first.version + 1);

  db->json = simpleCatalog({a, b, c});
  const CatalogSnapshot third = loader.buildSnapshot();
  EXPECT_EQ(third.idToIdx.at(101), slotA);
}

TEST(snapshot_drops_campaigns_beyond_capacity) {
  CampaignSpec a, b, c;
  a.id = 1;
  b.id = 2;
  c.id = 3;
  CampaignSlots slots(2);
  const auto snap = buildSnapshot(slots, simpleCatalog({a, b, c}));
  EXPECT_EQ(snap->idToIdx.size(), 2);
  EXPECT_EQ(snap->idToIdx.count(3), 0);
}

TEST(snapshot_of_an_empty_catalog_is_safe_to_query) {
  CampaignSlots slots(2);
  const auto snap = buildSnapshot(slots, "{}");
  EXPECT_TRUE(snap->campaigns.empty());
  EXPECT_TRUE(query(*snap, makeRequest()).empty());
}

TEST(targeting_empty_means_any_value) {
  CampaignSlots slots(4);
  const auto snap = targeted(slots, "{}");
  EXPECT_TRUE(matches(*snap, 1, makeRequest()));
  AdRequest unknown = makeRequest();
  unknown.geo = 0;
  unknown.device = 0;
  unknown.os = 0;
  unknown.language = 0;
  EXPECT_TRUE(matches(*snap, 1, unknown));
}

TEST(targeting_requires_a_match_on_every_listed_dimension) {
  CampaignSlots slots(4);
  const auto snap = targeted(slots, R"({"geos":[840,124],"devices":[1],"os":[1],"languages":[1]})");
  AdRequest req = makeRequest();
  EXPECT_TRUE(matches(*snap, 1, req));
  req.geo = 124;
  EXPECT_TRUE(matches(*snap, 1, req));
  req.geo = 276;
  EXPECT_FALSE(matches(*snap, 1, req));
  req = makeRequest();
  req.device = 2;
  EXPECT_FALSE(matches(*snap, 1, req));
  req = makeRequest();
  req.os = 2;
  EXPECT_FALSE(matches(*snap, 1, req));
  req = makeRequest();
  req.language = 2;
  EXPECT_FALSE(matches(*snap, 1, req));
}

TEST(targeting_segments_match_on_any_overlap) {
  CampaignSlots slots(4);
  const auto snap = targeted(slots, R"({"segments":[10,20]})");
  const AdRequest req = makeRequest();
  UserContext user;
  EXPECT_FALSE(matches(*snap, 1, req, user));
  user.segments = {5};
  EXPECT_FALSE(matches(*snap, 1, req, user));
  user.segments = {5, 20};
  EXPECT_TRUE(matches(*snap, 1, req, user));
}

TEST(targeting_excludes_win_over_includes) {
  CampaignSlots slots(4);
  const auto snap = targeted(slots, R"({"geos":[840],"excludes":[{"dim":"geo","value":840}]})");
  EXPECT_FALSE(matches(*snap, 1, makeRequest()));
}

TEST(targeting_excludes_on_each_dimension) {
  CampaignSlots slots(8);
  const std::vector<std::pair<std::string, u32>> rules = {
      {"geo", 840}, {"device", 1}, {"os", 1}, {"language", 1}, {"slotSize", kSlotSize}};
  for (const auto& [dim, value] : rules) {
    CampaignSlots local(4);
    const auto snap = targeted(local, R"({"excludes":[{"dim":")" + dim + R"(","value":)" + std::to_string(value) + "}]}");
    EXPECT_FALSE(matches(*snap, 1, makeRequest()));
    AdRequest other = makeRequest();
    other.geo = 1;
    other.device = 2;
    other.os = 2;
    other.language = 2;
    if (dim != "slotSize") EXPECT_TRUE(matches(*snap, 1, other));
  }

  const auto bySegment = targeted(slots, R"({"excludes":[{"dim":"segment","value":9}]})");
  UserContext user;
  user.segments = {1, 9};
  EXPECT_FALSE(matches(*bySegment, 1, makeRequest(), user));
  user.segments = {1};
  EXPECT_TRUE(matches(*bySegment, 1, makeRequest(), user));
}

TEST(targeting_requires_a_creative_of_the_slot_size) {
  CampaignSpec spec;
  spec.id = 1;
  CreativeSpec creative;
  creative.id = 10;
  creative.campaign = 1;
  creative.size = 72890;
  CampaignSlots slots(4);
  const auto snap = buildSnapshot(slots, catalogJson({spec}, {creative}));
  EXPECT_FALSE(matches(*snap, 1, makeRequest()));
  AdRequest req = makeRequest();
  req.slotSize = 72890;
  EXPECT_TRUE(matches(*snap, 1, req));
}

TEST(targeting_skips_inactive_campaigns_and_campaigns_with_no_creative) {
  CampaignSpec paused, archived, bare, live;
  paused.id = 1;
  paused.status = "paused";
  archived.id = 2;
  archived.status = "archived";
  bare.id = 3;
  live.id = 4;
  CreativeSpec c1, c2, c4;
  c1.id = 10;
  c1.campaign = 1;
  c2.id = 20;
  c2.campaign = 2;
  c4.id = 40;
  c4.campaign = 4;
  CampaignSlots slots(8);
  const auto snap = buildSnapshot(slots, catalogJson({paused, archived, bare, live}, {c1, c2, c4}));
  const std::vector<u32> found = query(*snap, makeRequest());
  ASSERT_EQ(found.size(), 1);
  EXPECT_EQ(found[0], snap->idToIdx.at(4));
}

TEST(targeting_index_matches_brute_force_evaluation) {
  std::minstd_rand rng(7);
  const auto pick = [&rng](u32 low, u32 high) { return std::uniform_int_distribution<u32>(low, high)(rng); };
  const auto someValues = [&](u32 high) {
    std::vector<u32> out;
    const u32 n = pick(0, 2);
    for (u32 i = 0; i < n; ++i) out.push_back(pick(1, high));
    return out;
  };

  std::vector<Campaign> campaigns(400);
  std::vector<std::vector<Creative>> creatives(campaigns.size());
  for (std::size_t i = 0; i < campaigns.size(); ++i) {
    Campaign& c = campaigns[i];
    c.id = i + 1;
    c.status = pick(0, 4) == 0 ? EntityStatus::Paused : EntityStatus::Active;
    c.targeting.geos = someValues(4);
    c.targeting.devices = someValues(3);
    c.targeting.os = someValues(3);
    c.targeting.languages = someValues(3);
    c.targeting.segments = someValues(6);
    if (pick(0, 2) == 0) {
      c.targeting.excludes.push_back(Rule{static_cast<Dimension>(pick(0, 5)), pick(1, 4)});
    }
    const u32 count = pick(0, 2);
    for (u32 k = 0; k < count; ++k) {
      Creative creative;
      creative.size = pick(1, 3);
      creatives[i].push_back(creative);
    }
  }

  TargetingIndex index;
  index.build(campaigns, [&](u32 idx) { return std::span<const Creative>(creatives[idx]); });

  const auto has = [](const std::vector<u32>& list, u32 value) {
    return std::find(list.begin(), list.end(), value) != list.end();
  };
  const auto listed = [&](const std::vector<u32>& list, u32 value) { return list.empty() || has(list, value); };

  for (int round = 0; round < 2000; ++round) {
    AdRequest req;
    req.geo = pick(0, 4);
    req.device = pick(0, 3);
    req.os = pick(0, 3);
    req.language = pick(0, 3);
    req.slotSize = pick(1, 3);
    UserContext user;
    user.segments = someValues(6);

    std::vector<u32> expected;
    for (u32 i = 0; i < campaigns.size(); ++i) {
      const Campaign& c = campaigns[i];
      const Targeting& t = c.targeting;
      if (c.status != EntityStatus::Active || creatives[i].empty()) continue;
      if (!std::any_of(creatives[i].begin(), creatives[i].end(),
                       [&](const Creative& k) { return k.size == req.slotSize; })) {
        continue;
      }
      if (!listed(t.geos, req.geo) || !listed(t.devices, req.device) || !listed(t.os, req.os) ||
          !listed(t.languages, req.language)) {
        continue;
      }
      if (!t.segments.empty() && !std::any_of(t.segments.begin(), t.segments.end(),
                                              [&](u32 s) { return has(user.segments, s); })) {
        continue;
      }
      bool excluded = false;
      for (const Rule& rule : t.excludes) {
        switch (rule.dim) {
          case Dimension::Geo: excluded |= rule.value == req.geo; break;
          case Dimension::Device: excluded |= rule.value == req.device; break;
          case Dimension::Os: excluded |= rule.value == req.os; break;
          case Dimension::Language: excluded |= rule.value == req.language; break;
          case Dimension::SlotSize: excluded |= rule.value == req.slotSize; break;
          case Dimension::Segment: excluded |= has(user.segments, rule.value); break;
        }
      }
      if (!excluded) expected.push_back(i);
    }

    std::vector<u32> actual;
    index.query(req, user).appendTo(actual);
    ASSERT_TRUE(actual == expected);
  }
}

TEST(snapshot_manager_is_empty_until_first_refresh) {
  CampaignSlots slots(4);
  SnapshotManager manager(loaderFor(dbWith(simpleCatalog({})), slots), 3600);
  EXPECT_TRUE(manager.get() == nullptr);
  manager.refreshNow();
  EXPECT_TRUE(manager.get() != nullptr);
}

TEST(snapshot_manager_keeps_an_old_snapshot_alive_for_its_readers) {
  CampaignSpec a, b;
  a.id = 1;
  a.bid = 1111;
  b.id = 2;
  auto db = dbWith(simpleCatalog({a}));
  CampaignSlots slots(4);
  SnapshotManager manager(loaderFor(db, slots), 3600);
  manager.refreshNow();
  const auto pinned = manager.get();
  const Campaign* campaign = &pinned->campaign(pinned->idToIdx.at(1));

  a.bid = 9999;
  db->json = simpleCatalog({a, b});
  manager.refreshNow();

  EXPECT_EQ(campaign->bidMicros, 1111);
  EXPECT_EQ(manager.get()->campaign(manager.get()->idToIdx.at(1)).bidMicros, 9999);
  EXPECT_GT(manager.get()->version, pinned->version);
}

TEST(snapshot_manager_keeps_serving_after_a_failed_refresh) {
  CampaignSpec a;
  a.id = 1;
  auto db = dbWith(simpleCatalog({a}));
  CampaignSlots slots(4);
  SnapshotManager manager(loaderFor(db, slots), 3600);
  manager.refreshNow();
  const u64 version = manager.get()->version;

  db->failing = true;
  EXPECT_THROW(manager.refreshNow(), std::runtime_error);
  EXPECT_EQ(manager.get()->version, version);

  db->failing = false;
  db->json = "this is not json";
  EXPECT_THROW(manager.refreshNow(), JsonError);
  EXPECT_EQ(manager.get()->version, version);
}

TEST(snapshot_manager_runs_listeners_before_publishing) {
  CampaignSpec a;
  a.id = 1;
  CampaignSlots slots(4);
  SnapshotManager manager(loaderFor(dbWith(simpleCatalog({a})), slots), 3600);
  bool ran = false;
  manager.addListener([&](const CatalogSnapshot& next) {
    ran = true;
    EXPECT_TRUE(manager.get() == nullptr);
    EXPECT_EQ(next.idToIdx.size(), 1);
  });
  manager.refreshNow();
  EXPECT_TRUE(ran);
}

TEST(snapshot_manager_background_thread_applies_changes) {
  CampaignSpec a, b;
  a.id = 1;
  b.id = 2;
  auto db = dbWith(simpleCatalog({a}));
  CampaignSlots slots(4);
  MetricsRegistry metrics;
  SnapshotManager manager(loaderFor(db, slots), 1, &metrics);
  manager.refreshNow();
  manager.startRefreshThread();
  {
    std::lock_guard lock(db->mutex);
    db->json = simpleCatalog({a, b});
  }
  EXPECT_TRUE(waitUntil([&] { return manager.get()->idToIdx.count(2) == 1; }, 4000));
  EXPECT_GE(metrics.counterValue("ad_catalog_swaps_total"), 2);
  manager.stop();
  manager.stop();
}

TEST(snapshot_manager_counts_background_failures) {
  CampaignSpec a;
  a.id = 1;
  auto db = dbWith(simpleCatalog({a}));
  CampaignSlots slots(4);
  MetricsRegistry metrics;
  SnapshotManager manager(loaderFor(db, slots), 1, &metrics);
  manager.refreshNow();
  const u64 version = manager.get()->version;
  {
    std::lock_guard lock(db->mutex);
    db->failing = true;
  }
  manager.startRefreshThread();
  EXPECT_TRUE(waitUntil([&] { return metrics.counterValue("ad_catalog_refresh_failures_total") >= 1; }, 4000));
  EXPECT_EQ(manager.get()->version, version);
}

TEST(seed_file_connection_reads_tables) {
  const std::string path = std::string(AD_TEST_DATA_DIR) + "/seed_test.json";
  writeFile(path, R"({"campaigns":[{"id":1},{"id":2}]})");
  SeedFilePgConnection db(path);
  EXPECT_EQ(db.fetchRows("campaigns").size(), 2);
}

TEST(seed_file_connection_treats_a_missing_table_as_an_error) {
  const std::string path = std::string(AD_TEST_DATA_DIR) + "/seed_missing_table.json";
  writeFile(path, R"({"advertisers":[]})");
  SeedFilePgConnection db(path);
  EXPECT_THROW(db.fetchRows("campaigns"), JsonError);
}

TEST(seed_file_connection_reports_a_missing_file) {
  SeedFilePgConnection db(std::string(AD_TEST_DATA_DIR) + "/does_not_exist.json");
  EXPECT_THROW(db.fetchRows("campaigns"), std::runtime_error);
}

TEST(shipped_seed_catalog_loads_cleanly) {
  CampaignSlots slots(16);
  CampaignLoader loader(std::make_unique<SeedFilePgConnection>(std::string(AD_SOURCE_DIR) + "/data/catalog.json"), slots);
  const CatalogSnapshot snap = loader.buildSnapshot();
  EXPECT_EQ(snap.idToIdx.size(), 4);
  EXPECT_EQ(snap.creatives.size(), 5);
  EXPECT_TRUE(snap.campaign(snap.idToIdx.at(301)).status == EntityStatus::Paused);
  EXPECT_TRUE(snap.campaign(snap.idToIdx.at(101)).status == EntityStatus::Active);
}
