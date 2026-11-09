#include "pipeline/pipeline.h"

#include <algorithm>
#include <string>
#include <utility>

namespace ad {

bool ScheduleFilter::apply(const Candidate& c, const AdRequest& req, const UserContext&) {
  const Campaign& campaign = *c.campaign;
  return req.ts >= campaign.startTs && req.ts < campaign.endTs &&
         campaign.targeting.hoursOfDay.test(static_cast<std::size_t>(utcHour(req.ts)));
}

bool BudgetFilter::apply(const Candidate& c, const AdRequest&, const UserContext&) {
  const i64 minimum = c.campaign->bidType == BidType::CPM
                          ? std::max<i64>(impressionCostMicros(c.bidMicros), 1)
                          : 1;
  return budget_.remaining(c.campaignIdx) >= minimum;
}

bool BudgetFilter::onWin(const Candidate& c, i64 costMicros) {
  return budget_.reserve(c.campaignIdx, costMicros);
}

void BudgetFilter::undoWin(const Candidate& c, i64 costMicros) {
  budget_.release(c.campaignIdx, costMicros);
}

bool FloorPriceFilter::apply(const Candidate& c, const AdRequest& req, const UserContext&) {
  return c.campaign->bidType != BidType::CPM || c.bidMicros >= req.floorMicros;
}

bool PacingFilter::apply(const Candidate& c, const AdRequest& req, const UserContext&) {
  return pacing_.allowed(c.campaignIdx, req.ts);
}

bool PacingFilter::onWin(const Candidate& c, i64 costMicros) {
  pacing_.consume(c.campaignIdx, costMicros);
  return true;
}

void PacingFilter::undoWin(const Candidate& c, i64 costMicros) {
  pacing_.consume(c.campaignIdx, -costMicros);
}

void FrequencyCapFilter::prepare(std::span<const Candidate> cands, const AdRequest&, UserContext& user) {
  prefetchBatch(cands, user);
}

void FrequencyCapFilter::prefetchBatch(std::span<const Candidate> cands, UserContext& user) {
  std::vector<u64> campaigns;
  campaigns.reserve(cands.size());
  for (const Candidate& c : cands) {
    if (c.campaign->maxPerUserPerDay > 0) campaigns.push_back(c.campaign->id);
  }
  if (campaigns.empty()) return;

  const std::vector<u16> counts = freqCap_.countBatch(user.userId, campaigns);
  std::vector<std::pair<u64, u16>> items;
  items.reserve(campaigns.size());
  for (std::size_t i = 0; i < campaigns.size(); ++i) items.emplace_back(campaigns[i], counts[i]);
  user.recentImpressions.assign(std::move(items));
}

bool FrequencyCapFilter::apply(const Candidate& c, const AdRequest&, const UserContext& user) {
  const u16 cap = c.campaign->maxPerUserPerDay;
  if (cap == 0) return true;
  const u16* known = user.recentImpressions.find(c.campaign->id);
  const u16 seen = known != nullptr ? *known : freqCap_.count(user.userId, c.campaign->id);
  return seen < cap;
}

std::vector<Candidate> FilterPipeline::run(std::vector<Candidate> cands, const AdRequest& req,
                                           UserContext& user) {
  for (const std::unique_ptr<Filter>& filter : filters_) {
    if (cands.empty()) break;
    filter->prepare(cands, req, user);
    const std::size_t before = cands.size();
    std::erase_if(cands, [&](const Candidate& c) { return !filter->apply(c, req, user); });
    recordDrop(*filter, before - cands.size());
  }
  return cands;
}

bool FilterPipeline::commitWinner(const Candidate& winner, i64 costMicros) {
  std::size_t accepted = 0;
  for (; accepted < filters_.size(); ++accepted) {
    if (!filters_[accepted]->onWin(winner, costMicros)) break;
  }
  if (accepted == filters_.size()) return true;
  while (accepted-- > 0) filters_[accepted]->undoWin(winner, costMicros);
  return false;
}

void FilterPipeline::recordDrop(const Filter& filter, std::size_t count) {
  if (count == 0) return;
  if (metrics_ != nullptr) {
    const std::string labels = "filter=\"" + std::string(filter.name()) + "\"";
    metrics_->inc("ad_filter_drops_total", labels, count);
  }
  if (tracer_ != nullptr) tracer_->stageDrop(filter.name(), std::to_string(count) + " dropped");
}

std::vector<Candidate> CandidateRetriever::retrieve(const CatalogSnapshot& snap, const AdRequest& req,
                                                    const UserContext& user) const {
  thread_local std::vector<u32> scratch;
  scratch.clear();
  snap.index.query(req, user).appendTo(scratch);

  std::vector<Candidate> out;
  out.reserve(scratch.size());
  for (u32 idx : scratch) {
    const Campaign& campaign = snap.campaign(idx);
    const std::span<const Creative> creatives = snap.creativesFor(idx);

    // Rotate across the creatives that fit, using the request id as the seed.
    std::size_t fitting = 0;
    for (const Creative& creative : creatives) fitting += creative.size == req.slotSize ? 1 : 0;
    if (fitting == 0) continue;
    std::size_t skip = static_cast<std::size_t>(req.requestId % fitting);
    for (const Creative& creative : creatives) {
      if (creative.size != req.slotSize) continue;
      if (skip-- != 0) continue;
      Candidate c;
      c.campaignIdx = idx;
      c.creativeId = creative.id;
      c.bidMicros = campaign.bidMicros;
      c.campaign = &campaign;
      out.push_back(c);
      break;
    }
  }
  return out;
}

}
