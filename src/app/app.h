#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "catalog/catalog.h"
#include "common/crypto.h"
#include "core/decision_engine.h"
#include "events/events.h"
#include "observability/observability.h"
#include "scoring/scoring.h"
#include "server/ad_request_handler.h"
#include "server/http_server.h"
#include "state/redis_client.h"
#include "state/runtime_state.h"

namespace ad {

struct AppConfig {
  std::string bindAddress = "127.0.0.1";
  u16 port = 8080;
  int workerThreads = 8;
  std::string catalogPath = "data/catalog.json";
  std::string redisHost = "127.0.0.1";
  u16 redisPort = 6379;
  int redisTimeoutMs = 5;
  std::string hmacSecret;
  std::string publicBaseUrl;  // empty: tracking urls are relative paths
  u32 maxCampaigns = 100'000;
  int catalogRefreshSec = 30;
  int deadlineMs = 50;
  int budgetSyncSec = 5;
  int reservationTtlSec = 120;
  int urlTtlSec = 3600;
  bool tracing = false;
  bool firstPriceAuction = false;
  float epsilon = 0.02f;
  float priorCtr = 0.01f;
};

class App {
 public:
  explicit App(AppConfig config);
  ~App();

  // Throws if the first catalog cannot be loaded or the port cannot be bound.
  void start();
  void stop();

  u16 port() const { return server_.boundPort(); }

  SnapshotManager& snapshots() { return snapshots_; }
  BudgetManager& budget() { return budget_; }
  CtrTable& ctr() { return ctr_; }
  MetricsRegistry& metrics() { return metrics_; }
  const UrlSigner& signer() const { return signer_; }

 private:
  void registerRoutes();
  void maintenanceLoop();
  void adjustPacing(Time now);

  AppConfig config_;
  MetricsRegistry metrics_;
  Tracer tracer_;
  CampaignSlots slots_;
  RedisClient redis_;
  BudgetManager budget_;
  PacingController pacing_;
  FrequencyCapStore freqCap_;
  UserProfileStore users_;
  SnapshotManager snapshots_;
  CtrTable ctr_;
  UrlSigner signer_;
  EventQueue eventQueue_;
  InProcessKafka kafka_;
  EventWriter eventWriter_;
  StatsUpdater stats_;
  std::unique_ptr<DecisionEngine> engine_;
  std::unique_ptr<AdRequestHandler> adHandler_;
  EventHandler eventHandler_;
  HttpServer server_;

  bool started_ = false;
  std::thread maintenance_;
  std::mutex stopMutex_;
  std::condition_variable stopCv_;
  bool stopping_ = false;
};

}
