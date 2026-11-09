#include "common/net.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ad::net {
namespace {

#ifdef _WIN32
using NativeHandle = SOCKET;
#else
using NativeHandle = int;
#endif

NativeHandle native(std::intptr_t h) { return static_cast<NativeHandle>(h); }

void ensureInitialized() {
#ifdef _WIN32
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
#endif
}

bool wouldBlock() {
#ifdef _WIN32
  const int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR;
#endif
}

void setNonBlocking(NativeHandle h) {
#ifdef _WIN32
  u_long on = 1;
  ioctlsocket(h, FIONBIO, &on);
#else
  fcntl(h, F_SETFL, fcntl(h, F_GETFL, 0) | O_NONBLOCK);
#endif
}

void setNoDelay(NativeHandle h) {
  int on = 1;
  setsockopt(h, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}

// Waits for readiness. Returns 1 when ready, 0 on timeout, -1 on error.
// Windows uses select() because WSAPoll misses failed connects on older builds.
int waitFor(NativeHandle h, bool forWrite, int timeoutMs) {
  timeoutMs = std::max(timeoutMs, 0);
#ifdef _WIN32
  fd_set primary;
  fd_set failed;
  FD_ZERO(&primary);
  FD_ZERO(&failed);
  FD_SET(h, &primary);
  FD_SET(h, &failed);
  timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
  const int rc = select(0, forWrite ? nullptr : &primary, forWrite ? &primary : nullptr, &failed, &tv);
  if (rc < 0) return -1;
  return rc == 0 ? 0 : 1;
#else
  pollfd p{h, static_cast<short>(forWrite ? POLLOUT : POLLIN), 0};
  const int rc = poll(&p, 1, timeoutMs);
  if (rc < 0) return errno == EINTR ? 0 : -1;
  return rc == 0 ? 0 : 1;
#endif
}

#ifdef _WIN32
const int kSendFlags = 0;
#elif defined(MSG_NOSIGNAL)
const int kSendFlags = MSG_NOSIGNAL;
#else
const int kSendFlags = 0;
#endif

bool resolveIpv4(const std::string& host, u16 port, sockaddr_in& out) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) return false;
  std::memcpy(&out, found->ai_addr, sizeof out);
  freeaddrinfo(found);
  out.sin_port = htons(port);
  return true;
}

}

void Socket::close() {
  if (handle_ == kInvalid) return;
#ifdef _WIN32
  closesocket(native(handle_));
#else
  ::close(native(handle_));
#endif
  handle_ = kInvalid;
}

int Socket::recvSome(char* buffer, std::size_t length, int timeoutMs) {
  const int ready = waitFor(native(handle_), false, timeoutMs);
  if (ready == 0) return kTimeout;
  if (ready < 0) return kError;
  const int n = static_cast<int>(
      ::recv(native(handle_), buffer, static_cast<int>(std::min<std::size_t>(length, 1 << 30)), 0));
  if (n > 0) return n;
  if (n == 0) return 0;
  return wouldBlock() ? kTimeout : kError;
}

bool Socket::sendAll(std::string_view data, int timeoutMs) {
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!data.empty()) {
    const int n = static_cast<int>(::send(native(handle_), data.data(),
                                          static_cast<int>(std::min<std::size_t>(data.size(), 1 << 30)),
                                          kSendFlags));
    if (n > 0) {
      data.remove_prefix(static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && !wouldBlock()) return false;
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0 || waitFor(native(handle_), true, static_cast<int>(left)) <= 0) return false;
  }
  return true;
}

Socket Socket::connectTcp(const std::string& host, u16 port, int timeoutMs) {
  ensureInitialized();
  sockaddr_in addr{};
  if (!resolveIpv4(host, port, addr)) return Socket();
  const NativeHandle h = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  Socket s(static_cast<std::intptr_t>(h));
#ifdef _WIN32
  if (h == INVALID_SOCKET) return Socket(kInvalid);
#else
  if (h < 0) return Socket(kInvalid);
#endif
  setNonBlocking(h);
  setNoDelay(h);
  if (::connect(h, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
    if (!wouldBlock()) return Socket();
    if (waitFor(h, true, timeoutMs) <= 0) return Socket();
    int err = 0;
    socklen_t len = sizeof err;
    getsockopt(h, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
    if (err != 0) return Socket();
  }
  return s;
}

Socket Socket::listenTcp(const std::string& bindAddress, u16 port) {
  ensureInitialized();
  sockaddr_in addr{};
  if (!resolveIpv4(bindAddress, port, addr)) return Socket();
  const NativeHandle h = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
  if (h == INVALID_SOCKET) return Socket();
#else
  if (h < 0) return Socket();
  int on = 1;  // On Windows the same option would let others hijack the port.
  setsockopt(h, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
#endif
  Socket s(static_cast<std::intptr_t>(h));
  if (::bind(h, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) return Socket();
  if (::listen(h, SOMAXCONN) != 0) return Socket();
  setNonBlocking(h);
  return s;
}

Socket Socket::acceptClient(int timeoutMs) {
  if (waitFor(native(handle_), false, timeoutMs) <= 0) return Socket();
  const NativeHandle h = ::accept(native(handle_), nullptr, nullptr);
#ifdef _WIN32
  if (h == INVALID_SOCKET) return Socket();
#else
  if (h < 0) return Socket();
#endif
  setNonBlocking(h);
  setNoDelay(h);
  return Socket(static_cast<std::intptr_t>(h));
}

std::vector<char> Socket::pollReadable(const std::vector<Socket*>& sockets, int timeoutMs) {
  std::vector<char> ready(sockets.size(), 0);
#ifdef _WIN32
  std::vector<WSAPOLLFD> fds(sockets.size());
  for (std::size_t i = 0; i < sockets.size(); ++i) {
    fds[i].fd = native(sockets[i]->handle_);
    fds[i].events = POLLRDNORM;
  }
  const int rc = WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), std::max(timeoutMs, 0));
#else
  std::vector<pollfd> fds(sockets.size());
  for (std::size_t i = 0; i < sockets.size(); ++i) {
    fds[i].fd = native(sockets[i]->handle_);
    fds[i].events = POLLIN;
  }
  const int rc = poll(fds.data(), static_cast<nfds_t>(fds.size()), std::max(timeoutMs, 0));
#endif
  if (rc <= 0) return ready;
  for (std::size_t i = 0; i < sockets.size(); ++i) ready[i] = fds[i].revents != 0 ? 1 : 0;
  return ready;
}

u16 Socket::localPort() const {
  sockaddr_in addr{};
  socklen_t len = sizeof addr;
  if (getsockname(native(handle_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
  return ntohs(addr.sin_port);
}

}
