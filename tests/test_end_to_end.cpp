#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "common/json.h"
#include "common/net.h"
#include "dev/mini_redis.h"
#include "test_framework.h"
#include "test_helpers.h"

using namespace ad;
using namespace adtest;

namespace {

struct Reply {
  int status = 0;
  std::map<std::string, std::string> headers;  // names are lowercase
  std::string body;
};

std::vector<Reply> parseReplies(std::string_view raw) {
  std::vector<Reply> out;
  while (!raw.empty()) {
    const std::size_t headEnd = raw.find("\r\n\r\n");
    if (headEnd == std::string_view::npos) break;
    std::string_view head = raw.substr(0, headEnd);
    raw.remove_prefix(headEnd + 4);

    Reply reply;
    const std::size_t lineEnd = head.find("\r\n");
    const std::string_view statusLine = head.substr(0, lineEnd);
    reply.status = std::stoi(std::string(statusLine.substr(9, 3)));
    head = lineEnd == std::string_view::npos ? std::string_view{} : head.substr(lineEnd + 2);
    while (!head.empty()) {
      const std::size_t end = head.find("\r\n");
      const std::string_view line = head.substr(0, end);
      head = end == std::string_view::npos ? std::string_view{} : head.substr(end + 2);
      const std::size_t colon = line.find(':');
      std::string name(line.substr(0, colon));
      for (char& c : name) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
      reply.headers[name] = std::string(line.substr(colon + 2));
    }
    const auto length = reply.headers.find("content-length");
    const std::size_t bodySize = length == reply.headers.end() ? 0 : std::stoul(length->second);
    reply.body = std::string(raw.substr(0, bodySize));
    raw.remove_prefix(std::min(bodySize, raw.size()));
    out.push_back(std::move(reply));
  }
  return out;
}

std::string roundTrip(u16 port, std::string_view wire, int timeoutMs = 15'000) {
  net::Socket socket = net::Socket::connectTcp("127.0.0.1", port, timeoutMs);
  if (!socket.valid() || !socket.sendAll(wire, timeoutMs)) return {};
  std::string out;
  char chunk[4096];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) break;
    const int n = socket.recvSome(chunk, sizeof chunk, static_cast<int>(left));
    if (n <= 0) break;
    out.append(chunk, static_cast<std::size_t>(n));
  }
  return out;
}

Reply call(u16 port, std::string_view method, std::string_view target, std::string_view body = {}) {
  std::string wire;
  wire.append(method).append(" ").append(target).append(" HTTP/1.1\r\nHost: test\r\nConnection: close\r\n");
  wire.append("Content-Length: ").append(std::to_string(body.size())).append("\r\n\r\n").append(body);
  const std::vector<Reply> replies = parseReplies(roundTrip(port, wire));
  return replies.empty() ? Reply{} : replies[0];
}

std::string adBody(u64 requestId, i64 floor = 0, u64 userId = 42, u32 size = kSlotSize) {
  return "{\"requestId\":" + std::to_string(requestId) + ",\"userId\":" + std::to_string(userId) +
         ",\"slotId\":7,\"slotSize\":" + std::to_string(size) +
         ",\"geo\":840,\"device\":1,\"os\":1,\"language\":1,\"floorMicros\":" + std::to_string(floor) + "}";
}

Json adRequest(u16 port, u64 requestId, i64 floor = 0, u64 userId = 42) {
  const Reply reply = call(port, "POST", "/ad-request", adBody(requestId, floor, userId));
  if (reply.status != 200) throw std::runtime_error("ad request failed: " + std::to_string(reply.status));
  return Json::parse(reply.body);
}

std::atomic<int> gCatalogCounter{0};

struct Harness {
  MiniRedis redis;
  std::string catalogPath;
  std::unique_ptr<App> app;

  explicit Harness(const std::string& catalog, const std::function<void(AppConfig&)>& tweak = {}) {
    if (!redis.start()) throw std::runtime_error("cannot start redis");
    catalogPath = std::string(AD_TEST_DATA_DIR) + "/e2e_catalog_" + std::to_string(gCatalogCounter++) + ".json";
    writeFile(catalogPath, catalog);
    app = std::make_unique<App>(config(tweak));
    app->start();
  }

