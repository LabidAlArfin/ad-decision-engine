#include <algorithm>
#include <atomic>
#include <iterator>
#include <numeric>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "common/containers.h"
#include "common/crypto.h"
#include "common/json.h"
#include "common/roaring.h"
#include "common/types.h"
#include "common/url.h"
#include "observability/observability.h"
#include "test_framework.h"

using namespace ad;

namespace {

std::string shaHex(std::string_view text) { return toHex(sha256(text)); }
std::string hmacHex(std::string_view key, std::string_view text) { return toHex(hmacSha256(key, text)); }

std::vector<u32> valuesOf(const Roaring& r) {
  std::vector<u32> out;
  r.appendTo(out);
  return out;
}

std::vector<u32> valuesOf(const std::set<u32>& s) { return std::vector<u32>(s.begin(), s.end()); }

std::string replaced(std::string text, std::string_view from, std::string_view to) {
  const std::size_t pos = text.find(from);
  if (pos != std::string::npos) text.replace(pos, from.size(), to);
  return text;
}

}

TEST(time_utc_day_floors_before_the_epoch) {
  EXPECT_EQ(utcDay(fromUnixMs(0)), 0);
  EXPECT_EQ(utcDay(fromUnixMs(kMsPerDay - 1)), 0);
  EXPECT_EQ(utcDay(fromUnixMs(kMsPerDay)), 1);
  EXPECT_EQ(utcDay(fromUnixMs(-1)), -1);
  EXPECT_EQ(utcDay(fromUnixMs(-kMsPerDay)), -1);
  EXPECT_EQ(utcDay(fromUnixMs(-kMsPerDay - 1)), -2);
}

TEST(time_utc_hour) {
  EXPECT_EQ(utcHour(fromUnixMs(0)), 0);
  EXPECT_EQ(utcHour(fromUnixMs(5 * kMsPerHour + 1)), 5);
  EXPECT_EQ(utcHour(fromUnixMs(kMsPerDay - 1)), 23);
  EXPECT_EQ(utcHour(fromUnixMs(-1)), 23);
}

