#include "server/http_server.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "common/url.h"

namespace ad {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;
constexpr int kRequestTimeoutMs = 5000;
constexpr int kKeepAliveIdleMs = 250;
constexpr int kSendTimeoutMs = 2000;
constexpr int kMaxRequestsPerConnection = 1000;
constexpr int kPendingPerWorker = 64;
constexpr int kAcceptPollMs = 50;
constexpr std::size_t kMaxParked = 4096;

constexpr int kClosed = -1;  // readRequest: connection ended, nothing to answer
constexpr int kOk = 0;

const char* reasonPhrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 410: return "Gone";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Unknown";
  }
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  });
  return out;
}

// Stops a response header from being split by a stray line break in a value.
std::string sanitizeHeaderValue(std::string_view value) {
  std::string out(value);
  std::erase_if(out, [](char c) { return c == '\r' || c == '\n'; });
  return out;
}

bool hasControlOrSpace(std::string_view s) {
  return std::any_of(s.begin(), s.end(), [](char ch) {
    const auto c = static_cast<unsigned char>(ch);
    return c <= 0x20 || c == 0x7F;
  });
}

// A bare CR or LF here is how a request gets smuggled past a proxy that splits
// lines differently, so only the horizontal tab is let through.
bool hasControlExceptTab(std::string_view s) {
  return std::any_of(s.begin(), s.end(), [](char ch) {
    const auto c = static_cast<unsigned char>(ch);
    return (c < 0x20 && c != '\t') || c == 0x7F;
  });
}

std::string serialize(const HttpResponse& res, bool keepAlive, bool isHead = false) {
  std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + reasonPhrase(res.status) + "\r\n";
  const bool hasBody = res.status != 204;
  if (hasBody) {
    out += "Content-Type: " + sanitizeHeaderValue(res.contentType) + "\r\n";
    out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
  }
  for (const auto& [name, value] : res.headers) {
    out += sanitizeHeaderValue(name) + ": " + sanitizeHeaderValue(value) + "\r\n";
  }
  out += keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
  if (hasBody && !isHead) out += res.body;
  return out;
}

int msLeft(Clock::time_point deadline) {
  return static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
}

// Returns kOk, kClosed, or the HTTP status to reject the request with.
// Bytes past the end of this request stay in `buffer` for the next one.
int readRequest(net::Socket& socket, std::string& buffer, HttpRequest& req, bool& keepAlive,
                int firstByteTimeoutMs) {
  bool started = !buffer.empty();  // part of a pipelined request may already be here
  auto deadline =
      Clock::now() + std::chrono::milliseconds(started ? kRequestTimeoutMs : firstByteTimeoutMs);

  const auto receive = [&]() -> int {
    char chunk[4096];
    const int left = msLeft(deadline);
    const int n = left > 0 ? socket.recvSome(chunk, sizeof chunk, left) : net::Socket::kTimeout;
    if (n > 0) {
      buffer.append(chunk, static_cast<std::size_t>(n));
      if (!started) {
        started = true;
        deadline = Clock::now() + std::chrono::milliseconds(kRequestTimeoutMs);
      }
    }
    return n;
  };

  std::size_t headerEnd;
  while ((headerEnd = buffer.find("\r\n\r\n")) == std::string::npos) {
    if (buffer.size() > kMaxHeaderBytes) return 431;
    const int n = receive();
    if (n == 0 || n == net::Socket::kError) return kClosed;
    if (n == net::Socket::kTimeout) return started ? 408 : kClosed;
  }
  if (headerEnd > kMaxHeaderBytes) return 431;

  const std::string_view head = std::string_view(buffer).substr(0, headerEnd);
  const std::size_t lineEnd = head.find("\r\n");
  const std::string_view requestLine = head.substr(0, lineEnd);
  const std::size_t sp1 = requestLine.find(' ');
  const std::size_t sp2 = requestLine.rfind(' ');
  if (sp1 == std::string_view::npos || sp2 == sp1) return 400;

  req.method = std::string(requestLine.substr(0, sp1));
  req.target = std::string(requestLine.substr(sp1 + 1, sp2 - sp1 - 1));
  const std::string_view version = requestLine.substr(sp2 + 1);
  if (req.target.empty() || req.target[0] != '/' || hasControlOrSpace(req.target)) return 400;
  if (version != "HTTP/1.1" && version != "HTTP/1.0") return 400;
  keepAlive = version == "HTTP/1.1";

  const std::size_t question = req.target.find('?');
  req.path = req.target.substr(0, question);
  if (question != std::string::npos) {
    req.rawQuery = req.target.substr(question + 1);
    req.query = parseQuery(req.rawQuery);
  }

  std::size_t contentLength = 0;
  bool sawContentLength = false;
  std::string_view rest = lineEnd == std::string_view::npos ? std::string_view{} : head.substr(lineEnd + 2);
  while (!rest.empty()) {
    const std::size_t end = rest.find("\r\n");
    const std::string_view line = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 2);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return 400;
    const std::string_view rawName = line.substr(0, colon);
    if (hasControlOrSpace(rawName)) return 400;
    std::string name = lower(rawName);
    const std::string_view value = trim(line.substr(colon + 1));
    if (hasControlExceptTab(value)) return 400;

    if (name == "transfer-encoding") return 501;
    if (name == "content-length") {
      std::size_t parsed = 0;
      auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (ec != std::errc{} || p != value.data() + value.size()) return 400;
      if (sawContentLength && parsed != contentLength) return 400;
      contentLength = parsed;
      sawContentLength = true;
    } else if (name == "connection") {
      const std::string token = lower(value);
      if (token == "close") keepAlive = false;
      if (token == "keep-alive") keepAlive = true;
    }
    req.headers.insert_or_assign(std::move(name), std::string(value));
  }
  if (contentLength > kMaxBodyBytes) return 413;

  const std::size_t bodyStart = headerEnd + 4;
  while (buffer.size() < bodyStart + contentLength) {
    const int n = receive();
    if (n == 0 || n == net::Socket::kError) return kClosed;
    if (n == net::Socket::kTimeout) return 408;
  }
  req.body = buffer.substr(bodyStart, contentLength);
  buffer.erase(0, bodyStart + contentLength);
  return kOk;
}

}