  AppConfig config(const std::function<void(AppConfig&)>& tweak = {}) const {
    AppConfig c;
    c.port = 0;
    c.workerThreads = 4;
    c.catalogPath = catalogPath;
    c.redisPort = redis.port();
    c.redisTimeoutMs = 500;
    c.hmacSecret = "test-secret";
    c.catalogRefreshSec = 3600;
    c.deadlineMs = 10'000;
    c.epsilon = 0.f;
    if (tweak) tweak(c);
    return c;
  }

  u16 port() const { return app->port(); }

  u32 idxOf(u64 campaignId) const {
    const auto snapshot = app->snapshots().get();
    return snapshot->idToIdx.at(campaignId);
  }
};

CampaignSpec cpm(u64 id, i64 bid) {
  CampaignSpec spec;
  spec.id = id;
  spec.bid = bid;
  return spec;
}

std::string twoCampaigns() { return simpleCatalog({cpm(1, 4'000'000), cpm(2, 2'000'000)}); }

}

TEST(e2e_full_request_impression_click_loop) {
  Harness h(twoCampaigns());
  const u32 idx = h.idxOf(1);

  // Request: the $4 CPM bid wins and pays the $2 runner-up price.
  const Json ad = adRequest(h.port(), 1001);
  ASSERT_TRUE(ad.at("fill").asBool());
  EXPECT_EQ(ad.at("requestId").asU64(), 1001);
  EXPECT_EQ(ad.at("campaignId").asU64(), 1);
  EXPECT_EQ(ad.at("creativeId").asU64(), 10);
  EXPECT_EQ(ad.at("priceMicros").asI64(), 2'000'000);
  EXPECT_EQ(ad.at("assetUrl").asString(), "https://cdn.example.com/a.png");
  EXPECT_EQ(h.app->budget().reserved(idx), 2000);
  EXPECT_EQ(h.app->budget().spentToday(idx), 0);

  const std::string impressionUrl = ad.at("impressionUrl").asString();
  EXPECT_EQ(call(h.port(), "GET", impressionUrl).status, 204);
  EXPECT_EQ(h.app->budget().reserved(idx), 0);
  EXPECT_EQ(h.app->budget().spentToday(idx), 2000);

  EXPECT_EQ(call(h.port(), "GET", impressionUrl).status, 204);
  EXPECT_EQ(h.app->budget().spentToday(idx), 2000);

  const Reply click = call(h.port(), "GET", ad.at("clickUrl").asString());
  EXPECT_EQ(click.status, 302);
  EXPECT_EQ(click.headers.at("location"), "https://example.com/landing");
  EXPECT_EQ(h.app->budget().spentToday(idx), 2000);

  EXPECT_TRUE(waitUntil([&] {
    const CtrTable::Stat s = h.app->ctr().stat(1, 7, 1);
    return s.impressions == 1.f && s.clicks == 1.f;
  }));

  const std::string metrics = call(h.port(), "GET", "/metrics").body;
  EXPECT_NE(metrics.find("ad_requests_total 1\n"), std::string::npos);
  EXPECT_NE(metrics.find("ad_fills_total 1\n"), std::string::npos);
  EXPECT_NE(metrics.find("ad_events_total{type=\"impression\"} 1\n"), std::string::npos);
  EXPECT_NE(metrics.find("ad_events_total{type=\"click\"} 1\n"), std::string::npos);
  EXPECT_NE(metrics.find("ad_events_rejected_total{reason=\"duplicate\"} 1\n"), std::string::npos);
  EXPECT_NE(metrics.find("ad_stage_latency_ms_count{stage=\"filter\"} 1\n"), std::string::npos);
}

TEST(e2e_cpc_campaign_pays_on_click_not_on_impression) {
  CampaignSpec spec = cpm(1, 1'000'000);
  spec.bidType = "CPC";
  Harness h(simpleCatalog({spec}));
  const u32 idx = h.idxOf(1);

  const Json ad = adRequest(h.port(), 1, 1000);
  ASSERT_TRUE(ad.at("fill").asBool());
  const i64 price = ad.at("priceMicros").asI64();
  EXPECT_EQ(h.app->budget().reserved(idx), price / 1000);

  EXPECT_EQ(call(h.port(), "GET", ad.at("impressionUrl").asString()).status, 204);
  EXPECT_EQ(h.app->budget().spentToday(idx), 0);
  EXPECT_EQ(h.app->budget().reserved(idx), 0);

  EXPECT_EQ(call(h.port(), "GET", ad.at("clickUrl").asString()).status, 302);
  const i64 charged = h.app->budget().spentToday(idx);
  EXPECT_GT(charged, 0);
  EXPECT_LE(charged, spec.bid);
}

