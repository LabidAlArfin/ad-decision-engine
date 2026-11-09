#pragma once

#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "catalog/catalog.h"
#include "domain/model.h"
#include "observability/observability.h"
#include "state/runtime_state.h"

namespace ad {

class Filter {
 public:
  virtual ~Filter() = default;
  virtual bool apply(const Candidate& c, const AdRequest& req, const UserContext& user) = 0;
  virtual std::string_view name() const = 0;

  // prepare: runs once on the survivors before apply, for batched lookups.
  virtual void prepare(std::span<const Candidate>, const AdRequest&, UserContext&) {}
  // onWin: the candidate won at this cost. False vetoes the win. Spending
  // happens here because a filter that spent in apply() would charge losers.
  virtual bool onWin(const Candidate&, i64 /*costMicros*/) { return true; }
  virtual void undoWin(const Candidate&, i64 /*costMicros*/) {}
};

class ScheduleFilter : public Filter {
 public:
  bool apply(const Candidate& c, const AdRequest& req, const UserContext& user) override;
  std::string_view name() const override { return "schedule"; }
};

class BudgetFilter : public Filter {
 public:
  explicit BudgetFilter(BudgetManager& budget) : budget_(budget) {}
  bool apply(const Candidate& c, const AdRequest& req, const UserContext& user) override;
  std::string_view name() const override { return "budget"; }
  bool onWin(const Candidate& c, i64 costMicros) override;
  void undoWin(const Candidate& c, i64 costMicros) override;

 private:
  BudgetManager& budget_;
};

// CPC bids are only comparable to the floor after scoring, so those pass here
// and are checked by the Auction instead.
class FloorPriceFilter : public Filter {
 public:
  bool apply(const Candidate& c, const AdRequest& req, const UserContext& user) override;
  std::string_view name() const override { return "floor_price"; }
};

class PacingFilter : public Filter {
 public:
  explicit PacingFilter(PacingController& pacing) : pacing_(pacing) {}
  bool apply(const Candidate& c, const AdRequest& req, const UserContext& user) override;
  std::string_view name() const override { return "pacing"; }
  bool onWin(const Candidate& c, i64 costMicros) override;
  void undoWin(const Candidate& c, i64 costMicros) override;

 private:
  PacingController& pacing_;
};

class FrequencyCapFilter : public Filter {
 public:
  explicit FrequencyCapFilter(FrequencyCapStore& freqCap) : freqCap_(freqCap) {}
  bool apply(const Candidate& c, const AdRequest& req, const UserContext& user) override;
  std::string_view name() const override { return "frequency_cap"; }
  void prepare(std::span<const Candidate> cands, const AdRequest& req, UserContext& user) override;

  // Loads all counts in one round trip into user.recentImpressions.
  void prefetchBatch(std::span<const Candidate> cands, UserContext& user);

 private:
  FrequencyCapStore& freqCap_;
};

class FilterPipeline {
 public:
  FilterPipeline(MetricsRegistry* metrics, Tracer* tracer) : metrics_(metrics), tracer_(tracer) {}

  // Filters run in the order added; put the cheap ones first.
  void add(std::unique_ptr<Filter> filter) { filters_.push_back(std::move(filter)); }

  std::vector<Candidate> run(std::vector<Candidate> cands, const AdRequest& req, UserContext& user);

  // Offers the win to every filter, undoing all of them if any one vetoes.
  bool commitWinner(const Candidate& winner, i64 costMicros);

 private:
  void recordDrop(const Filter& filter, std::size_t count);

  std::vector<std::unique_ptr<Filter>> filters_;
  MetricsRegistry* metrics_;
  Tracer* tracer_;
};

class CandidateRetriever {
 public:
  std::vector<Candidate> retrieve(const CatalogSnapshot& snap, const AdRequest& req,
                                  const UserContext& user) const;
};

}
