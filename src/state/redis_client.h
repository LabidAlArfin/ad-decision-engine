#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "common/net.h"
#include "common/types.h"

namespace ad {

struct RedisReply {
  enum class Type { Nil, Status, Integer, Bulk, Array, Error };
  Type type = Type::Nil;
  i64 integer = 0;
  std::string str;
  std::vector<RedisReply> elements;
};

using Replies = std::vector<RedisReply>;
using RedisCommand = std::vector<std::string>;

class ConnectionPool {
 public:
  ConnectionPool(std::string host, u16 port, std::size_t maxIdle)
      : host_(std::move(host)), port_(port), maxIdle_(maxIdle) {}

  // Returns an invalid socket while the circuit breaker is open or when
  // connecting fails.
  net::Socket acquire(int timeoutMs);
  void release(net::Socket socket);
  void markSuccess();
  void markFailure();

 private:
  std::string host_;
  u16 port_;
  std::size_t maxIdle_;
  std::mutex mutex_;
  std::vector<net::Socket> idle_;
  std::atomic<int> consecutiveFailures_{0};
  std::atomic<i64> openUntilMs_{0};
};

// Speaks the Redis RESP protocol directly over TCP. Every call is bounded by
// its timeout. Callers treat a failure as "Redis unavailable" and fall back to
// local state, so a slow Redis cannot stall ad serving.
class RedisClient {
 public:
  RedisClient(std::string host, u16 port, int timeoutMs = 5, std::size_t poolSize = 16)
      : pool_(std::move(host), port, poolSize), timeoutMs_(timeoutMs) {}

  // Empty result on failure; otherwise one entry per key.
  std::vector<std::optional<std::string>> mget(const std::vector<std::string>& keys);

  // INCR plus EXPIRE. Returns the new value, or -1 on failure.
  i64 incr(const std::string& key, int ttlSec);

  // nullopt means the call failed. A missing field returns an empty string,
  // so callers can tell "not known" from "Redis is down".
  std::optional<std::string> hget(const std::string& key, const std::string& field);

  // Empty result on failure; otherwise one reply per command.
  // timeoutMs of 0 uses the client default.
  Replies pipeline(const std::vector<RedisCommand>& commands, int timeoutMs = 0);

 private:
  ConnectionPool pool_;
  int timeoutMs_;
};

}
