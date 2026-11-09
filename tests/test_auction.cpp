#include <algorithm>
#include <bitset>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/decision_engine.h"
#include "observability/observability.h"
#include "pipeline/pipeline.h"
#include "scoring/scoring.h"
#include "state/runtime_state.h"
#include "test_framework.h"
#include "test_helpers.h"

using namespace ad;
using namespace adtest;

namespace {

Candidate bidder(u32 idx, i64 ecpm) {
  Candidate c;
  c.campaignIdx = idx;
  c.creativeId = idx * 10;
  c.bidMicros = ecpm;
  c.ecpmMicros = ecpm;
  c.score = static_cast<float>(ecpm);
  return c;
}

Campaign campaignOf(BidType type, i64 bid) {
  Campaign c;
  c.id = 1;
  c.bidType = type;
  c.bidMicros = bid;
  c.status = EntityStatus::Active;
  c.startTs = fromUnixMs(0);
  c.endTs = fromUnixMs(kFarFuture);
  return c;
}

Candidate candidateFor(const Campaign& campaign, u32 idx = 0) {
  Candidate c;
  c.campaignIdx = idx;
  c.bidMicros = campaign.bidMicros;
  c.campaign = &campaign;
  return c;
}

class ScriptedFilter : public Filter {
 public:
  using Keep = std::function<bool(const Candidate&)>;

  ScriptedFilter(std::string name, std::vector<std::string>* log, Keep keep, bool acceptWin = true)
      : name_(std::move(name)), log_(log), keep_(std::move(keep)), acceptWin_(acceptWin) {}

  bool apply(const Candidate& c, const AdRequest&, const UserContext&) override {
    log_->push_back("apply:" + name_);
    return keep_(c);
  }
  std::string_view name() const override { return name_; }
  bool onWin(const Candidate&, i64) override {
    log_->push_back("win:" + name_);
    return acceptWin_;
  }
  void undoWin(const Candidate&, i64) override { log_->push_back("undo:" + name_); }

 private:
  std::string name_;
  std::vector<std::string>* log_;
  Keep keep_;
  bool acceptWin_;
};

class VetoFilter : public Filter {
 public:
  explicit VetoFilter(std::set<u64> vetoed) : vetoed_(std::move(vetoed)) {}
  bool apply(const Candidate&, const AdRequest&, const UserContext&) override { return true; }
  std::string_view name() const override { return "veto"; }
  bool onWin(const Candidate& c, i64) override { return vetoed_.count(c.campaign->id) == 0; }

 private:
  std::set<u64> vetoed_;
};

class SlowFilter : public Filter {
 public:
  bool apply(const Candidate&, const AdRequest&, const UserContext&) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    return true;
  }
  std::string_view name() const override { return "slow"; }
};

struct EngineOptions {
  bool load = true;
  int deadlineMs = 2000;
  std::unique_ptr<Filter> extraFilter;
};

struct EngineFixture {
  MetricsRegistry metrics;
  Tracer tracer{true};
  CampaignSlots slots{256};
  BudgetManager budget{slots, nullptr, 5};
  PacingController pacing{256};
  FrequencyCapStore freqCap{nullptr, 4096, 3600};
  UserProfileStore users{nullptr, 4096};
  std::shared_ptr<DbState> db;
  SnapshotManager snapshots;
  CtrTable ctr{1.f, 99.f};
  std::unique_ptr<DecisionEngine> engine;

  explicit EngineFixture(std::string catalog, EngineOptions options = {})
      : db(dbWith(std::move(catalog))),
        snapshots(CampaignLoader(std::make_unique<MemoryDb>(db), slots), 3600, &metrics) {
    snapshots.addListener([this](const CatalogSnapshot& next) {
      budget.applyCatalog(next);
      pacing.applyCatalog(next, fromUnixMs(kRequestTimeMs));
    });
    FilterPipeline pipeline(&metrics, &tracer);
    pipeline.add(std::make_unique<ScheduleFilter>());
    pipeline.add(std::make_unique<FloorPriceFilter>());
    pipeline.add(std::make_unique<BudgetFilter>(budget));
    pipeline.add(std::make_unique<PacingFilter>(pacing));
    pipeline.add(std::make_unique<FrequencyCapFilter>(freqCap));
    if (options.extraFilter) pipeline.add(std::move(options.extraFilter));
    engine = std::make_unique<DecisionEngine>(
        snapshots, CandidateRetriever{}, std::move(pipeline), std::make_unique<CtrScorer>(ctr, 0.f, 0.01f),
        std::make_unique<SecondPriceAuction>(), users, options.deadlineMs, tracer, metrics);
    if (options.load) snapshots.refreshNow();
  }