TEST(e2e_frequency_cap_moves_the_user_to_the_next_campaign) {
  CampaignSpec capped = cpm(1, 4'000'000);
  capped.cap = 2;
  Harness h(simpleCatalog({capped, cpm(2, 2'000'000)}));

  for (u64 id = 1; id <= 2; ++id) {
    const Json ad = adRequest(h.port(), id);
    EXPECT_EQ(ad.at("campaignId").asU64(), 1);
    EXPECT_EQ(call(h.port(), "GET", ad.at("impressionUrl").asString()).status, 204);
  }
  EXPECT_EQ(adRequest(h.port(), 3).at("campaignId").asU64(), 2);
  EXPECT_EQ(adRequest(h.port(), 4, 0, 777).at("campaignId").asU64(), 1);
}

TEST(e2e_user_segments_come_from_redis) {
  CampaignSpec segmented = cpm(1, 9'000'000);
  segmented.targeting = R"({"segments":[10]})";
  Harness h(simpleCatalog({segmented, cpm(2, 1'000'000)}));
  h.redis.hset("user:500", "segments", "3,10");

  EXPECT_EQ(adRequest(h.port(), 1, 0, 500).at("campaignId").asU64(), 1);
  EXPECT_EQ(adRequest(h.port(), 2, 0, 501).at("campaignId").asU64(), 2);
}

TEST(e2e_no_fill_explains_why) {
  Harness h(twoCampaigns());
  const Reply reply = call(h.port(), "POST", "/ad-request", adBody(1, 0, 42, 999));
  ASSERT_EQ(reply.status, 200);
  const Json body = Json::parse(reply.body);
  EXPECT_FALSE(body.at("fill").asBool());
  EXPECT_EQ(body.at("reason").asString(), "no_candidates");
  EXPECT_TRUE(body.find("impressionUrl") == nullptr);
}

TEST(e2e_floor_price_sets_the_clearing_price) {
  Harness h(twoCampaigns());
  EXPECT_EQ(adRequest(h.port(), 1, 3'000'000).at("priceMicros").asI64(), 3'000'000);
  EXPECT_FALSE(adRequest(h.port(), 2, 5'000'000).at("fill").asBool());
}

TEST(e2e_first_price_option) {
  Harness h(twoCampaigns(), [](AppConfig& c) { c.firstPriceAuction = true; });
  EXPECT_EQ(adRequest(h.port(), 1).at("priceMicros").asI64(), 4'000'000);
}

TEST(e2e_tampered_tracking_link_is_refused) {
  Harness h(twoCampaigns());
  std::string url = adRequest(h.port(), 1).at("impressionUrl").asString();
  const std::size_t pos = url.find("price=");
  url.insert(pos + 6, "9");
  EXPECT_EQ(call(h.port(), "GET", url).status, 403);
  EXPECT_EQ(h.app->budget().spentToday(h.idxOf(1)), 0);
}

TEST(e2e_expired_tracking_link_is_refused) {
  Harness h(twoCampaigns(), [](AppConfig& c) { c.urlTtlSec = 0; });
  const std::string url = adRequest(h.port(), 1).at("impressionUrl").asString();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(call(h.port(), "GET", url).status, 410);
}

TEST(e2e_public_base_url_prefixes_tracking_links) {
  Harness h(twoCampaigns(), [](AppConfig& c) { c.publicBaseUrl = "https://ads.example.com"; });
  const std::string url = adRequest(h.port(), 1).at("impressionUrl").asString();
  ASSERT_TRUE(url.starts_with("https://ads.example.com/impression?"));
  // The signature covers the path and query, which is all the server receives.
  EXPECT_EQ(call(h.port(), "GET", url.substr(std::string("https://ads.example.com").size())).status, 204);
}

