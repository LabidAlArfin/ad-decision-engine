#include "core/decision_engine.h"

#include <algorithm>
#include <chrono>
#include <string>

namespace ad {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kTopK = 32;
constexpr int kMaxCommitAttempts = 3;

}

DecisionEngine::DecisionEngine(SnapshotManager& snapshots, CandidateRetriever retriever,
                               FilterPipeline pipeline, std::unique_ptr<Scorer> scorer,
                               std::unique_ptr<Auction> auction, UserProfileStore& users,
                               int deadlineMs, Tracer& tracer, MetricsRegistry& metrics)
    : snapshots_(snapshots),
      retriever_(retriever),
      pipeline_(std::move(pipeline)),
      scorer_(std::move(scorer)),
      auction_(std::move(auction)),
      users_(users),
      deadlineMs_(deadlineMs),
      tracer_(tracer),
      metrics_(metrics) {}

UserContext DecisionEngine::buildUserContext(const AdRequest& req) {
  UserContext user;
  user.userId = req.userId;
  user.segments = users_.segments(req.userId);
  return user;
}

std::vector<Candidate> DecisionEngine::topK(std::vector<Candidate> scored, std::size_t k) {
  if (scored.size() > k) {
    std::nth_element(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(k), scored.end(),
                     [](const Candidate& a, const Candidate& b) {
                       // Bids a float score cannot separate must not be cut at random.
                       return a.score != b.score ? a.score > b.score : a.ecpmMicros > b.ecpmMicros;
                     });
    scored.resize(k);
  }
  return scored;
}

AuctionResult DecisionEngine::fallback(std::string_view reason) {
  tracer_.stageDrop("decide", reason);
  metrics_.inc("ad_no_fill_total", "reason=\"" + std::string(reason) + "\"");
  AuctionResult result;
  result.noFillReason = std::string(reason);
  result.trace = tracer_.end();
  return result;
}

AuctionResult DecisionEngine::decide(const AdRequest& req) {
  const auto start = Clock::now();
  const auto deadline = start + std::chrono::milliseconds(deadlineMs_);
  auto stageStart = start;
  const auto lap = [&](std::string_view stage) {
    const auto now = Clock::now();
    metrics_.observeLatency(stage, std::chrono::duration<double, std::milli>(now - stageStart).count());
    stageStart = now;
  };

  metrics_.inc("ad_requests_total");
  tracer_.begin(req.requestId);

  // Holding this pointer keeps the catalog alive for the whole request, even
  // if a refresh swaps in a newer one meanwhile.
  const std::shared_ptr<const CatalogSnapshot> snapshot = snapshots_.get();
  if (!snapshot) return fallback("no_catalog");

  UserContext user = buildUserContext(req);
  lap("user_context");

  std::vector<Candidate> candidates = retriever_.retrieve(*snapshot, req, user);
  lap("retrieve");
  if (candidates.empty()) return fallback("no_candidates");
  if (Clock::now() > deadline) return fallback("deadline_exceeded");

  candidates = pipeline_.run(std::move(candidates), req, user);
  lap("filter");
  if (candidates.empty()) return fallback("all_filtered");
  if (Clock::now() > deadline) return fallback("deadline_exceeded");

  for (Candidate& c : candidates) c.score = scorer_->score(c, req, user);
  std::vector<Candidate> ranked = topK(std::move(candidates), kTopK);
  lap("score");

  // Checked last, before anything is reserved: a late answer is thrown away by
  // the caller, and its budget hold would otherwise sit there until it expired.
  if (Clock::now() > deadline) return fallback("deadline_exceeded");

  for (int attempt = 0; attempt < kMaxCommitAttempts; ++attempt) {
    AuctionResult result = auction_->run(ranked, req.floorMicros);
    if (!result.winner) return fallback(result.noFillReason);

    if (pipeline_.commitWinner(*result.winner, impressionCostMicros(result.priceMicros))) {
      lap("auction");
      metrics_.inc("ad_fills_total");
      result.trace = tracer_.end();
      result.snapshot = snapshot;
      return result;
    }
    // Another request took the last of this campaign's budget. Rerun without it.
    tracer_.stageDrop("commit", "winner_could_not_reserve");
    const u32 lost = result.winner->campaignIdx;
    std::erase_if(ranked, [lost](const Candidate& c) { return c.campaignIdx == lost; });
  }
  return fallback("reservation_failed");
}

}