HttpResponse HttpResponse::json(int status, std::string body) {
  HttpResponse r;
  r.status = status;
  r.body = std::move(body);
  return r;
}

HttpResponse HttpResponse::text(int status, std::string body) {
  HttpResponse r;
  r.status = status;
  r.contentType = "text/plain; charset=utf-8";
  r.body = std::move(body);
  return r;
}

HttpServer::HttpServer(u16 port, int workerThreads, std::string bindAddress)
    : port_(port), workerThreads_(std::max(workerThreads, 1)), bindAddress_(std::move(bindAddress)) {}

void HttpServer::start() {
  listener_ = net::Socket::listenTcp(bindAddress_, port_);
  if (!listener_.valid()) {
    throw std::runtime_error("http: cannot listen on " + bindAddress_ + ":" + std::to_string(port_));
  }
  boundPort_ = listener_.localPort();
  running_.store(true);
  for (int i = 0; i < workerThreads_; ++i) workers_.emplace_back([this] { workerLoop(); });
  acceptor_ = std::thread([this] { acceptLoop(); });
}

void HttpServer::stop() {
  {
    // Under the lock, or a worker between its check and its wait would miss
    // this wakeup and sleep forever.
    std::lock_guard lock(mutex_);
    if (!running_.exchange(false)) return;
  }
  cv_.notify_all();
  if (acceptor_.joinable()) acceptor_.join();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();
  listener_.close();
  std::lock_guard lock(mutex_);
  pending_.clear();
}

void HttpServer::enqueue(net::Socket client) {
  const std::size_t maxPending = static_cast<std::size_t>(workerThreads_) * kPendingPerWorker;
  std::unique_lock lock(mutex_);
  if (pending_.size() >= maxPending) {
    lock.unlock();
    client.sendAll(serialize(HttpResponse::text(503, "overloaded\n"), false), 10);
    return;
  }
  pending_.push_back(std::move(client));
  lock.unlock();
  cv_.notify_one();
}

void HttpServer::acceptLoop() {
  // Connections that have sent nothing yet wait here and not in a worker, so
  // silent sockets (preconnects, pooled proxy connections) cannot use up the pool.
  struct Parked {
    net::Socket socket;
    Clock::time_point deadline;
  };
  std::deque<Parked> parked;
  std::vector<net::Socket*> watched;

  while (running_.load()) {
    watched.clear();
    watched.push_back(&listener_);
    for (Parked& p : parked) watched.push_back(&p.socket);
    const std::vector<char> ready = net::Socket::pollReadable(watched, kAcceptPollMs);

    const auto now = Clock::now();
    std::deque<Parked> stillWaiting;
    for (std::size_t i = 0; i < parked.size(); ++i) {
      if (ready[i + 1] != 0) {
        enqueue(std::move(parked[i].socket));
      } else if (now < parked[i].deadline) {
        stillWaiting.push_back(std::move(parked[i]));
      }
    }
    parked = std::move(stillWaiting);

    if (ready[0] == 0) continue;
    for (;;) {
      net::Socket client = listener_.acceptClient(0);
      if (!client.valid()) break;
      if (parked.size() >= kMaxParked) parked.pop_front();
      parked.push_back(Parked{std::move(client), now + std::chrono::milliseconds(kRequestTimeoutMs)});
    }
  }
}

void HttpServer::workerLoop() {
  for (;;) {
    net::Socket client;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return !running_.load() || !pending_.empty(); });
      if (!running_.load()) return;
      client = std::move(pending_.front());
      pending_.pop_front();
    }
    serveConnection(client);
  }
}

HttpResponse HttpServer::dispatch(const HttpRequest& request) const {
  const HttpHandler* handler = router_.find(request.path);
  if (handler == nullptr) return HttpResponse::text(404, "not found\n");
  try {
    return (*handler)(request);
  } catch (const std::exception& e) {
    std::cerr << "http: handler error on " << request.path << ": " << e.what() << "\n";
    return HttpResponse::text(500, "internal error\n");
  }
}

void HttpServer::serveConnection(net::Socket& socket) {
  std::string buffer;
  for (int served = 0; served < kMaxRequestsPerConnection && running_.load(); ++served) {
    HttpRequest request;
    bool keepAlive = false;
    const int rc = readRequest(socket, buffer, request, keepAlive,
                               served == 0 ? kRequestTimeoutMs : kKeepAliveIdleMs);
    if (rc == kClosed) return;
    if (rc != kOk) {
      socket.sendAll(serialize(HttpResponse::text(rc, "bad request\n"), false), kSendTimeoutMs);
      return;
    }
    const HttpResponse response = dispatch(request);
    {
      // Give the worker up when other connections are waiting.
      std::lock_guard lock(mutex_);
      if (!pending_.empty()) keepAlive = false;
    }
    if (served + 1 == kMaxRequestsPerConnection) keepAlive = false;
    if (!socket.sendAll(serialize(response, keepAlive, request.method == "HEAD"), kSendTimeoutMs) ||
        !keepAlive) {
      return;
    }
  }
}

}