TEST(e2e_landing_urls_survive_encoding) {
  CreativeSpec creative;
  creative.id = 10;
  creative.campaign = 1;
  creative.click = "https://example.com/p?a=1&b=two%20words#frag";
  Harness h(catalogJson({cpm(1, 1'000'000)}, {creative}));
  const Reply click = call(h.port(), "GET", adRequest(h.port(), 1).at("clickUrl").asString());
  EXPECT_EQ(click.status, 302);
  EXPECT_EQ(click.headers.at("location"), "https://example.com/p?a=1&b=two%20words#frag");
}

TEST(e2e_a_line_break_in_a_landing_url_cannot_add_headers) {
  CreativeSpec creative;
  creative.id = 10;
  creative.campaign = 1;
  creative.click = "https://example.com/\\r\\nSet-Cookie: stolen=1";
  Harness h(catalogJson({cpm(1, 1'000'000)}, {creative}));
  const Reply click = call(h.port(), "GET", adRequest(h.port(), 1).at("clickUrl").asString());
  EXPECT_EQ(click.status, 302);
  EXPECT_EQ(click.headers.count("set-cookie"), 0);
}

TEST(e2e_invalid_ad_requests_get_a_400) {
  Harness h(twoCampaigns());
  const std::vector<std::string> bodies = {
      "",
      "not json",
      "[]",
      R"({"requestId":1})",
      R"({"requestId":0,"userId":1,"slotId":1,"slotSize":1})",
      R"({"requestId":1,"userId":0,"slotId":1,"slotSize":1})",
      R"({"requestId":1,"userId":1,"slotId":1,"slotSize":0})",
      R"({"requestId":-1,"userId":1,"slotId":1,"slotSize":1})",
      R"({"requestId":"1","userId":1,"slotId":1,"slotSize":1})",
      R"({"requestId":1,"userId":1,"slotId":4294967296,"slotSize":1})",
      R"({"requestId":1,"userId":1,"slotId":1,"slotSize":1,"floorMicros":-5})",
      R"({"requestId":1,"userId":1,"slotId":1,"slotSize":1,"debug":"yes"})",
      R"({"requestId":1,"userId":1,"slotId":1,"slotSize":1,"pageUrl":")" + std::string(3000, 'x') + "\"}",
  };
  for (const std::string& body : bodies) {
    const Reply reply = call(h.port(), "POST", "/ad-request", body);
    EXPECT_EQ(reply.status, 400);
    if (reply.status != 400) recordFailure(__FILE__, __LINE__, "accepted: " + body.substr(0, 120));
  }
  EXPECT_EQ(h.app->metrics().counterValue("ad_requests_total"), 0);
}

TEST(e2e_wrong_method_and_unknown_route) {
  Harness h(twoCampaigns());
  EXPECT_EQ(call(h.port(), "GET", "/ad-request").status, 405);
  EXPECT_EQ(call(h.port(), "GET", "/nope").status, 404);
}

TEST(e2e_health_reports_the_catalog_version) {
  Harness h(twoCampaigns());
  const Reply reply = call(h.port(), "GET", "/healthz");
  EXPECT_EQ(reply.status, 200);
  EXPECT_EQ(reply.body, "ok catalog_version=1\n");
}

