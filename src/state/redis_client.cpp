#include "state/redis_client.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <string_view>
#include <utility>

namespace ad {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kBreakerThreshold = 3;
constexpr i64 kBreakerOpenMs = 500;
constexpr std::size_t kMaxLine = 64 * 1024;
constexpr i64 kMaxBulk = 64 * 1024 * 1024;
constexpr int kMaxDepth = 8;

i64 steadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void appendCommand(std::string& out, const RedisCommand& command) {
  out += '*';
  out += std::to_string(command.size());
  out += "\r\n";
  for (const std::string& arg : command) {
    out += '$';
    out += std::to_string(arg.size());
    out += "\r\n";
    out += arg;
    out += "\r\n";
  }
}

class ReplyReader {
 public:
  ReplyReader(net::Socket& socket, Clock::time_point deadline) : socket_(socket), deadline_(deadline) {}

  bool read(RedisReply& out, int depth = 0) {
    if (depth > kMaxDepth) return false;
    std::string line;
    if (!readLine(line) || line.empty()) return false;
    const char kind = line[0];
    const std::string_view rest = std::string_view(line).substr(1);
    switch (kind) {
      case '+':
        out.type = RedisReply::Type::Status;
        out.str = std::string(rest);
        return true;
      case '-':
        out.type = RedisReply::Type::Error;
        out.str = std::string(rest);
        return true;
      case ':':
        out.type = RedisReply::Type::Integer;
        return parseInt(rest, out.integer);
      case '$': {
        i64 length = 0;
        if (!parseInt(rest, length) || length > kMaxBulk) return false;
        if (length < 0) {
          out.type = RedisReply::Type::Nil;
          return true;
        }
        const auto n = static_cast<std::size_t>(length);
        if (!ensure(n + 2)) return false;
        out.type = RedisReply::Type::Bulk;
        out.str.assign(buffer_, pos_, n);
        pos_ += n + 2;
        return true;
      }
      case '*': {
        i64 count = 0;
        if (!parseInt(rest, count) || count > 1'000'000) return false;
        if (count < 0) {
          out.type = RedisReply::Type::Nil;
          return true;
        }
        out.type = RedisReply::Type::Array;
        out.elements.resize(static_cast<std::size_t>(count));
        for (RedisReply& element : out.elements) {
          if (!read(element, depth + 1)) return false;
        }
        return true;
      }
      default:
        return false;
    }
  }

 private:
  static bool parseInt(std::string_view text, i64& out) {
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && end == text.data() + text.size();
  }

  bool fill() {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - Clock::now()).count();
    if (left <= 0) return false;
    char chunk[8192];
    const int n = socket_.recvSome(chunk, sizeof chunk, static_cast<int>(left));
    if (n <= 0) return false;
    buffer_.append(chunk, static_cast<std::size_t>(n));
    return true;
  }

  bool ensure(std::size_t bytes) {
    while (buffer_.size() - pos_ < bytes) {
      if (!fill()) return false;
    }
    return true;
  }

  bool readLine(std::string& line) {
    std::size_t from = pos_;
    for (;;) {
      const std::size_t end = buffer_.find("\r\n", from);
      if (end != std::string::npos) {
        line.assign(buffer_, pos_, end - pos_);
        pos_ = end + 2;
        return true;
      }
      if (buffer_.size() - pos_ > kMaxLine) return false;
      from = buffer_.empty() ? pos_ : std::max(pos_, buffer_.size() - 1);
      if (!fill()) return false;
    }
  }

  net::Socket& socket_;
  Clock::time_point deadline_;
  std::string buffer_;
  std::size_t pos_ = 0;
};

}

net::Socket ConnectionPool::acquire(int timeoutMs) {
  if (steadyNowMs() < openUntilMs_.load(std::memory_order_relaxed)) return net::Socket();
  {
    std::lock_guard lock(mutex_);
    if (!idle_.empty()) {
      net::Socket socket = std::move(idle_.back());
      idle_.pop_back();
      return socket;
    }
  }
  net::Socket socket = net::Socket::connectTcp(host_, port_, timeoutMs);
  if (!socket.valid()) markFailure();
  return socket;
}

void ConnectionPool::release(net::Socket socket) {
  std::lock_guard lock(mutex_);
  if (idle_.size() < maxIdle_) idle_.push_back(std::move(socket));
}

void ConnectionPool::markSuccess() { consecutiveFailures_.store(0, std::memory_order_relaxed); }

void ConnectionPool::markFailure() {
  if (consecutiveFailures_.fetch_add(1, std::memory_order_relaxed) + 1 >= kBreakerThreshold) {
    openUntilMs_.store(steadyNowMs() + kBreakerOpenMs, std::memory_order_relaxed);
  }
}

Replies RedisClient::pipeline(const std::vector<RedisCommand>& commands, int timeoutMs) {
  if (commands.empty()) return {};
  const int budget = timeoutMs > 0 ? timeoutMs : timeoutMs_;
  const auto deadline = Clock::now() + std::chrono::milliseconds(budget);

  net::Socket socket = pool_.acquire(budget);
  if (!socket.valid()) return {};

  std::string wire;
  for (const RedisCommand& command : commands) appendCommand(wire, command);
  if (!socket.sendAll(wire, budget)) {
    pool_.markFailure();
    return {};
  }

  ReplyReader reader(socket, deadline);
  Replies replies(commands.size());
  for (RedisReply& reply : replies) {
    if (!reader.read(reply)) {
      // The socket is dropped here, not returned to the pool: unread replies
      // on it would otherwise be handed to the next caller as its own answer.
      pool_.markFailure();
      return {};
    }
  }
  pool_.markSuccess();
  pool_.release(std::move(socket));
  return replies;
}

std::vector<std::optional<std::string>> RedisClient::mget(const std::vector<std::string>& keys) {
  if (keys.empty()) return {};
  RedisCommand command;
  command.reserve(keys.size() + 1);
  command.emplace_back("MGET");
  command.insert(command.end(), keys.begin(), keys.end());

  Replies replies = pipeline({command});
  if (replies.size() != 1 || replies[0].type != RedisReply::Type::Array ||
      replies[0].elements.size() != keys.size()) {
    return {};
  }
  std::vector<std::optional<std::string>> out(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (replies[0].elements[i].type == RedisReply::Type::Bulk) {
      out[i] = std::move(replies[0].elements[i].str);
    }
  }
  return out;
}

i64 RedisClient::incr(const std::string& key, int ttlSec) {
  Replies replies = pipeline({{"INCR", key}, {"EXPIRE", key, std::to_string(ttlSec)}});
  if (replies.size() != 2 || replies[0].type != RedisReply::Type::Integer) return -1;
  return replies[0].integer;
}

std::optional<std::string> RedisClient::hget(const std::string& key, const std::string& field) {
  Replies replies = pipeline({{"HGET", key, field}});
  if (replies.size() != 1 || replies[0].type == RedisReply::Type::Error) return std::nullopt;
  return std::move(replies[0].str);
}

}
