#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "app/app.h"
#include "common/crypto.h"
#include "dev/mini_redis.h"

namespace {

std::atomic<bool> gStop{false};

void onSignal(int) { gStop.store(true); }

void printUsage() {
  std::cout << "ad_engine [options]\n"
               "  --port N            listen port (default 8080)\n"
               "  --bind ADDR         listen address (default 127.0.0.1)\n"
               "  --threads N         HTTP worker threads (default 8)\n"
               "  --catalog PATH      catalog JSON file (default data/catalog.json)\n"
               "  --redis HOST:PORT   Redis server. Without it, an embedded dev Redis is started\n"
               "  --secret S          tracking URL signing key (or env AD_ENGINE_SECRET)\n"
               "  --public-url URL    prefix for tracking URLs (default: relative paths)\n"
               "  --refresh-sec N     catalog refresh interval (default 30)\n"
               "  --deadline-ms N     per-request deadline (default 50)\n"
               "  --redis-timeout N   Redis timeout in ms (default 5)\n"
               "  --first-price       use first-price instead of second-price auctions\n"
               "  --trace             allow {\"debug\":true} requests to return a trace\n";
}

int toInt(const std::string& text, const char* flag, int min = 1, int max = 1'000'000) {
  int value = 0;
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size() || value < min || value > max) {
    std::cerr << "invalid value for " << flag << ": " << text << " (expected " << min << " to "
              << max << ")\n";
    std::exit(2);
  }
  return value;
}

}

int main(int argc, char** argv) {
  ad::AppConfig config;
  bool haveRedis = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << arg << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      printUsage();
      return 0;
    } else if (arg == "--port") {
      config.port = static_cast<ad::u16>(toInt(next(), "--port", 0, 65535));
    } else if (arg == "--bind") {
      config.bindAddress = next();
    } else if (arg == "--threads") {
      config.workerThreads = toInt(next(), "--threads");
    } else if (arg == "--catalog") {
      config.catalogPath = next();
    } else if (arg == "--redis") {
      const std::string value = next();
      const std::size_t colon = value.rfind(':');
      if (colon == std::string::npos) {
        std::cerr << "--redis expects HOST:PORT\n";
        return 2;
      }
      config.redisHost = value.substr(0, colon);
      config.redisPort = static_cast<ad::u16>(toInt(value.substr(colon + 1), "--redis", 1, 65535));
      haveRedis = true;
    } else if (arg == "--secret") {
      config.hmacSecret = next();
    } else if (arg == "--public-url") {
      config.publicBaseUrl = next();
    } else if (arg == "--refresh-sec") {
      config.catalogRefreshSec = toInt(next(), "--refresh-sec");
    } else if (arg == "--deadline-ms") {
      config.deadlineMs = toInt(next(), "--deadline-ms");
    } else if (arg == "--redis-timeout") {
      config.redisTimeoutMs = toInt(next(), "--redis-timeout");
    } else if (arg == "--first-price") {
      config.firstPriceAuction = true;
    } else if (arg == "--trace") {
      config.tracing = true;
    } else {
      std::cerr << "unknown option: " << arg << "\n";
      printUsage();
      return 2;
    }
  }

  if (config.hmacSecret.empty()) {
    if (const char* env = std::getenv("AD_ENGINE_SECRET")) config.hmacSecret = env;
  }
  if (config.hmacSecret.empty()) {
    config.hmacSecret = ad::randomHex(32);
    std::cerr << "warning: no signing secret set, using a random one. "
                 "Tracking links will stop working after a restart.\n";
  }

  std::unique_ptr<ad::MiniRedis> embedded;
  if (!haveRedis) {
    embedded = std::make_unique<ad::MiniRedis>();
    if (!embedded->start()) {
      std::cerr << "could not start the embedded Redis\n";
      return 1;
    }
    config.redisPort = embedded->port();
    // A loopback call under load can exceed the 5 ms meant for a real deployment.
    config.redisTimeoutMs = std::max(config.redisTimeoutMs, 25);
    std::cerr << "note: using embedded dev Redis on port " << config.redisPort
              << " (in-memory, for development only)\n";
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif

  try {
    ad::App app(std::move(config));
    app.start();
    std::cout << "ad engine listening on port " << app.port() << "\n" << std::flush;
    while (!gStop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "shutting down\n";
    app.stop();
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