  u32 idxOf(u64 campaignId) const { return slots.find(campaignId).value(); }
};

CampaignSpec cpm(u64 id, i64 bid) {
  CampaignSpec spec;
  spec.id = id;
  spec.bid = bid;
  return spec;
}

CampaignSpec cpc(u64 id, i64 bid) {
  CampaignSpec spec;
  spec.id = id;
  spec.bid = bid;
  spec.bidType = "CPC";
  return spec;
}

}

TEST(ctr_starts_at_the_prior) {
  CtrTable table(1.f, 99.f);
  EXPECT_NEAR(table.lookup(1, 2, 3), 0.01, 1e-6);
}

TEST(ctr_learns_from_events) {
  CtrTable table(1.f, 1.f);
  Event event;
  event.campaignId = 1;
  event.slotId = 2;
  event.device = 3;
  event.type = EventType::Impression;
  table.update(event);
  table.update(event);
  event.type = EventType::Click;
  table.update(event);
  table.update(event);
  EXPECT_NEAR(table.lookup(1, 2, 3), 0.75, 1e-6);
}

TEST(ctr_keys_are_independent) {
  CtrTable table(1.f, 1.f);
  Event event;
  event.campaignId = 1;
  event.slotId = 2;
  event.device = 3;
  event.type = EventType::Impression;
  table.update(event);
  EXPECT_NEAR(table.lookup(1, 2, 3), 1.0 / 3.0, 1e-6);
  EXPECT_NEAR(table.lookup(9, 2, 3), 0.5, 1e-6);
  EXPECT_NEAR(table.lookup(1, 9, 3), 0.5, 1e-6);
  EXPECT_NEAR(table.lookup(1, 2, 9), 0.5, 1e-6);
}

TEST(ctr_never_exceeds_one) {
  CtrTable table(1.f, 1.f);
  Event click;
  click.campaignId = 1;
  click.type = EventType::Click;
  for (int i = 0; i < 100; ++i) table.update(click);
  EXPECT_LE(table.lookup(1, 0, 0), 1.0);
}

TEST(ctr_decay_shrinks_counts) {
  CtrTable table(1.f, 1.f);
  Event event;
  event.campaignId = 1;
  event.type = EventType::Impression;
  for (int i = 0; i < 10; ++i) table.update(event);
  table.decay(0.5f);
  EXPECT_NEAR(table.stat(1, 0, 0).impressions, 5.0, 1e-6);
}

TEST(bid_scorer_uses_a_cpm_bid_as_is) {
  const Campaign campaign = campaignOf(BidType::CPM, 2'000'000);
  Candidate c = candidateFor(campaign);
  BidScorer scorer;
  const float score = scorer.score(c, makeRequest(), UserContext{});
  EXPECT_EQ(c.ecpmMicros, 2'000'000);
  EXPECT_NEAR(score, 2'000'000.0, 1.0);
}

