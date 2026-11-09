#include "dev/mini_redis.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <optional>
#include <string_view>

namespace ad {
namespace {

constexpr i64 kMaxArgs = 1024 * 1024;
constexpr std::size_t kMaxBuffered = 64 * 1024 * 1024;

std::string bulk(const std::string& value) {
  return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}
const char* const kNil = "$-1\r\n";

std::string integer(i64 value) { return ":" + std::to_string(value) + "\r\n"; }

std::optional<i64> toInt(std::string_view text) {
  i64 value = 0;
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}

// Parses one command from the front of `buffer`. Returns nullopt until a
// complete command has arrived.
std::optional<std::vector<std::string>> parseCommand(const std::string& buffer, std::size_t& consumed) {
  std::size_t pos = 0;
  const auto readLine = [&](std::string_view& line) {
    const std::size_t end = buffer.find("\r\n", pos);
    if (end == std::string::npos) return false;
    line = std::string_view(buffer).substr(pos, end - pos);
    pos = end + 2;
    return true;
  };

  std::string_view line;
  if (!readLine(line) || line.empty() || line[0] != '*') return std::nullopt;
  const auto count = toInt(line.substr(1));
  if (!count || *count < 0 || *count > kMaxArgs) return std::nullopt;

  std::vector<std::string> command;
  command.reserve(static_cast<std::size_t>(std::min<i64>(*count, 64)));
  for (i64 i = 0; i < *count; ++i) {
    if (!readLine(line) || line.empty() || line[0] != '$') return std::nullopt;
    const auto length = toInt(line.substr(1));
    if (!length || *length < 0) return std::nullopt;
    const auto n = static_cast<std::size_t>(*length);
    if (buffer.size() < pos + n + 2) return std::nullopt;
    command.emplace_back(buffer, pos, n);
    pos += n + 2;
  }
  consumed = pos;
  return command;
}

}

bool MiniRedis::start(u16 port) {
  listener_ = net::Socket::listenTcp("127.0.0.1", port);
  if (!listener_.valid()) return false;
  port_ = listener_.localPort();
  running_.store(true);
  acceptor_ = std::thread([this] { acceptLoop(); });
  return true;
}

void MiniRedis::stop() {
  if (!running_.exchange(false)) return;
  if (acceptor_.joinable()) acceptor_.join();
  listener_.close();
  std::lock_guard lock(threadsMutex_);
  for (std::thread& t : connections_) {
    if (t.joinable()) t.join();
  }
  connections_.clear();
}

void MiniRedis::acceptLoop() {
  while (running_.load()) {
    net::Socket client = listener_.acceptClient(20);
    if (!client.valid()) continue;
    std::lock_guard lock(threadsMutex_);
    connections_.emplace_back([this, s = std::move(client)]() mutable { serve(std::move(s)); });
  }
}

void MiniRedis::serve(net::Socket socket) {
  std::string buffer;
  while (running_.load()) {
    char chunk[4096];
    const int n = socket.recvSome(chunk, sizeof chunk, 20);
    if (n == net::Socket::kTimeout) continue;
    if (n <= 0) return;
    buffer.append(chunk, static_cast<std::size_t>(n));

    std::string out;
    std::size_t consumed = 0;
    while (auto command = parseCommand(buffer, consumed)) {
      buffer.erase(0, consumed);
      out += execute(*command);
    }
    if (buffer.size() > kMaxBuffered) return;
    if (out.empty()) continue;
    const int delay = replyDelayMs_.load();
    if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    if (!socket.sendAll(out, 2000)) return;
  }
}

std::string MiniRedis::execute(const std::vector<std::string>& c) {
  if (c.empty()) return "-ERR empty command\r\n";
  std::string name = c[0];
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
    return static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
  });

  std::lock_guard lock(dataMutex_);
  if (name == "PING") return "+PONG\r\n";
  if (name == "GET" && c.size() == 2) {
    auto it = strings_.find(c[1]);
    return it == strings_.end() ? std::string(kNil) : bulk(it->second);
  }
  if (name == "SET" && c.size() >= 3) {
    strings_[c[1]] = c[2];
    return "+OK\r\n";
  }
  if (name == "MGET" && c.size() >= 2) {
    std::string out = "*" + std::to_string(c.size() - 1) + "\r\n";
    for (std::size_t i = 1; i < c.size(); ++i) {
      auto it = strings_.find(c[i]);
      out += it == strings_.end() ? std::string(kNil) : bulk(it->second);
    }
    return out;
  }
  if ((name == "INCR" && c.size() == 2) || (name == "INCRBY" && c.size() == 3)) {
    const std::optional<i64> delta = name == "INCR" ? std::optional<i64>(1) : toInt(c[2]);
    auto it = strings_.find(c[1]);
    const std::optional<i64> current = it == strings_.end() ? std::optional<i64>(0) : toInt(it->second);
    if (!delta || !current) return "-ERR value is not an integer or out of range\r\n";
    if ((*delta > 0 && *current > std::numeric_limits<i64>::max() - *delta) ||
        (*delta < 0 && *current < std::numeric_limits<i64>::min() - *delta)) {
      return "-ERR increment or decrement would overflow\r\n";
    }
    const i64 next = *current + *delta;
    strings_[c[1]] = std::to_string(next);
    return integer(next);
  }
  if (name == "EXPIRE" && c.size() == 3) return integer(strings_.count(c[1]) != 0 ? 1 : 0);
  if (name == "HGET" && c.size() == 3) {
    auto h = hashes_.find(c[1]);
    if (h == hashes_.end()) return kNil;
    auto f = h->second.find(c[2]);
    return f == h->second.end() ? std::string(kNil) : bulk(f->second);
  }
  if (name == "HSET" && c.size() == 4) {
    const bool added = hashes_[c[1]].insert_or_assign(c[2], c[3]).second;
    return integer(added ? 1 : 0);
  }
  return "-ERR unknown command '" + c[0] + "'\r\n";
}

void MiniRedis::set(const std::string& key, const std::string& value) {
  std::lock_guard lock(dataMutex_);
  strings_[key] = value;
}

void MiniRedis::hset(const std::string& key, const std::string& field, const std::string& value) {
  std::lock_guard lock(dataMutex_);
  hashes_[key][field] = value;
}

std::string MiniRedis::get(const std::string& key) {
  std::lock_guard lock(dataMutex_);
  auto it = strings_.find(key);
  return it == strings_.end() ? std::string() : it->second;
}

}
