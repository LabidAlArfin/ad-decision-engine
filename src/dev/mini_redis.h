#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/net.h"
#include "common/types.h"

namespace ad {

// A tiny Redis stand-in that speaks real RESP over TCP. It exists so that the
// engine can run and be tested on a machine with no Redis installed.
// Development only: state lives in memory and key expiry is not enforced.
class MiniRedis {
 public:
  MiniRedis() = default;
  ~MiniRedis() { stop(); }

  bool start(u16 port = 0);  // port 0 picks a free port
  void stop();
  u16 port() const { return port_; }

  void setReplyDelayMs(int ms) { replyDelayMs_.store(ms); }
  void set(const std::string& key, const std::string& value);
  void hset(const std::string& key, const std::string& field, const std::string& value);
  std::string get(const std::string& key);

 private:
  void acceptLoop();
  void serve(net::Socket socket);
  std::string execute(const std::vector<std::string>& command);

  net::Socket listener_;
  u16 port_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<int> replyDelayMs_{0};
  std::thread acceptor_;
  std::mutex threadsMutex_;
  std::vector<std::thread> connections_;

  std::mutex dataMutex_;
  std::unordered_map<std::string, std::string> strings_;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> hashes_;
};

}