TEST(e2e_http_rejects_malformed_protocol_input) {
  Harness h(twoCampaigns());
  const auto statusOf = [&](const std::string& wire) {
    const std::vector<Reply> replies = parseReplies(roundTrip(h.port(), wire));
    return replies.empty() ? 0 : replies[0].status;
  };
  EXPECT_EQ(statusOf("garbage\r\n\r\n"), 400);
  EXPECT_EQ(statusOf("GET /healthz HTTP/9.9\r\n\r\n"), 400);
  EXPECT_EQ(statusOf("GET healthz HTTP/1.1\r\n\r\n"), 400);
  EXPECT_EQ(statusOf("GET /healthz HTTP/1.1\r\nBadHeader\r\n\r\n"), 400);
  EXPECT_EQ(statusOf("GET /healthz HTTP/1.1\r\nBad Name: x\r\n\r\n"), 400);
  EXPECT_EQ(statusOf("POST /ad-request HTTP/1.1\r\nContent-Length: abc\r\n\r\n"), 400);
  EXPECT_EQ(statusOf("POST /ad-request HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab"), 400);
  EXPECT_EQ(statusOf("POST /ad-request HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n"), 413);
  EXPECT_EQ(statusOf("POST /ad-request HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"), 501);
  EXPECT_EQ(statusOf("GET /healthz HTTP/1.1\r\nX: " + std::string(16'400, 'a')), 431);
  EXPECT_EQ(statusOf("GET /healthz HTTP/1.1\r\nConnection: close\r\n\r\n"), 200);
}

TEST(e2e_http_rejects_control_characters) {
  Harness h(twoCampaigns());
  const auto statusOf = [&](const std::string& wire) {
    const std::vector<Reply> replies = parseReplies(roundTrip(h.port(), wire));
    return replies.empty() ? 0 : replies[0].status;
  };
  EXPECT_EQ(statusOf("GET /healthz HTTP/1.1\r\nX: a\nTransfer-Encoding: chunked\r\n\r\n"), 400);
  EXPECT_EQ(statusOf("GET /healthz HTTP/1.1\r\nX: a\rb\r\n\r\n"), 400);
  EXPECT_EQ(statusOf(std::string("GET /healthz\0x HTTP/1.1\r\n\r\n", 29)), 400);
  EXPECT_EQ(statusOf("GET /health z HTTP/1.1\r\n\r\n"), 400);
  EXPECT_EQ(statusOf("GET /healthz HTTP/1.1\r\nX: a\tb\r\nConnection: close\r\n\r\n"), 200);
}

TEST(e2e_http_head_has_headers_but_no_body) {
  Harness h(twoCampaigns());
  const std::string raw = roundTrip(h.port(), "HEAD /healthz HTTP/1.1\r\nConnection: close\r\n\r\n");
  ASSERT_TRUE(raw.starts_with("HTTP/1.1 200"));
  EXPECT_NE(raw.find("Content-Length: "), std::string::npos);
  EXPECT_TRUE(raw.ends_with("\r\n\r\n"));
}

TEST(e2e_http_says_close_on_the_last_request_it_will_serve) {
  Harness h(twoCampaigns());
  std::string wire;
  for (int i = 0; i < 1001; ++i) wire += "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n";
  const std::vector<Reply> replies = parseReplies(roundTrip(h.port(), wire));
  ASSERT_EQ(replies.size(), 1000);
  EXPECT_EQ(replies[998].headers.at("connection"), "keep-alive");
  EXPECT_EQ(replies[999].headers.at("connection"), "close");
}

TEST(e2e_http_second_pipelined_request_may_arrive_slowly) {
  Harness h(twoCampaigns());
  net::Socket socket = net::Socket::connectTcp("127.0.0.1", h.port(), 1000);
  ASSERT_TRUE(socket.valid());
  ASSERT_TRUE(socket.sendAll("GET /healthz HTTP/1.1\r\nHost: x\r\n\r\nGET /healthz HT", 1000));

  std::string out;
  char chunk[4096];
  while (out.find("catalog_version=1\n") == std::string::npos) {
    const int n = socket.recvSome(chunk, sizeof chunk, 5000);
    ASSERT_TRUE(n > 0);
    out.append(chunk, static_cast<std::size_t>(n));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(600));  // longer than the keep-alive idle limit
  ASSERT_TRUE(socket.sendAll("TP/1.1\r\nConnection: close\r\n\r\n", 1000));
  for (;;) {
    const int n = socket.recvSome(chunk, sizeof chunk, 5000);
    if (n <= 0) break;
    out.append(chunk, static_cast<std::size_t>(n));
  }
  const std::vector<Reply> replies = parseReplies(out);
  ASSERT_EQ(replies.size(), 2);
  EXPECT_EQ(replies[1].status, 200);
}

TEST(e2e_idle_connections_do_not_use_up_the_workers) {
  Harness h(twoCampaigns());
  std::vector<net::Socket> idle;
  for (int i = 0; i < 12; ++i) {
    idle.push_back(net::Socket::connectTcp("127.0.0.1", h.port(), 1000));
    ASSERT_TRUE(idle.back().valid());
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(call(h.port(), "GET", "/healthz").status, 200);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  EXPECT_LT(elapsed, 1000);
}

TEST(e2e_http_keep_alive_serves_pipelined_requests) {
  Harness h(twoCampaigns());
  const std::string wire =
      "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n"
      "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n"
      "GET /healthz HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  const std::vector<Reply> replies = parseReplies(roundTrip(h.port(), wire));
  ASSERT_EQ(replies.size(), 3);
  EXPECT_EQ(replies[0].status, 200);
  EXPECT_EQ(replies[1].status, 404);
  EXPECT_EQ(replies[2].status, 200);
}

TEST(e2e_http_body_arriving_in_pieces_is_reassembled) {
  Harness h(twoCampaigns());
  const std::string body = adBody(1);
  const std::string head = "POST /ad-request HTTP/1.1\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  net::Socket socket = net::Socket::connectTcp("127.0.0.1", h.port(), 1000);
  ASSERT_TRUE(socket.valid());
  ASSERT_TRUE(socket.sendAll(head.substr(0, 10), 1000));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_TRUE(socket.sendAll(head.substr(10), 1000));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_TRUE(socket.sendAll(body, 1000));

  std::string out;
  char chunk[4096];
  for (;;) {
    const int n = socket.recvSome(chunk, sizeof chunk, 5000);
    if (n <= 0) break;
    out.append(chunk, static_cast<std::size_t>(n));
  }
  const std::vector<Reply> replies = parseReplies(out);
  ASSERT_EQ(replies.size(), 1);
  EXPECT_EQ(replies[0].status, 200);
  EXPECT_TRUE(Json::parse(replies[0].body).at("fill").asBool());
}

TEST(e2e_a_client_that_disconnects_midway_does_no_harm) {
  Harness h(twoCampaigns());
  for (int i = 0; i < 20; ++i) {
    net::Socket socket = net::Socket::connectTcp("127.0.0.1", h.port(), 1000);
    ASSERT_TRUE(socket.valid());
    ASSERT_TRUE(socket.sendAll("POST /ad-request HTTP/1.1\r\nContent-Length: 500\r\n\r\n{", 1000));
  }
  EXPECT_EQ(call(h.port(), "GET", "/healthz").status, 200);
}

TEST(e2e_catalog_refresh_changes_serving) {
  Harness h(twoCampaigns());
  EXPECT_EQ(adRequest(h.port(), 1).at("campaignId").asU64(), 1);

  writeFile(h.catalogPath, simpleCatalog({cpm(1, 4'000'000), cpm(2, 2'000'000), cpm(3, 9'000'000)}));
  h.app->snapshots().refreshNow();
  const Json newest = adRequest(h.port(), 2);
  EXPECT_EQ(newest.at("campaignId").asU64(), 3);
  EXPECT_EQ(newest.at("priceMicros").asI64(), 4'000'000);

  writeFile(h.catalogPath, simpleCatalog({cpm(2, 2'000'000)}));
  h.app->snapshots().refreshNow();
  EXPECT_EQ(adRequest(h.port(), 3).at("campaignId").asU64(), 2);
}

TEST(e2e_a_catalog_missing_its_campaigns_table_leaves_serving_untouched) {
  Harness h(twoCampaigns());
  writeFile(h.catalogPath, R"({"advertisers":[]})");
  EXPECT_THROW(h.app->snapshots().refreshNow(), JsonError);
  EXPECT_TRUE(adRequest(h.port(), 1).at("fill").asBool());
}

TEST(e2e_a_broken_catalog_edit_leaves_serving_untouched) {
  Harness h(twoCampaigns());
  writeFile(h.catalogPath, "{ this is broken");
  EXPECT_THROW(h.app->snapshots().refreshNow(), JsonError);
  EXPECT_EQ(adRequest(h.port(), 1).at("campaignId").asU64(), 1);
}

TEST(e2e_a_win_from_the_old_catalog_can_still_be_tracked) {
  Harness h(twoCampaigns());
  const u32 idx = h.idxOf(1);
  const Json ad = adRequest(h.port(), 1);

  writeFile(h.catalogPath, simpleCatalog({cpm(2, 2'000'000)}));
  h.app->snapshots().refreshNow();

  EXPECT_EQ(call(h.port(), "GET", ad.at("impressionUrl").asString()).status, 204);
  EXPECT_EQ(h.app->budget().spentToday(idx), 2000);
}

TEST(e2e_serves_through_a_redis_outage) {
  MiniRedis dead;
  ASSERT_TRUE(dead.start());
  const u16 deadPort = dead.port();
  dead.stop();

  CampaignSpec capped = cpm(1, 4'000'000);
  capped.cap = 1;
  Harness h(simpleCatalog({capped, cpm(2, 2'000'000)}), [deadPort](AppConfig& c) {
    c.redisPort = deadPort;
    c.redisTimeoutMs = 20;
  });

  const Json first = adRequest(h.port(), 1);
  ASSERT_TRUE(first.at("fill").asBool());
  EXPECT_EQ(first.at("campaignId").asU64(), 1);
  EXPECT_EQ(call(h.port(), "GET", first.at("impressionUrl").asString()).status, 204);
  // The cap still holds, from local counts alone.
  EXPECT_EQ(adRequest(h.port(), 2).at("campaignId").asU64(), 2);
}

TEST(e2e_slow_redis_cannot_stall_a_request) {
  Harness h(twoCampaigns(), [](AppConfig& c) {
    c.redisTimeoutMs = 20;
    c.deadlineMs = 2000;
  });
  h.redis.setReplyDelayMs(1000);
  const auto start = std::chrono::steady_clock::now();
  const Json ad = adRequest(h.port(), 1);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  h.redis.setReplyDelayMs(0);
  EXPECT_TRUE(ad.at("fill").asBool());
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 800);
}

TEST(e2e_concurrent_requests_cannot_overspend_a_budget) {
  CampaignSpec spec = cpm(1, 4'000'000);
  spec.daily = 20'000;
  spec.total = 20'000;
  Harness h(simpleCatalog({spec}));

  // Each fill clears at the $4 floor and costs 4,000 micros, so 5 fills exist.
  std::atomic<u64> nextId{1};
  std::atomic<int> errors{0};
  std::mutex mutex;
  std::vector<std::string> pixels;
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 25; ++i) {
        const Reply reply = call(h.port(), "POST", "/ad-request", adBody(nextId.fetch_add(1), 4'000'000));
        if (reply.status != 200) {
          errors.fetch_add(1);
          continue;
        }
        const Json body = Json::parse(reply.body);
        if (body.at("fill").asBool()) {
          std::lock_guard lock(mutex);
          pixels.push_back(body.at("impressionUrl").asString());
        }
      }
    });
  }
  for (std::thread& t : threads) t.join();

  EXPECT_EQ(errors.load(), 0);
  EXPECT_GE(pixels.size(), 1);
  EXPECT_LE(pixels.size(), 5);
  for (const std::string& pixel : pixels) EXPECT_EQ(call(h.port(), "GET", pixel).status, 204);
  EXPECT_LE(h.app->budget().spentToday(h.idxOf(1)), 20'000);
  EXPECT_EQ(h.app->budget().spentToday(h.idxOf(1)), static_cast<i64>(pixels.size()) * 4000);
}

