#pragma once

#include <string>
#include <string_view>

#include "common/crypto.h"
#include "common/json.h"
#include "core/decision_engine.h"
#include "server/http_server.h"

namespace ad {

class RequestParser {
 public:
  // Throws JsonError on a missing or mistyped field. The timestamp always comes
  // from the server clock, so a client cannot shift a campaign's schedule.
  AdRequest parse(const Json& body) const;
};

class AdRequestHandler {
 public:
  AdRequestHandler(DecisionEngine& engine, const UrlSigner& signer, MetricsRegistry& metrics,
                   std::string publicBaseUrl, int urlTtlSec)
      : engine_(engine),
        signer_(signer),
        metrics_(metrics),
        publicBaseUrl_(std::move(publicBaseUrl)),
        urlTtlSec_(urlTtlSec) {}

  HttpResponse handleAdRequest(const HttpRequest& http);

 private:
  AdRequest parseAdRequest(const Json& body) const { return parser_.parse(body); }
  Status validate(const AdRequest& req) const;
  Json buildResponse(const AdRequest& req, const AuctionResult& res, bool includeTrace) const;
  std::string signTrackingUrl(const AdRequest& req, const AuctionResult& res, EventType type,
                              std::string_view landingUrl) const;

  DecisionEngine& engine_;
  RequestParser parser_;
  const UrlSigner& signer_;
  MetricsRegistry& metrics_;
  std::string publicBaseUrl_;
  int urlTtlSec_;
};

}