TEST(sha256_published_vectors) {
  EXPECT_EQ(shaHex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(shaHex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(shaHex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  EXPECT_EQ(shaHex(std::string(1'000'000, 'a')),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// Reference digests computed with Python's hashlib, at the lengths where the
// padding spills into another block.
TEST(sha256_padding_boundaries) {
  EXPECT_EQ(shaHex(std::string(55, 'a')), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  EXPECT_EQ(shaHex(std::string(56, 'a')), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  EXPECT_EQ(shaHex(std::string(63, 'a')), "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
  EXPECT_EQ(shaHex(std::string(64, 'a')), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
  EXPECT_EQ(shaHex(std::string(65, 'a')), "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
  EXPECT_EQ(shaHex(std::string(119, 'a')), "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb");
  EXPECT_EQ(shaHex(std::string(120, 'a')), "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c");
}

TEST(hmac_rfc4231_vectors) {
  EXPECT_EQ(hmacHex(std::string(20, '\x0b'), "Hi There"),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  EXPECT_EQ(hmacHex("Jefe", "what do ya want for nothing?"),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  EXPECT_EQ(hmacHex(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First"),
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
  EXPECT_EQ(hmacHex(std::string(64, 'k'), std::string(200, 'm')),
            "cbe7a7476313c59aa81a2a0a723c9f793d20d82b544e788bf308f39922b3d12c");
}

TEST(constant_time_equals) {
  EXPECT_TRUE(constantTimeEquals("abc", "abc"));
  EXPECT_TRUE(constantTimeEquals("", ""));
  EXPECT_FALSE(constantTimeEquals("abc", "abd"));
  EXPECT_FALSE(constantTimeEquals("abc", "abcd"));
}

TEST(random_hex_has_the_requested_length_and_varies) {
  EXPECT_EQ(randomHex(32).size(), 64);
  EXPECT_NE(randomHex(32), randomHex(32));
}

TEST(url_signer_accepts_its_own_links) {
  const UrlSigner signer("secret");
  EXPECT_TRUE(signer.verify(signer.sign("/impression", "rid=1&price=5")));
}

TEST(url_signer_rejects_every_tampering) {
  const UrlSigner signer("secret");
  const std::string link = signer.sign("/impression", "rid=1&price=5");

  EXPECT_FALSE(signer.verify(replaced(link, "price=5", "price=9")));
  EXPECT_FALSE(signer.verify(replaced(link, "/impression", "/click")));
  EXPECT_FALSE(signer.verify("/impression?rid=1&price=5"));
  EXPECT_FALSE(signer.verify(link + "&price=0"));
  EXPECT_FALSE(signer.verify(link.substr(0, link.size() - 1)));
  EXPECT_FALSE(UrlSigner("other").verify(link));
}

TEST(percent_encode_leaves_only_unreserved_characters) {
  EXPECT_EQ(percentEncode("aZ09-_.~"), "aZ09-_.~");
  EXPECT_EQ(percentEncode("a b&c=d/?"), "a%20b%26c%3Dd%2F%3F");
  EXPECT_EQ(percentEncode(std::string("\xC3\xA9")), "%C3%A9");
}

TEST(percent_decode_round_trips) {
  const std::string original = "https://example.com/x?a=1&b=two words#frag";
  EXPECT_EQ(percentDecode(percentEncode(original)).value(), original);
  EXPECT_EQ(percentDecode("%41").value(), "A");
  EXPECT_EQ(percentDecode("a+b").value(), "a b");
}

TEST(percent_decode_rejects_malformed_escapes) {
  EXPECT_FALSE(percentDecode("%").has_value());
  EXPECT_FALSE(percentDecode("%4").has_value());
  EXPECT_FALSE(percentDecode("%zz").has_value());
  EXPECT_FALSE(percentDecode("ok%4").has_value());
}

TEST(parse_query_handles_odd_input) {
  const auto q = parseQuery("a=1&b=x%20y&novalue&c=%zz&a=2&empty=");
  EXPECT_EQ(q.at("a"), "2");
  EXPECT_EQ(q.at("b"), "x y");
  EXPECT_EQ(q.at("empty"), "");
  EXPECT_EQ(q.count("novalue"), 0);
  EXPECT_EQ(q.count("c"), 0);
  EXPECT_TRUE(parseQuery("").empty());
}

TEST(json_keeps_full_64_bit_integers) {
  EXPECT_EQ(Json::parse("18446744073709551615").asU64(), 18446744073709551615ULL);
  EXPECT_EQ(Json::parse("18446744073709551615").dump(), "18446744073709551615");
  EXPECT_EQ(Json::parse("-9223372036854775808").asI64(), std::numeric_limits<i64>::min());
  EXPECT_EQ(Json::parse("9007199254740993").asU64(), 9007199254740993ULL);
}

TEST(json_integers_too_large_are_not_silently_truncated) {
  EXPECT_THROW(Json::parse("18446744073709551616").asU64(), JsonError);
  EXPECT_THROW(Json::parse("18446744073709551615").asI64(), JsonError);
}

TEST(json_round_trips_a_document) {
  const std::string text = R"({"a":[1,-2,true,false,null,"x\ny"],"b":{"c":"d"},"e":1.5})";
  const Json doc = Json::parse(text);
  EXPECT_EQ(doc.dump(), text);
  EXPECT_EQ(doc.at("b").at("c").asString(), "d");
  EXPECT_EQ(doc.at("a").asArray().size(), 6);
  EXPECT_TRUE(doc.at("a").asArray()[4].isNull());
  EXPECT_NEAR(doc.at("e").asDouble(), 1.5, 1e-12);
}

TEST(json_decodes_escapes) {
  EXPECT_EQ(Json::parse(R"("\"\\\/\b\f\n\r\t")").asString(), "\"\\/\b\f\n\r\t");
  EXPECT_EQ(Json::parse(R"("\u00e9")").asString(), "\xC3\xA9");
  EXPECT_EQ(Json::parse(R"("\u20ac")").asString(), "\xE2\x82\xAC");
  EXPECT_EQ(Json::parse(R"("\ud83d\ude00")").asString(), "\xF0\x9F\x98\x80");
}

TEST(json_escapes_control_characters_when_dumping) {
  EXPECT_EQ(Json(std::string("a\x01" "b")).dump(), R"("a\u0001b")");
  EXPECT_EQ(Json(std::string("\"\\")).dump(), R"("\"\\")");
}

TEST(json_rejects_malformed_documents) {
  const std::vector<std::string> bad = {
      "",        "{",          "}",          "[",         "[1,",      "[1,]",    "{\"a\"}",
      "{\"a\":}", "{\"a\":1,}", "{a:1}",      "01",        "1.",       "1e",      "-",
      "+1",      "tru",        "nul",        "\"abc",     "\"\\x\"",  "\"\\u12\"", "\"\\ud800\"",
      "\"\\udc00\"", "[1] 2",  "\"a\x01z\"", "NaN",       "Infinity",
  };
  for (const std::string& text : bad) {
    bool threw = false;
    try {
      Json::parse(text);
    } catch (const JsonError&) {
      threw = true;
    }
    EXPECT_TRUE(threw);
    if (!threw) adtest::recordFailure(__FILE__, __LINE__, "accepted: " + text);
  }
}

TEST(json_caps_nesting) {
  EXPECT_THROW(Json::parse(std::string(200'000, '[')), JsonError);
  const std::string fine = std::string(20, '[') + std::string(20, ']');
  EXPECT_EQ(Json::parse(fine).dump(), fine);
}

TEST(json_type_mismatches_throw) {
  EXPECT_THROW(Json::parse("-1").asU64(), JsonError);
  EXPECT_THROW(Json::parse("1.5").asU64(), JsonError);
  EXPECT_THROW(Json::parse("\"x\"").asBool(), JsonError);
  EXPECT_THROW(Json::parse("1").asString(), JsonError);
  EXPECT_THROW(Json::parse("{}").asArray(), JsonError);
  EXPECT_THROW(Json::parse("[]").asObject(), JsonError);
  EXPECT_THROW(Json::parse("{}").at("missing"), JsonError);
  EXPECT_TRUE(Json::parse("[]").find("key") == nullptr);
}

TEST(roaring_basic_membership) {
  Roaring r;
  EXPECT_TRUE(r.isEmpty());
  r.add(5);
  r.add(5);
  r.add(70'000);
  r.add(0xFFFFFFFF);
  EXPECT_EQ(r.cardinality(), 3);
  EXPECT_TRUE(r.contains(5));
  EXPECT_TRUE(r.contains(70'000));
  EXPECT_TRUE(r.contains(0xFFFFFFFF));
  EXPECT_FALSE(r.contains(6));
  EXPECT_FALSE(r.contains(0));
  const std::vector<u32> expected = {5, 70'000, 0xFFFFFFFF};
  EXPECT_TRUE(valuesOf(r) == expected);
}

// Every operation is compared with std::set, at densities that land in both
// the array and the bitmap representation.
TEST(roaring_matches_a_reference_set) {
  std::minstd_rand rng(12345);
  for (int size : {50, 3000, 4096, 4097, 20000, 90000}) {
    std::uniform_int_distribution<u32> pick(0, 2 * 65536);
    Roaring a, b;
    std::set<u32> sa, sb;
    for (int i = 0; i < size; ++i) {
      const u32 v = pick(rng);
      a.add(v);
      sa.insert(v);
    }
    for (int i = 0; i < size / 2 + 10; ++i) {
      const u32 v = pick(rng);
      b.add(v);
      sb.insert(v);
    }
    EXPECT_EQ(a.cardinality(), sa.size());
    EXPECT_TRUE(valuesOf(a) == valuesOf(sa));

    std::set<u32> expected;
    Roaring result = a;
    result |= b;
    std::set_union(sa.begin(), sa.end(), sb.begin(), sb.end(), std::inserter(expected, expected.end()));
    EXPECT_TRUE(valuesOf(result) == valuesOf(expected));
    EXPECT_EQ(result.cardinality(), expected.size());

    expected.clear();
    result = a;
    result &= b;
    std::set_intersection(sa.begin(), sa.end(), sb.begin(), sb.end(), std::inserter(expected, expected.end()));
    EXPECT_TRUE(valuesOf(result) == valuesOf(expected));
    EXPECT_EQ(result.cardinality(), expected.size());

    expected.clear();
    result = a;
    result -= b;
    std::set_difference(sa.begin(), sa.end(), sb.begin(), sb.end(), std::inserter(expected, expected.end()));
    EXPECT_TRUE(valuesOf(result) == valuesOf(expected));
    EXPECT_EQ(result.cardinality(), expected.size());
  }
}

TEST(roaring_operations_that_empty_a_set) {
  Roaring a, b;
  for (u32 i = 0; i < 10'000; ++i) a.add(i);
  for (u32 i = 0; i < 10'000; ++i) b.add(i + 100'000);

  Roaring disjoint = a;
  disjoint &= b;
  EXPECT_TRUE(disjoint.isEmpty());
  EXPECT_EQ(disjoint.cardinality(), 0);

  Roaring self = a;
  self -= a;
  EXPECT_TRUE(self.isEmpty());
  EXPECT_FALSE(self.contains(5));

  Roaring empty;
  Roaring grown = a;
  grown |= empty;
  EXPECT_EQ(grown.cardinality(), 10'000);
  empty |= a;
  EXPECT_EQ(empty.cardinality(), 10'000);
}

TEST(roaring_dense_chunk_shrinks_back_correctly) {
  Roaring dense, sparse;
  for (u32 i = 0; i < 65536; ++i) dense.add(i);
  sparse.add(7);
  sparse.add(65535);
  EXPECT_EQ(dense.cardinality(), 65536);
  dense &= sparse;
  const std::vector<u32> expected = {7, 65535};
  EXPECT_TRUE(valuesOf(dense) == expected);
  dense.add(9);
  EXPECT_EQ(dense.cardinality(), 3);
  EXPECT_TRUE(dense.contains(9));
}

TEST(small_map_keeps_keys_sorted_and_unique) {
  small_map<u64, u16> m;
  EXPECT_TRUE(m.find(1) == nullptr);
  m.set(9, 1);
  m.set(3, 2);
  m.set(9, 7);
  EXPECT_EQ(m.size(), 2);
  EXPECT_EQ(*m.find(9), 7);
  EXPECT_EQ(*m.find(3), 2);
  m.assign({{5, 50}, {1, 10}, {3, 30}});
  EXPECT_EQ(m.size(), 3);
  EXPECT_EQ(*m.find(1), 10);
  EXPECT_EQ(*m.find(5), 50);
  EXPECT_TRUE(m.find(9) == nullptr);
}

TEST(lru_cache_never_exceeds_capacity) {
  LruCache<u64, int> cache(160);
  for (u64 i = 0; i < 20'000; ++i) cache.put(i, static_cast<int>(i));
  int present = 0;
  for (u64 i = 0; i < 20'000; ++i) present += cache.get(i).has_value() ? 1 : 0;
  EXPECT_LE(present, 160);
  EXPECT_GT(present, 0);
  EXPECT_EQ(cache.get(19'999).value(), 19'999);
}

TEST(lru_cache_expires_entries) {
  LruCache<u64, int> cache(100, std::chrono::milliseconds(30));
  cache.put(1, 10);
  EXPECT_EQ(cache.get(1).value(), 10);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  EXPECT_FALSE(cache.get(1).has_value());
}

TEST(lru_cache_upsert_sees_the_current_value) {
  LruCache<u64, int> cache(100);
  const auto bump = [](std::optional<int> v) { return v ? *v + 1 : 100; };
  EXPECT_EQ(cache.upsert(1, bump), 100);
  EXPECT_EQ(cache.upsert(1, bump), 101);
  EXPECT_EQ(cache.get(1).value(), 101);
}

TEST(lru_cache_upsert_treats_expired_as_absent) {
  LruCache<u64, int> cache(100, std::chrono::milliseconds(20));
  const auto bump = [](std::optional<int> v) { return v ? *v + 1 : 100; };
  cache.upsert(1, bump);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  EXPECT_EQ(cache.upsert(1, bump), 100);
}

TEST(lru_cache_is_safe_under_contention) {
  LruCache<u64, int> cache(1024);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&cache] {
      for (u64 i = 0; i < 20'000; ++i) {
        cache.upsert(i % 64, [](std::optional<int> v) { return v.value_or(0) + 1; });
        (void)cache.get(i % 128);
      }
    });
  }
  for (std::thread& t : threads) t.join();
  int total = 0;
  for (u64 i = 0; i < 64; ++i) total += cache.get(i).value_or(0);
  EXPECT_EQ(total, 8 * 20'000);
}

TEST(ring_buffer_rounds_up_and_preserves_order) {
  MpmcRingBuffer<int> ring(5);
  EXPECT_EQ(ring.capacity(), 8);
  for (int i = 0; i < 8; ++i) EXPECT_TRUE(ring.tryPush(i));
  EXPECT_FALSE(ring.tryPush(99));
  int value = -1;
  for (int i = 0; i < 8; ++i) {
    EXPECT_TRUE(ring.tryPop(value));
    EXPECT_EQ(value, i);
  }
  EXPECT_FALSE(ring.tryPop(value));
}

TEST(ring_buffer_wraps_around) {
  MpmcRingBuffer<int> ring(4);
  int value = 0;
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(ring.tryPush(i));
    ASSERT_TRUE(ring.tryPop(value));
    EXPECT_EQ(value, i);
  }
}

TEST(ring_buffer_loses_nothing_with_many_threads) {
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 50'000;
  MpmcRingBuffer<u64> ring(1024);
  std::atomic<u64> sum{0};
  std::atomic<int> popped{0};

  std::vector<std::thread> threads;
  for (int p = 0; p < kProducers; ++p) {
    threads.emplace_back([&ring, p] {
      for (int i = 1; i <= kPerProducer; ++i) {
        const u64 value = static_cast<u64>(p) * 1'000'000 + static_cast<u64>(i);
        while (!ring.tryPush(value)) std::this_thread::yield();
      }
    });
  }
  for (int c = 0; c < 4; ++c) {
    threads.emplace_back([&] {
      u64 value = 0;
      while (popped.load() < kProducers * kPerProducer) {
        if (ring.tryPop(value)) {
          sum.fetch_add(value);
          popped.fetch_add(1);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }
  for (std::thread& t : threads) t.join();

  u64 expected = 0;
  for (int p = 0; p < kProducers; ++p) {
    for (int i = 1; i <= kPerProducer; ++i) expected += static_cast<u64>(p) * 1'000'000 + static_cast<u64>(i);
  }
  EXPECT_EQ(popped.load(), kProducers * kPerProducer);
  EXPECT_EQ(sum.load(), expected);
}

TEST(metrics_counts_and_exposes) {
  MetricsRegistry metrics;
  metrics.inc("a_total");
  metrics.inc("a_total", "", 2);
  metrics.inc("b_total", "k=\"v\"");

  EXPECT_EQ(metrics.counterValue("a_total"), 3);
  EXPECT_EQ(metrics.counterValue("b_total", "k=\"v\""), 1);
  EXPECT_EQ(metrics.counterValue("b_total"), 0);
  EXPECT_EQ(metrics.counterValue("missing"), 0);

  const std::string text = metrics.expose();
  EXPECT_NE(text.find("# TYPE a_total counter\n"), std::string::npos);
  EXPECT_NE(text.find("a_total 3\n"), std::string::npos);
  EXPECT_NE(text.find("b_total{k=\"v\"} 1\n"), std::string::npos);
}

TEST(metrics_histogram_buckets_are_cumulative) {
  MetricsRegistry metrics;
  metrics.observeLatency("x", 0.1);
  metrics.observeLatency("x", 3.0);
  metrics.observeLatency("x", 1000.0);

  const std::string text = metrics.expose();
  EXPECT_NE(text.find("ad_stage_latency_ms_bucket{stage=\"x\",le=\"0.25\"} 1\n"), std::string::npos);
  EXPECT_NE(text.find("ad_stage_latency_ms_bucket{stage=\"x\",le=\"2\"} 1\n"), std::string::npos);
  EXPECT_NE(text.find("ad_stage_latency_ms_bucket{stage=\"x\",le=\"5\"} 2\n"), std::string::npos);
  EXPECT_NE(text.find("ad_stage_latency_ms_bucket{stage=\"x\",le=\"250\"} 2\n"), std::string::npos);
  EXPECT_NE(text.find("ad_stage_latency_ms_bucket{stage=\"x\",le=\"+Inf\"} 3\n"), std::string::npos);
  EXPECT_NE(text.find("ad_stage_latency_ms_count{stage=\"x\"} 3\n"), std::string::npos);
  EXPECT_NE(text.find("ad_stage_latency_ms_sum{stage=\"x\"} 1003.100\n"), std::string::npos);
}

TEST(metrics_count_correctly_under_contention) {
  MetricsRegistry metrics;
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&metrics] {
      for (int i = 0; i < 20'000; ++i) {
        metrics.inc("c_total");
        metrics.observeLatency("s", 1.0);
      }
    });
  }
  for (std::thread& t : threads) t.join();
  EXPECT_EQ(metrics.counterValue("c_total"), 160'000);
  EXPECT_NE(metrics.expose().find("ad_stage_latency_ms_count{stage=\"s\"} 160000\n"), std::string::npos);
}

TEST(tracer_records_only_when_enabled) {
  Tracer off(false);
  off.begin(1);
  off.stageDrop("a", "b");
  EXPECT_TRUE(off.end().drops.empty());

  Tracer on(true);
  EXPECT_EQ(on.begin(77), 77);
  on.stageDrop("budget", "2 dropped");
  const DebugTrace trace = on.end();
  EXPECT_EQ(trace.id, 77);
  ASSERT_EQ(trace.drops.size(), 1);
  EXPECT_EQ(trace.drops[0].stage, "budget");
  EXPECT_EQ(trace.drops[0].reason, "2 dropped");
  EXPECT_TRUE(on.end().drops.empty());
}

TEST(tracer_keeps_threads_apart) {
  Tracer tracer(true);
  tracer.begin(1);
  tracer.stageDrop("main", "x");
  std::thread other([&tracer] {
    tracer.begin(2);
    tracer.stageDrop("other", "y");
    tracer.end();
  });
  other.join();
  const DebugTrace trace = tracer.end();
  EXPECT_EQ(trace.id, 1);
  ASSERT_EQ(trace.drops.size(), 1);
  EXPECT_EQ(trace.drops[0].stage, "main");
}
