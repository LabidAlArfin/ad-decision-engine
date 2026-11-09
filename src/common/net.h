#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/types.h"

namespace ad::net {

// Non-blocking TCP socket. Every operation takes a timeout, so that a slow
// peer can never hold a request thread longer than the caller allows.
class Socket {
 public:
  static constexpr int kTimeout = -1;
  static constexpr int kError = -2;

  Socket() = default;
  ~Socket() { close(); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& o) noexcept : handle_(std::exchange(o.handle_, kInvalid)) {}
  Socket& operator=(Socket&& o) noexcept {
    if (this != &o) {
      close();
      handle_ = std::exchange(o.handle_, kInvalid);
    }
    return *this;
  }

  bool valid() const { return handle_ != kInvalid; }
  void close();

  // Returns bytes read (> 0), 0 when the peer closed, kTimeout or kError.
  int recvSome(char* buffer, std::size_t length, int timeoutMs);
  bool sendAll(std::string_view data, int timeoutMs);

  // Each returns an invalid socket on failure.
  static Socket connectTcp(const std::string& host, u16 port, int timeoutMs);
  static Socket listenTcp(const std::string& bindAddress, u16 port);
  Socket acceptClient(int timeoutMs);

  // Waits until any socket can be read (or has closed), up to the timeout.
  // Returns one flag per socket, all zero on timeout or error.
  static std::vector<char> pollReadable(const std::vector<Socket*>& sockets, int timeoutMs);

  u16 localPort() const;

 private:
  static constexpr std::intptr_t kInvalid = -1;
  explicit Socket(std::intptr_t handle) : handle_(handle) {}
  std::intptr_t handle_ = kInvalid;
};

}
