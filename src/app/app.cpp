#include "app/app.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace ad {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kEventQueueCapacity = 1 << 16;
constexpr std::size_t kFreqCapEntries = 1'000'000;
constexpr std::size_t kProfileEntries = 1'000'000;
constexpr int kFreqCapTtlSec = 2 * 86'400;
constexpr int kEventBatchSize = 500;
constexpr int kEventFlushMs = 100;
constexpr auto kPacingAdjustEvery = std::chrono::seconds(60);
constexpr auto kMaintenanceTick = std::chrono::milliseconds(200);

}

App::App(AppConfig config)
    : config_(std::move(config)),
      tracer_(config_.tracing),
      slots_(config_.maxCampaigns),
      redis_(config_.redisHost, config_.redisPort, config_.redisTimeoutMs),
      budget_(slots_, &redis_, config_.budgetSyncSec),
      pacing_(config_.maxCampaigns),
      freqCap_(&redis_, kFreqCapEntries, kFreqCapTtlSec),
      users_(&redis_, kProfileEntries),
      snapshots_(CampaignLoader(std::make_unique<SeedFilePgConnection>(config_.catalogPath), slots_),
                 config_.catalogRefreshSec, &metrics_),
      ctr_(config_.priorCtr * 100.f, (1.f - config_.priorCtr) * 100.f),
      signer_(config_.hmacSecret),
      eventQueue_(kEventQueueCapacity),
      eventWriter_(eventQueue_, KafkaProducer("ad-events", kafka_), kEventBatchSize, kEventFlushMs, &metrics_),
      stats_(ctr_),
      // A tracking link must expire before its dedup entry can be forgotten.
      eventHandler_(eventQueue_, budget_, freqCap_, signer_, metrics_,
                    std::chrono::seconds(config_.urlTtlSec)),
      server_(config_.port, config_.workerThreads, config_.bindAddress) {
  snapshots_.addListener([this](const CatalogSnapshot& snapshot) {
    budget_.applyCatalog(snapshot);
    pacing_.applyCatalog(snapshot, nowTime());
  });

  kafka_.subscribe([this](std::vector<std::string> messages) {
    std::vector<Event> events;
    events.reserve(messages.size());
    for (const std::string& message : messages) {
      if (auto event = KafkaProducer::decode(message)) events.push_back(*event);
    }
    stats_.consume(events);
  });

  FilterPipeline pipeline(&metrics_, &tracer_);
  pipeline.add(std::make_unique<ScheduleFilter>());
  pipeline.add(std::make_unique<FloorPriceFilter>());
  pipeline.add(std::make_unique<BudgetFilter>(budget_));
  pipeline.add(std::make_unique<PacingFilter>(pacing_));
  pipeline.add(std::make_unique<FrequencyCapFilter>(freqCap_));  // last: it goes to Redis

  std::unique_ptr<Auction> auction;
  if (config_.firstPriceAuction) {
    auction = std::make_unique<FirstPriceAuction>();
  } else {
    auction = std::make_unique<SecondPriceAuction>();
  }
  engine_ = std::make_unique<DecisionEngine>(
      snapshots_, CandidateRetriever{}, std::move(pipeline),
      std::make_unique<CtrScorer>(ctr_, config_.epsilon, config_.priorCtr), std::move(auction), users_,
      config_.deadlineMs, tracer_, metrics_);
  adHandler_ = std::make_unique<AdRequestHandler>(*engine_, signer_, metrics_, config_.publicBaseUrl,
                                                  config_.urlTtlSec);
  registerRoutes();
}

App::~App() { stop(); }

void App::registerRoutes() {
  server_.route("/ad-request", [this](const HttpRequest& r) { return adHandler_->handleAdRequest(r); });
  server_.route("/impression", [this](const HttpRequest& r) { return eventHandler_.handleImpression(r); });
  server_.route("/click", [this](const HttpRequest& r) { return eventHandler_.handleClick(r); });
  server_.route("/metrics", [this](const HttpRequest&) {
    HttpResponse response = HttpResponse::text(200, metrics_.expose());
    response.contentType = "text/plain; version=0.0.4";
    return response;
  });
  server_.route("/healthz", [this](const HttpRequest&) {
    const auto snapshot = snapshots_.get();
    return snapshot ? HttpResponse::text(200, "ok catalog_version=" + std::to_string(snapshot->version) + "\n")
                    : HttpResponse::text(503, "no catalog\n");
  });
}

void App::start() {
  snapshots_.refreshNow();
  budget_.syncWithRedis();  // recover spend from before a restart
  // Set before any thread exists: if server_.start() throws, stop() in the
  // destructor still has to join the threads started here.
  started_ = true;
  kafka_.start();
  eventWriter_.start();
  snapshots_.startRefreshThread();
  maintenance_ = std::thread([this] { maintenanceLoop(); });
  server_.start();
}

void App::stop() {
  if (!started_) return;
  started_ = false;
  server_.stop();
  eventWriter_.stop();
  kafka_.stop();
  snapshots_.stop();
  {
    std::lock_guard lock(stopMutex_);
    stopping_ = true;
  }
  stopCv_.notify_all();
  if (maintenance_.joinable()) maintenance_.join();
  budget_.syncWithRedis();
}

void App::adjustPacing(Time now) {
  const auto snapshot = snapshots_.get();
  if (!snapshot) return;
  const double dayFraction =
      static_cast<double>(toUnixMs(now) - utcDay(now) * kMsPerDay) / static_cast<double>(kMsPerDay);
  for (u32 idx = 0; idx < snapshot->campaigns.size(); ++idx) {
    const Campaign& c = snapshot->campaigns[idx];
    if (c.status != EntityStatus::Active || c.pacing != PacingMode::Even) continue;
    const double expected = static_cast<double>(c.dailyBudgetMicros) * dayFraction;
    if (expected < 1.0) continue;
    const double error = (expected - static_cast<double>(budget_.spentToday(idx))) / expected;
    pacing_.adjustRate(idx, static_cast<float>(error));
  }
}

void App::maintenanceLoop() {
  auto lastSync = Clock::now();
  auto lastExpiry = Clock::now();
  auto lastPacing = Clock::now();
  i64 lastDay = utcDay(nowTime());
  const auto syncEvery = std::chrono::seconds(config_.budgetSyncSec);
  const auto expireEvery = std::chrono::seconds(std::max(config_.reservationTtlSec / 2, 1));

  std::unique_lock lock(stopMutex_);
  while (!stopCv_.wait_for(lock, kMaintenanceTick, [this] { return stopping_; })) {
    lock.unlock();
    const Time now = nowTime();
    const auto tick = Clock::now();
    pacing_.refill(now);
    if (tick - lastSync >= syncEvery) {
      budget_.syncWithRedis();
      lastSync = tick;
    }
    if (tick - lastExpiry >= expireEvery) {
      budget_.expireReservations();
      lastExpiry = tick;
    }
    if (tick - lastPacing >= kPacingAdjustEvery) {
      adjustPacing(now);
      lastPacing = tick;
    }
    if (const i64 day = utcDay(now); day != lastDay) {
      budget_.rollDay(now);
      stats_.recomputeNightly();
      lastDay = day;
    }
    lock.lock();
  }
}

}
