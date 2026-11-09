#include "server/ad_request_handler.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "common/url.h"

namespace ad {
namespace {

constexpr std::size_t kMaxPageUrl = 2048;

u32 requiredU32(const Json& body, std::string_view key) {
  const u64 value = body.at(key).asU64();
  if (value > std::numeric_limits<u32>::max()) {
    throw JsonError("field '" + std::string(key) + "' is out of range");
  }
  return static_cast<u32>(value);
}

u32 optionalU32(const Json& body, std::string_view key) {
  return body.find(key) != nullptr ? requiredU32(body, key) : 0;
}

HttpResponse errorResponse(int status, const std::string& message) {
  Json out;
  out.set("error", message);
  return HttpResponse::json(status, out.dump());
}

// Second-price per click: the CPM clearing price spread over expected clicks.
i64 clickChargeMicros(const Candidate& winner, i64 priceMicros) {
  if (winner.pctr <= 0.f) return 0;
  const auto charge = static_cast<i64>(
      std::llround(static_cast<double>(priceMicros) / (1000.0 * static_cast<double>(winner.pctr))));
  return std::min(charge, winner.bidMicros);
}

}

AdRequest RequestParser::parse(const Json& body) const {
  AdRequest req;
  req.requestId = body.at("requestId").asU64();
  req.userId = body.at("userId").asU64();
  req.slotId = requiredU32(body, "slotId");
  req.slotSize = requiredU32(body, "slotSize");
  req.geo = optionalU32(body, "geo");
  req.device = optionalU32(body, "device");
  req.os = optionalU32(body, "os");
  req.language = optionalU32(body, "language");
  if (const Json* url = body.find("pageUrl")) req.pageUrl = url->asString();
  if (const Json* floor = body.find("floorMicros")) req.floorMicros = floor->asI64();
  req.ts = nowTime();
  return req;
}

Status AdRequestHandler::validate(const AdRequest& req) const {
  if (req.requestId == 0) return Status::error("requestId must be non-zero");
  if (req.userId == 0) return Status::error("userId must be non-zero");
  if (req.slotSize == 0) return Status::error("slotSize must be non-zero");
  if (req.floorMicros < 0) return Status::error("floorMicros must not be negative");
  if (req.pageUrl.size() > kMaxPageUrl) return Status::error("pageUrl is too long");
  return Status::ok();
}

std::string AdRequestHandler::signTrackingUrl(const AdRequest& req, const AuctionResult& res,
                                              EventType type, std::string_view landingUrl) const {
  const Candidate& winner = *res.winner;
  const bool isCpm = winner.campaign->bidType == BidType::CPM;
  const i64 impressionCost = impressionCostMicros(res.priceMicros);

  i64 price = 0;
  i64 reserved = 0;
  if (type == EventType::Impression) {
    price = isCpm ? impressionCost : 0;
    reserved = impressionCost;
  } else if (!isCpm) {
    price = clickChargeMicros(winner, res.priceMicros);
  }

  std::string query;
  query.reserve(160 + landingUrl.size());
  query += "rid=" + std::to_string(req.requestId);
  query += "&cid=" + std::to_string(winner.campaign->id);
  query += "&uid=" + std::to_string(req.userId);
  query += "&slot=" + std::to_string(req.slotId);
  query += "&dev=" + std::to_string(req.device);
  query += "&price=" + std::to_string(price);
  query += "&rsv=" + std::to_string(reserved);
  query += "&exp=" + std::to_string(toUnixMs(req.ts) + i64{urlTtlSec_} * 1000);
  if (type == EventType::Click) query += "&to=" + percentEncode(landingUrl);

  const std::string_view path = type == EventType::Impression ? "/impression" : "/click";
  return publicBaseUrl_ + signer_.sign(path, query);
}

Json AdRequestHandler::buildResponse(const AdRequest& req, const AuctionResult& res,
                                     bool includeTrace) const {
  Json out;
  out.set("requestId", req.requestId);

  const Creative* creative = nullptr;
  if (res.winner && res.snapshot) {
    for (const Creative& c : res.snapshot->creativesFor(res.winner->campaignIdx)) {
      if (c.id == res.winner->creativeId) creative = &c;
    }
  }
  if (creative == nullptr) {
    out.set("fill", false);
    out.set("reason", res.noFillReason.empty() ? "creative_missing" : res.noFillReason);
  } else {
    out.set("fill", true);
    out.set("campaignId", res.winner->campaign->id);
    out.set("creativeId", creative->id);
    out.set("priceMicros", res.priceMicros);
    out.set("assetUrl", creative->assetUrl);
    out.set("impressionUrl", signTrackingUrl(req, res, EventType::Impression, {}));
    out.set("clickUrl", signTrackingUrl(req, res, EventType::Click, creative->clickUrl));
  }

  if (includeTrace) {
    Json::Array drops;
    for (const StageDrop& drop : res.trace.drops) {
      Json item;
      item.set("stage", drop.stage).set("reason", drop.reason);
      drops.push_back(std::move(item));
    }
    out.set("trace", Json(std::move(drops)));
  }
  return out;
}

HttpResponse AdRequestHandler::handleAdRequest(const HttpRequest& http) {
  if (http.method != "POST") return errorResponse(405, "POST required");

  AdRequest req;
  bool includeTrace = false;
  try {
    const Json body = Json::parse(http.body);
    req = parseAdRequest(body);
    if (const Json* debug = body.find("debug")) includeTrace = debug->asBool();
  } catch (const JsonError& e) {
    metrics_.inc("ad_bad_requests_total");
    return errorResponse(400, e.what());
  }
  if (const Status status = validate(req); !status.isOk()) {
    metrics_.inc("ad_bad_requests_total");
    return errorResponse(400, status.message());
  }

  const AuctionResult result = engine_.decide(req);
  return HttpResponse::json(200, buildResponse(req, result, includeTrace).dump());
}

}
