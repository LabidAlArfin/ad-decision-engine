#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "catalog/catalog.h"
#include "domain/model.h"
#include "observability/observability.h"
#include "pipeline/pipeline.h"
#include "scoring/scoring.h"
#include "state/runtime_state.h"

namespace ad {

class DecisionEngine {
 public:
  DecisionEngine(SnapshotManager& snapshots, CandidateRetriever retriever, FilterPipeline pipeline,
                 std::unique_ptr<Scorer> scorer, std::unique_ptr<Auction> auction,
                 UserProfileStore& users, int deadlineMs, Tracer& tracer, MetricsRegistry& metrics);

  AuctionResult decide(const AdRequest& req);

 private:
  UserContext buildUserContext(const AdRequest& req);
  static std::vector<Candidate> topK(std::vector<Candidate> scored, std::size_t k);
  AuctionResult fallback(std::string_view reason);

  SnapshotManager& snapshots_;
  CandidateRetriever retriever_;
  FilterPipeline pipeline_;
  std::unique_ptr<Scorer> scorer_;
  std::unique_ptr<Auction> auction_;
  UserProfileStore& users_;
  int deadlineMs_;
  Tracer& tracer_;
  MetricsRegistry& metrics_;
};

}
