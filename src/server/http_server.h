#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/net.h"
#include "common/types.h"

namespace ad {

struct HttpRequest {
  std::string method;
  std::string target;    // exactly as received; tracking signatures cover these bytes
  std::string path;
  std::string rawQuery;
  std::unordered_map<std::string, std::string> headers;  // names are lowercase
  std::unordered_map<std::string, std::string> query;    // percent-decoded
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string contentType = "application/json";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;

  static HttpResponse json(int status, std::string body);
  static HttpResponse text(int status, std::string body);
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class Router {
 public:
  void add(std::string path, HttpHandler handler) { routes_[std::move(path)] = std::move(handler); }
  const HttpHandler* find(const std::string& path) const {
    auto it = routes_.find(path);
    return it == routes_.end() ? nullptr : &it->second;
  }

 private:
  std::unordered_map<std::string, HttpHandler> routes_;
};

// A deliberately small HTTP/1.1 server: a fixed worker pool, bounded queues and
// strict request limits. It rejects chunked bodies rather than parsing them.
class HttpServer {
 public:
  HttpServer(u16 port, int workerThreads, std::string bindAddress = "127.0.0.1");
  ~HttpServer() { stop(); }

  // Register routes before start(); the table is not locked afterwards.
  void route(std::string path, HttpHandler handler) { router_.add(std::move(path), std::move(handler)); }

  // Throws if the port cannot be bound.
  void start();
  void stop();
  u16 boundPort() const { return boundPort_; }

 private:
  void acceptLoop();
  void enqueue(net::Socket client);
  void workerLoop();
  void serveConnection(net::Socket& socket);
  HttpResponse dispatch(const HttpRequest& request) const;

  u16 port_;
  int workerThreads_;
  std::string bindAddress_;
  Router router_;
  net::Socket listener_;
  u16 boundPort_ = 0;

  std::atomic<bool> running_{false};
  std::thread acceptor_;
  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<net::Socket> pending_;
};

}