TEST(bid_scorer_converts_a_cpc_bid_to_cpm) {
  const Campaign campaign = campaignOf(BidType::CPC, 1000);
  Candidate c = candidateFor(campaign);
  BidScorer scorer(0.5f);
  scorer.score(c, makeRequest(), UserContext{});
  EXPECT_EQ(c.ecpmMicros, 500'000);
  EXPECT_NEAR(c.pctr, 0.5, 1e-6);
}

TEST(ctr_scorer_uses_the_learned_rate) {
  CtrTable table(1.f, 1.f);
  Event click;
  click.campaignId = 1;
  click.slotId = 7;
  click.device = 1;
  click.type = EventType::Click;
  table.update(click);
  table.update(click);
  // (2 clicks + 1) / (0 impressions + 2) is capped at 1.
  const Campaign campaign = campaignOf(BidType::CPC, 1000);
  Candidate c = candidateFor(campaign);
  CtrScorer scorer(table, 0.f, 0.01f);
  scorer.score(c, makeRequest(), UserContext{});
  EXPECT_NEAR(c.pctr, 1.0, 1e-6);
  EXPECT_EQ(c.ecpmMicros, 1'000'000);
}

TEST(ctr_scorer_exploration_draws_around_the_prior) {
  CtrTable table(1.f, 999.f);
  const Campaign campaign = campaignOf(BidType::CPC, 1000);
  CtrScorer scorer(table, 1.f, 0.01f);
  for (int i = 0; i < 1000; ++i) {
    Candidate c = candidateFor(campaign);
    scorer.score(c, makeRequest(), UserContext{});
    EXPECT_GE(c.pctr, 0.01f - 1e-6f);
    EXPECT_LE(c.pctr, 0.03f + 1e-6f);
  }
}

TEST(ctr_scorer_leaves_cpm_ecpm_unchanged_by_exploration) {
  CtrTable table(1.f, 1.f);
  const Campaign campaign = campaignOf(BidType::CPM, 700);
  Candidate c = candidateFor(campaign);
  CtrScorer scorer(table, 1.f, 0.5f);
  scorer.score(c, makeRequest(), UserContext{});
  EXPECT_EQ(c.ecpmMicros, 700);
}

TEST(first_price_charges_the_winning_bid) {
  FirstPriceAuction auction;
  const std::vector<Candidate> bids = {bidder(1, 300), bidder(2, 900), bidder(3, 500)};
  const AuctionResult r = auction.run(bids, 0);
  ASSERT_TRUE(r.winner.has_value());
  EXPECT_EQ(r.winner->campaignIdx, 2);
  EXPECT_EQ(r.priceMicros, 900);
}

TEST(first_price_honours_the_floor) {
  FirstPriceAuction auction;
  const std::vector<Candidate> bids = {bidder(1, 300)};
  const AuctionResult r = auction.run(bids, 301);
  EXPECT_FALSE(r.winner.has_value());
  EXPECT_EQ(r.noFillReason, "below_floor");
  EXPECT_TRUE(auction.run(bids, 300).winner.has_value());
}

TEST(first_price_breaks_ties_deterministically) {
  FirstPriceAuction auction;
  const std::vector<Candidate> bids = {bidder(5, 300), bidder(2, 300), bidder(9, 300)};
  EXPECT_EQ(auction.run(bids, 0).winner->campaignIdx, 2);
}

TEST(first_price_prefers_the_higher_bid_when_a_float_score_cannot_tell) {
  const std::vector<Candidate> bids = {bidder(1, 100'000'001), bidder(2, 100'000'002)};
  EXPECT_EQ(bids[0].score, bids[1].score);
  const AuctionResult r = FirstPriceAuction().run(bids, 0);
  EXPECT_EQ(r.winner->campaignIdx, 2);
  EXPECT_EQ(r.priceMicros, 100'000'002);
}

TEST(auctions_refuse_a_bid_worth_nothing) {
  const std::vector<Candidate> bids = {bidder(1, 0)};
  EXPECT_FALSE(FirstPriceAuction().run(bids, 0).winner.has_value());
  EXPECT_FALSE(SecondPriceAuction().run(bids, 0).winner.has_value());
}

TEST(first_price_with_no_candidates) {
  FirstPriceAuction auction;
  const AuctionResult r = auction.run({}, 0);
  EXPECT_FALSE(r.winner.has_value());
  EXPECT_EQ(r.noFillReason, "no_candidates");
}

TEST(second_price_charges_the_runner_up_bid) {
  SecondPriceAuction auction;
  const std::vector<Candidate> bids = {bidder(1, 300'000), bidder(2, 900'000), bidder(3, 500'000)};
  const AuctionResult r = auction.run(bids, 0);
  ASSERT_TRUE(r.winner.has_value());
  EXPECT_EQ(r.winner->campaignIdx, 2);
  EXPECT_EQ(r.priceMicros, 500'000);
}

TEST(second_price_result_does_not_depend_on_input_order) {
  SecondPriceAuction auction;
  std::vector<Candidate> bids = {bidder(1, 100'000), bidder(2, 400'000), bidder(3, 300'000), bidder(4, 200'000)};
  std::sort(bids.begin(), bids.end(),
            [](const Candidate& a, const Candidate& b) { return a.campaignIdx < b.campaignIdx; });
  do {
    const AuctionResult r = auction.run(bids, 0);
    ASSERT_TRUE(r.winner.has_value());
    EXPECT_EQ(r.winner->campaignIdx, 2);
    EXPECT_EQ(r.priceMicros, 300'000);
  } while (std::next_permutation(bids.begin(), bids.end(), [](const Candidate& a, const Candidate& b) {
    return a.campaignIdx < b.campaignIdx;
  }));
}

TEST(second_price_floor_raises_the_clearing_price) {
  SecondPriceAuction auction;
  const std::vector<Candidate> bids = {bidder(1, 1'000'000), bidder(2, 100'000)};
  EXPECT_EQ(auction.run(bids, 300'000).priceMicros, 300'000);
}

TEST(second_price_ignores_bids_below_the_floor_when_pricing) {
  SecondPriceAuction auction;
  const std::vector<Candidate> bids = {bidder(1, 1'000'000), bidder(2, 200'000)};
  const AuctionResult r = auction.run(bids, 500'000);
  EXPECT_EQ(r.winner->campaignIdx, 1);
  EXPECT_EQ(r.priceMicros, 500'000);
}

TEST(second_price_lone_bidder_pays_the_minimum) {
  SecondPriceAuction auction(TieBreakRule::HigherBid, 1000);
  const std::vector<Candidate> bids = {bidder(1, 1'000'000)};
  EXPECT_EQ(auction.run(bids, 0).priceMicros, 1000);
}

TEST(second_price_never_charges_above_the_winning_bid) {
  SecondPriceAuction auction(TieBreakRule::HigherBid, 1000);
  const std::vector<Candidate> bids = {bidder(1, 500)};
  EXPECT_EQ(auction.run(bids, 0).priceMicros, 500);
}

TEST(second_price_tied_bids_clear_at_that_bid) {
  SecondPriceAuction auction;
  const std::vector<Candidate> bids = {bidder(7, 500'000), bidder(3, 500'000)};
  const AuctionResult r = auction.run(bids, 0);
  EXPECT_EQ(r.winner->campaignIdx, 3);
  EXPECT_EQ(r.priceMicros, 500'000);
}

TEST(second_price_tie_break_rules_differ_when_scores_tie_but_bids_do_not) {
  Candidate lowIdx = bidder(1, 1000);
  Candidate highIdx = bidder(2, 2000);
  lowIdx.score = 5.f;
  highIdx.score = 5.f;
  const std::vector<Candidate> bids = {lowIdx, highIdx};
  EXPECT_EQ(SecondPriceAuction(TieBreakRule::HigherBid).run(bids, 0).winner->campaignIdx, 2);
  EXPECT_EQ(SecondPriceAuction(TieBreakRule::LowerCampaignIdx).run(bids, 0).winner->campaignIdx, 1);
}

// Above 16.7M micros a float cannot represent every integer, so bids that
// differ by a few micros score equal. The higher bid must still win.
TEST(second_price_separates_bids_a_float_score_cannot) {
  const std::vector<Candidate> bids = {bidder(1, 100'000'001), bidder(2, 100'000'002)};
  EXPECT_EQ(bids[0].score, bids[1].score);
  const AuctionResult r = SecondPriceAuction().run(bids, 0);
  EXPECT_EQ(r.winner->campaignIdx, 2);
  EXPECT_EQ(r.priceMicros, 100'000'001);
}

TEST(second_price_with_no_candidates_or_all_below_floor) {
  SecondPriceAuction auction;
  EXPECT_EQ(auction.run({}, 0).noFillReason, "no_candidates");
  const std::vector<Candidate> bids = {bidder(1, 100)};
  EXPECT_EQ(auction.run(bids, 200).noFillReason, "below_floor");
}

TEST(schedule_filter_window_includes_start_and_excludes_end) {
  Campaign campaign = campaignOf(BidType::CPM, 1);
  campaign.startTs = fromUnixMs(1000);
  campaign.endTs = fromUnixMs(2000);
  ScheduleFilter filter;
  const Candidate c = candidateFor(campaign);
  AdRequest req = makeRequest();
  for (const auto& [ts, expected] : std::vector<std::pair<i64, bool>>{{999, false}, {1000, true}, {1999, true}, {2000, false}}) {
    req.ts = fromUnixMs(ts);
    EXPECT_EQ(filter.apply(c, req, UserContext{}), expected);
  }
}

TEST(schedule_filter_checks_hour_of_day) {
  Campaign campaign = campaignOf(BidType::CPM, 1);
  campaign.targeting.hoursOfDay.reset();
  campaign.targeting.hoursOfDay.set(5);
  ScheduleFilter filter;
  const Candidate c = candidateFor(campaign);
  AdRequest req = makeRequest();
  req.ts = fromUnixMs(5 * kMsPerHour);
  EXPECT_TRUE(filter.apply(c, req, UserContext{}));
  req.ts = fromUnixMs(6 * kMsPerHour);
  EXPECT_FALSE(filter.apply(c, req, UserContext{}));
}

TEST(floor_filter_applies_only_to_cpm_bids) {
  FloorPriceFilter filter;
  const Campaign cpmCampaign = campaignOf(BidType::CPM, 100);
  const Campaign cpcCampaign = campaignOf(BidType::CPC, 1);
  AdRequest req = makeRequest();
  req.floorMicros = 100;
  EXPECT_TRUE(filter.apply(candidateFor(cpmCampaign), req, UserContext{}));
  req.floorMicros = 101;
  EXPECT_FALSE(filter.apply(candidateFor(cpmCampaign), req, UserContext{}));
  EXPECT_TRUE(filter.apply(candidateFor(cpcCampaign), req, UserContext{}));
}

TEST(budget_filter_needs_room_for_one_impression) {
  CampaignSlots slots(4);
  BudgetManager budget(slots, nullptr, 5);
  CampaignSpec spec;
  spec.id = 1;
  spec.bid = 5000;
  spec.daily = 5;
  const auto snap = buildSnapshot(slots, simpleCatalog({spec}));
  budget.applyCatalog(*snap);
  const u32 idx = slots.find(1).value();

  BudgetFilter filter(budget);
  const Candidate c = candidateFor(snap->campaign(idx), idx);
  EXPECT_TRUE(filter.apply(c, makeRequest(), UserContext{}));
  ASSERT_TRUE(budget.reserve(idx, 1));
  EXPECT_FALSE(filter.apply(c, makeRequest(), UserContext{}));
}

TEST(budget_filter_win_reserves_and_undo_releases) {
  CampaignSlots slots(4);
  BudgetManager budget(slots, nullptr, 5);
  CampaignSpec spec;
  spec.id = 1;
  spec.daily = 100;
  const auto snap = buildSnapshot(slots, simpleCatalog({spec}));
  budget.applyCatalog(*snap);
  const u32 idx = slots.find(1).value();

  BudgetFilter filter(budget);
  const Candidate c = candidateFor(snap->campaign(idx), idx);
  EXPECT_TRUE(filter.onWin(c, 60));
  EXPECT_EQ(budget.reserved(idx), 60);
  EXPECT_FALSE(filter.onWin(c, 60));
  EXPECT_EQ(budget.reserved(idx), 60);
  filter.undoWin(c, 60);
  EXPECT_EQ(budget.reserved(idx), 0);
}

TEST(frequency_filter_blocks_at_the_cap) {
  FrequencyCapStore store(nullptr, 1024, 3600);
  Campaign campaign = campaignOf(BidType::CPM, 1);
  campaign.id = 100;
  campaign.maxPerUserPerDay = 2;
  FrequencyCapFilter filter(store);
  const Candidate c = candidateFor(campaign);
  UserContext user;
  user.userId = 1;

  EXPECT_TRUE(filter.apply(c, makeRequest(), user));
  store.increment(1, 100);
  EXPECT_TRUE(filter.apply(c, makeRequest(), user));
  store.increment(1, 100);
  EXPECT_FALSE(filter.apply(c, makeRequest(), user));
}

TEST(frequency_filter_ignores_campaigns_with_no_cap) {
  FrequencyCapStore store(nullptr, 1024, 3600);
  Campaign campaign = campaignOf(BidType::CPM, 1);
  campaign.maxPerUserPerDay = 0;
  for (int i = 0; i < 5; ++i) store.increment(1, campaign.id);
  FrequencyCapFilter filter(store);
  UserContext user;
  user.userId = 1;
  EXPECT_TRUE(filter.apply(candidateFor(campaign), makeRequest(), user));
}

TEST(frequency_filter_prefetch_loads_counts_once) {
  FrequencyCapStore store(nullptr, 1024, 3600);
  Campaign capped = campaignOf(BidType::CPM, 1);
  capped.id = 100;
  capped.maxPerUserPerDay = 1;
  Campaign free = campaignOf(BidType::CPM, 1);
  free.id = 200;
  store.increment(1, 100);

  FrequencyCapFilter filter(store);
  UserContext user;
  user.userId = 1;
  const std::vector<Candidate> cands = {candidateFor(capped), candidateFor(free)};
  filter.prefetchBatch(cands, user);
  EXPECT_EQ(user.recentImpressions.size(), 1);
  EXPECT_EQ(*user.recentImpressions.find(100), 1);
  EXPECT_FALSE(filter.apply(cands[0], makeRequest(), user));
  EXPECT_TRUE(filter.apply(cands[1], makeRequest(), user));
}

TEST(pipeline_runs_filters_in_order_and_stops_when_empty) {
  MetricsRegistry metrics;
  Tracer tracer(true);
  std::vector<std::string> log;
  const auto keepAll = [](const Candidate&) { return true; };
  const auto dropAll = [](const Candidate&) { return false; };

  FilterPipeline pipeline(&metrics, &tracer);
  pipeline.add(std::make_unique<ScriptedFilter>("first", &log, keepAll));
  pipeline.add(std::make_unique<ScriptedFilter>("second", &log, dropAll));
  pipeline.add(std::make_unique<ScriptedFilter>("third", &log, keepAll));

  UserContext user;
  const AdRequest req = makeRequest();
  tracer.begin(1);
  const std::vector<Candidate> out = pipeline.run({bidder(1, 1), bidder(2, 2)}, req, user);
  const DebugTrace trace = tracer.end();

  EXPECT_TRUE(out.empty());
  const std::vector<std::string> expected = {"apply:first", "apply:first", "apply:second", "apply:second"};
  EXPECT_TRUE(log == expected);
  EXPECT_EQ(metrics.counterValue("ad_filter_drops_total", "filter=\"second\""), 2);
  EXPECT_EQ(metrics.counterValue("ad_filter_drops_total", "filter=\"first\""), 0);
  ASSERT_EQ(trace.drops.size(), 1);
  EXPECT_EQ(trace.drops[0].stage, "second");
}

TEST(pipeline_keeps_candidate_order_and_only_survivors) {
  std::vector<std::string> log;
  FilterPipeline pipeline(nullptr, nullptr);
  pipeline.add(std::make_unique<ScriptedFilter>("even", &log, [](const Candidate& c) { return c.campaignIdx % 2 == 0; }));
  UserContext user;
  const std::vector<Candidate> out =
      pipeline.run({bidder(1, 1), bidder(2, 2), bidder(3, 3), bidder(4, 4)}, makeRequest(), user);
  ASSERT_EQ(out.size(), 2);
  EXPECT_EQ(out[0].campaignIdx, 2);
  EXPECT_EQ(out[1].campaignIdx, 4);
}

TEST(pipeline_commit_offers_the_win_to_every_filter) {
  std::vector<std::string> log;
  const auto keepAll = [](const Candidate&) { return true; };
  FilterPipeline pipeline(nullptr, nullptr);
  pipeline.add(std::make_unique<ScriptedFilter>("a", &log, keepAll));
  pipeline.add(std::make_unique<ScriptedFilter>("b", &log, keepAll));
  EXPECT_TRUE(pipeline.commitWinner(bidder(1, 1), 5));
  const std::vector<std::string> expected = {"win:a", "win:b"};
  EXPECT_TRUE(log == expected);
}

TEST(pipeline_commit_undoes_earlier_filters_on_a_veto) {
  std::vector<std::string> log;
  const auto keepAll = [](const Candidate&) { return true; };
  FilterPipeline pipeline(nullptr, nullptr);
  pipeline.add(std::make_unique<ScriptedFilter>("a", &log, keepAll));
  pipeline.add(std::make_unique<ScriptedFilter>("b", &log, keepAll));
  pipeline.add(std::make_unique<ScriptedFilter>("veto", &log, keepAll, false));
  pipeline.add(std::make_unique<ScriptedFilter>("never", &log, keepAll));
  EXPECT_FALSE(pipeline.commitWinner(bidder(1, 1), 5));
  const std::vector<std::string> expected = {"win:a", "win:b", "win:veto", "undo:b", "undo:a"};
  EXPECT_TRUE(log == expected);
}

TEST(engine_reports_no_catalog_before_the_first_load) {
  EngineOptions options;
  options.load = false;
  EngineFixture f(simpleCatalog({cpm(1, 1'000'000)}), std::move(options));
  const AuctionResult r = f.engine->decide(makeRequest());
  EXPECT_FALSE(r.winner.has_value());
  EXPECT_EQ(r.noFillReason, "no_catalog");
}

TEST(engine_runs_a_second_price_auction_and_holds_the_cost) {
  EngineFixture f(simpleCatalog({cpm(1, 4'000'000), cpm(2, 2'500'000), cpm(3, 1'000'000)}));
  const AuctionResult r = f.engine->decide(makeRequest());
  ASSERT_TRUE(r.winner.has_value());
  EXPECT_EQ(r.winner->campaign->id, 1);
  EXPECT_EQ(r.priceMicros, 2'500'000);
  EXPECT_TRUE(r.noFillReason.empty());
  EXPECT_EQ(f.budget.reserved(f.idxOf(1)), 2500);
  EXPECT_EQ(f.budget.reserved(f.idxOf(2)), 0);
  EXPECT_EQ(f.metrics.counterValue("ad_requests_total"), 1);
  EXPECT_EQ(f.metrics.counterValue("ad_fills_total"), 1);
}

TEST(engine_reports_no_candidates) {
  EngineFixture f(simpleCatalog({cpm(1, 1'000'000)}));
  AdRequest req = makeRequest();
  req.slotSize = 999;
  const AuctionResult r = f.engine->decide(req);
  EXPECT_FALSE(r.winner.has_value());
  EXPECT_EQ(r.noFillReason, "no_candidates");
  EXPECT_EQ(f.metrics.counterValue("ad_no_fill_total", "reason=\"no_candidates\""), 1);
}

TEST(engine_reports_when_filters_remove_everyone) {
  CampaignSpec expired = cpm(1, 1'000'000);
  expired.end = kRequestTimeMs - 1;
  EngineFixture f(simpleCatalog({expired}));
  const AuctionResult r = f.engine->decide(makeRequest());
  EXPECT_EQ(r.noFillReason, "all_filtered");
  EXPECT_EQ(f.metrics.counterValue("ad_filter_drops_total", "filter=\"schedule\""), 1);
}

TEST(engine_excludes_exhausted_budgets) {
  CampaignSpec broke = cpm(1, 9'000'000);
  broke.daily = 0;
  EngineFixture f(simpleCatalog({broke, cpm(2, 1'000'000)}));
  const AuctionResult r = f.engine->decide(makeRequest());
  ASSERT_TRUE(r.winner.has_value());
  EXPECT_EQ(r.winner->campaign->id, 2);
}

TEST(engine_excludes_capped_users) {
  CampaignSpec capped = cpm(1, 9'000'000);
  capped.cap = 1;
  EngineFixture f(simpleCatalog({capped, cpm(2, 1'000'000)}));
  EXPECT_EQ(f.engine->decide(makeRequest(1)).winner->campaign->id, 1);
  f.freqCap.increment(42, 1);
  EXPECT_EQ(f.engine->decide(makeRequest(2)).winner->campaign->id, 2);
}

TEST(engine_cpc_bids_can_fall_below_the_floor_after_scoring) {
  EngineFixture f(simpleCatalog({cpc(1, 100'000)}));
  AdRequest req = makeRequest();
  req.floorMicros = 5'000'000;
  const AuctionResult r = f.engine->decide(req);
  EXPECT_FALSE(r.winner.has_value());
  EXPECT_EQ(r.noFillReason, "below_floor");
}

TEST(engine_learned_click_rates_change_the_winner) {
  EngineFixture f(simpleCatalog({cpc(1, 100'000), cpc(2, 100'000)}));
  const AdRequest req = makeRequest();
  Event click;
  click.campaignId = 2;
  click.slotId = req.slotId;
  click.device = req.device;
  click.type = EventType::Click;
  for (int i = 0; i < 50; ++i) f.ctr.update(click);

  const AuctionResult r = f.engine->decide(req);
  ASSERT_TRUE(r.winner.has_value());
  EXPECT_EQ(r.winner->campaign->id, 2);
  EXPECT_LT(r.priceMicros, r.winner->ecpmMicros);
}

TEST(engine_keeps_the_true_top_bids_among_many) {
  std::vector<CampaignSpec> specs;
  for (u64 id = 1; id <= 200; ++id) specs.push_back(cpm(id, static_cast<i64>((id * 37) % 211 + 1) * 10'000));
  i64 best = 0;
  i64 second = 0;
  u64 bestId = 0;
  for (const CampaignSpec& s : specs) {
    if (s.bid > best) {
      second = best;
      best = s.bid;
      bestId = s.id;
    } else if (s.bid > second) {
      second = s.bid;
    }
  }
  EngineFixture f(simpleCatalog(specs));
  const AuctionResult r = f.engine->decide(makeRequest());
  ASSERT_TRUE(r.winner.has_value());
  EXPECT_EQ(r.winner->campaign->id, bestId);
  EXPECT_EQ(r.priceMicros, second);
}

// All of these round to the same float score, so only the bid can rank them.
TEST(engine_top_k_keeps_the_highest_bid_among_equal_scores) {
  std::vector<CampaignSpec> specs;
  for (u64 k = 0; k < 40; ++k) specs.push_back(cpm(k + 1, 1'000'000'000 - 8 + static_cast<i64>(k)));
  EngineFixture f(simpleCatalog(specs));
  const AuctionResult r = f.engine->decide(makeRequest());
  ASSERT_TRUE(r.winner.has_value());
  EXPECT_EQ(r.winner->ecpmMicros, 1'000'000'031);
  EXPECT_EQ(r.priceMicros, 1'000'000'030);
}

TEST(engine_reruns_the_auction_when_the_winner_cannot_reserve) {
  EngineOptions options;
  options.extraFilter = std::make_unique<VetoFilter>(std::set<u64>{1});
  EngineFixture f(simpleCatalog({cpm(1, 4'000'000), cpm(2, 2'000'000), cpm(3, 1'000'000)}), std::move(options));
  const AuctionResult r = f.engine->decide(makeRequest());
  ASSERT_TRUE(r.winner.has_value());
  EXPECT_EQ(r.winner->campaign->id, 2);
  EXPECT_EQ(r.priceMicros, 1'000'000);
  EXPECT_EQ(f.budget.reserved(f.idxOf(1)), 0);
  EXPECT_EQ(f.budget.reserved(f.idxOf(2)), 1000);
}

TEST(engine_gives_up_after_repeated_reservation_failures) {
  EngineOptions options;
  options.extraFilter = std::make_unique<VetoFilter>(std::set<u64>{1, 2, 3, 4});
  EngineFixture f(simpleCatalog({cpm(1, 4'000'000), cpm(2, 3'000'000), cpm(3, 2'000'000), cpm(4, 1'000'000)}),
                  std::move(options));
  const AuctionResult r = f.engine->decide(makeRequest());
  EXPECT_FALSE(r.winner.has_value());
  EXPECT_EQ(r.noFillReason, "reservation_failed");
  for (u64 id = 1; id <= 4; ++id) EXPECT_EQ(f.budget.reserved(f.idxOf(id)), 0);
}

TEST(engine_gives_up_at_the_deadline_without_holding_budget) {
  EngineOptions options;
  options.deadlineMs = 5;
  options.extraFilter = std::make_unique<SlowFilter>();
  EngineFixture f(simpleCatalog({cpm(1, 1'000'000)}), std::move(options));
  const AuctionResult r = f.engine->decide(makeRequest());
  EXPECT_FALSE(r.winner.has_value());
  EXPECT_EQ(r.noFillReason, "deadline_exceeded");
  EXPECT_EQ(f.budget.reserved(f.idxOf(1)), 0);
}

TEST(engine_rotates_only_through_creatives_that_fit) {
  CampaignSpec spec = cpm(1, 1'000'000);
  CreativeSpec first, second, wrongSize;
  first.id = 11;
  first.campaign = 1;
  second.id = 12;
  second.campaign = 1;
  wrongSize.id = 13;
  wrongSize.campaign = 1;
  wrongSize.size = 1234;
  EngineFixture f(catalogJson({spec}, {first, wrongSize, second}));

  std::set<u64> seen;
  for (u64 id = 1; id <= 8; ++id) seen.insert(f.engine->decide(makeRequest(id)).winner->creativeId);
  const std::set<u64> expected = {11, 12};
  EXPECT_TRUE(seen == expected);
}

TEST(engine_result_stays_valid_after_a_catalog_swap) {
  EngineFixture f(simpleCatalog({cpm(1, 1'000'000)}));
  const AuctionResult r = f.engine->decide(makeRequest());
  ASSERT_TRUE(r.winner.has_value());
  const u64 version = r.snapshot->version;

  f.db->json = simpleCatalog({});
  f.snapshots.refreshNow();

  EXPECT_NE(f.snapshots.get()->version, version);
  EXPECT_EQ(r.winner->campaign->id, 1);
  EXPECT_EQ(r.snapshot->creativesFor(r.winner->campaignIdx).size(), 1);
}

TEST(engine_returns_a_trace_of_what_was_dropped) {
  CampaignSpec expired = cpm(2, 9'000'000);
  expired.end = kRequestTimeMs - 1;
  EngineFixture f(simpleCatalog({cpm(1, 1'000'000), expired}));
  const AuctionResult r = f.engine->decide(makeRequest(55));
  EXPECT_EQ(r.trace.id, 55);
  ASSERT_EQ(r.trace.drops.size(), 1);
  EXPECT_EQ(r.trace.drops[0].stage, "schedule");
}

TEST(engine_records_latency_for_each_stage) {
  EngineFixture f(simpleCatalog({cpm(1, 1'000'000)}));
  f.engine->decide(makeRequest());
  const std::string text = f.metrics.expose();
  for (const char* stage : {"user_context", "retrieve", "filter", "score", "auction"}) {
    EXPECT_NE(text.find("ad_stage_latency_ms_count{stage=\"" + std::string(stage) + "\"} 1\n"), std::string::npos);
  }
}