TEST(e2e_many_parallel_clients_all_get_answers) {
  Harness h(twoCampaigns());
  std::atomic<u64> nextId{1};
  std::atomic<int> ok{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 100; ++i) {
        if (call(h.port(), "POST", "/ad-request", adBody(nextId.fetch_add(1))).status == 200) ok.fetch_add(1);
      }
    });
  }
  for (std::thread& t : threads) t.join();
  EXPECT_EQ(ok.load(), 800);
  EXPECT_EQ(h.app->metrics().counterValue("ad_requests_total"), 800);
}

TEST(e2e_serving_continues_while_the_catalog_is_swapped) {
  Harness h(twoCampaigns());
  std::atomic<bool> done{false};
  std::thread refresher([&] {
    while (!done.load()) {
      h.app->snapshots().refreshNow();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  int failures = 0;
  for (u64 id = 1; id <= 300; ++id) {
    const Reply reply = call(h.port(), "POST", "/ad-request", adBody(id));
    if (reply.status != 200 || !Json::parse(reply.body).at("fill").asBool()) ++failures;
  }
  done.store(true);
  refresher.join();
  EXPECT_EQ(failures, 0);
}

TEST(e2e_trace_is_returned_only_when_tracing_is_enabled) {
  CampaignSpec expired = cpm(2, 9'000'000);
  expired.end = 1;
  const std::string catalog = simpleCatalog({cpm(1, 1'000'000), expired});
  const std::string body = R"({"requestId":1,"userId":1,"slotId":1,"slotSize":300250,"debug":true})";

  Harness traced(catalog, [](AppConfig& c) { c.tracing = true; });
  const Json on = Json::parse(call(traced.port(), "POST", "/ad-request", body).body);
  ASSERT_EQ(on.at("trace").asArray().size(), 1);
  EXPECT_EQ(on.at("trace").asArray()[0].at("stage").asString(), "schedule");

  Harness plain(catalog);
  const Json off = Json::parse(call(plain.port(), "POST", "/ad-request", body).body);
  EXPECT_EQ(off.at("trace").asArray().size(), 0);
}

TEST(e2e_shipped_seed_catalog_serves_end_to_end) {
  Harness h("{}", [](AppConfig& c) { c.catalogPath = std::string(AD_SOURCE_DIR) + "/data/catalog.json"; });
  const Json ad = adRequest(h.port(), 1);
  ASSERT_TRUE(ad.at("fill").asBool());
  EXPECT_EQ(ad.at("campaignId").asU64(), 101);
  EXPECT_EQ(ad.at("priceMicros").asI64(), 2'500'000);
}

TEST(e2e_a_missing_catalog_fails_at_startup) {
  MiniRedis redis;
  ASSERT_TRUE(redis.start());
  AppConfig c;
  c.port = 0;
  c.catalogPath = std::string(AD_TEST_DATA_DIR) + "/no_such_catalog.json";
  c.redisPort = redis.port();
  c.hmacSecret = "s";
  App app(std::move(c));
  EXPECT_THROW(app.start(), std::runtime_error);
}

TEST(e2e_a_port_already_in_use_is_an_error_not_a_crash) {
  Harness first(twoCampaigns());
  AppConfig c = first.config();
  c.port = first.port();
  {
    App second(std::move(c));
    EXPECT_THROW(second.start(), std::runtime_error);
  }
  EXPECT_EQ(call(first.port(), "GET", "/healthz").status, 200);
}

TEST(e2e_stop_flushes_events_and_budget) {
  Harness h(twoCampaigns());
  const u32 idx = h.idxOf(1);
  for (u64 id = 1; id <= 20; ++id) {
    EXPECT_EQ(call(h.port(), "GET", adRequest(h.port(), id).at("impressionUrl").asString()).status, 204);
  }
  h.app->stop();
  EXPECT_NEAR(h.app->ctr().stat(1, 7, 1).impressions, 20.0, 1e-6);
  EXPECT_EQ(h.redis.get("budget:total:1"), std::to_string(20 * 2000));
  EXPECT_EQ(h.app->budget().spentToday(idx), 20 * 2000);
  h.app->stop();
}

TEST(e2e_a_restart_keeps_the_spend) {
  CampaignSpec spec = cpm(1, 4'000'000);
  spec.daily = 10'000;
  spec.total = 10'000;
  Harness h(simpleCatalog({spec}));

  // Each impression clears at the floor and costs 4,000, so only 2 fit in 10,000.
  int filled = 0;
  for (u64 id = 1; id <= 5; ++id) {
    const Json ad = adRequest(h.port(), id, 4'000'000);
    if (!ad.at("fill").asBool()) continue;
    ++filled;
    EXPECT_EQ(call(h.port(), "GET", ad.at("impressionUrl").asString()).status, 204);
  }
  EXPECT_EQ(filled, 2);
  h.app->stop();
  h.app.reset();

  App restarted(h.config());
  restarted.start();
  const auto snapshot = restarted.snapshots().get();
  EXPECT_EQ(restarted.budget().spentToday(snapshot->idToIdx.at(1)), 8000);
  const Reply reply = call(restarted.port(), "POST", "/ad-request", adBody(100, 4'000'000));
  EXPECT_FALSE(Json::parse(reply.body).at("fill").asBool());
  restarted.stop();
}
