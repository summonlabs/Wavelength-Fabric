#include "wavelength_fabric/net.hpp"

#ifdef _WIN32
// winsock2.h must precede windows.h; the lean-and-mean guard keeps the older
// winsock.h, which would collide with winsock2.h, out of this translation unit.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// GetSystemTimePreciseAsFileTime, used by wallClockNanos, is declared only
// from Windows 8 onwards.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

// Blocking IPv4 stream transport over the loopback interface.
//
// Every descriptor this file creates is owned by a Socket from the instant it
// exists, so no early return can leak one. Lengths that arrive from a caller are
// range-checked before they reach the kernel, because a wrapped length would
// silently truncate a frame instead of failing the transfer.

namespace wavelength_fabric {
namespace {

// Upper bound on a single transfer in either direction: a caller may not ask
// for an unbounded kernel copy.
constexpr std::size_t kMaxTransferBytes = 64u * 1024u * 1024u;

#ifdef _WIN32

using NativeSocket = SOCKET;
using SockLen = int;
using NameLen = std::size_t;
constexpr NativeSocket kInvalidNativeSocket = INVALID_SOCKET;
constexpr int kSendFlags = 0;
constexpr int kShutdownBoth = SD_BOTH;

#else

using NativeSocket = int;
using SockLen = socklen_t;
using NameLen = socklen_t;
constexpr NativeSocket kInvalidNativeSocket = -1;
constexpr int kSendFlags = MSG_NOSIGNAL;
constexpr int kShutdownBoth = SHUT_RDWR;

#endif

// Socket keeps its handle in an int, so the native value is mapped through
// intptr_t; both directions round-trip exactly.
[[nodiscard]] int toHandle(NativeSocket socket) noexcept {
  return static_cast<int>(static_cast<std::intptr_t>(socket));
}

[[nodiscard]] NativeSocket fromHandle(int handle) noexcept {
  return static_cast<NativeSocket>(static_cast<std::intptr_t>(handle));
}

[[nodiscard]] int lastSocketError() noexcept {
#ifdef _WIN32
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

// Text for an operating-system error code. It is carried verbatim in every
// failure message so a caller never has to guess what the kernel refused.
[[nodiscard]] std::string osErrorText(int code) {
#ifdef _WIN32
  if (code == 0) return "success";
  char text[512] = {};
  const DWORD length = ::FormatMessageA(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
      static_cast<DWORD>(code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), text,
      static_cast<DWORD>(sizeof(text)), nullptr);
  std::string message(text, static_cast<std::size_t>(length));
  while (!message.empty() &&
         (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
    message.pop_back();
  }
  if (message.empty()) return "error " + std::to_string(code);
  return message + " (code " + std::to_string(code) + ")";
#else
  if (code == 0) return "success";
  return std::string(std::strerror(code)) + " (errno " + std::to_string(code) + ")";
#endif
}

// A socket subsystem is process wide, so it is reference counted: repeated
// initSockets()/shutdownSockets() pairs are harmless, and a socket call made
// before any initSockets() performs the platform start-up exactly once.
std::atomic<std::uint32_t>& socketRefCount() noexcept {
  static std::atomic<std::uint32_t> count{0};
  return count;
}

std::mutex& socketMutex() noexcept {
  static std::mutex mutex;
  return mutex;
}

[[nodiscard]] Status startPlatformSockets() {
#ifdef _WIN32
  WSADATA data{};
  const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
  if (result != 0) {
    return fail(StatusCode::Unavailable, "WSAStartup failed: " + osErrorText(result));
  }
#endif
  return okStatus();
}

// Idempotent start-up for the entry points that need a live subsystem.
[[nodiscard]] Status ensureSockets() {
  std::atomic<std::uint32_t>& count = socketRefCount();
  if (count.load(std::memory_order_acquire) != 0) return okStatus();
  std::lock_guard<std::mutex> guard(socketMutex());
  if (count.load(std::memory_order_relaxed) != 0) return okStatus();
  Status status = startPlatformSockets();
  if (!status.ok()) return status;
  count.store(1, std::memory_order_release);
  return okStatus();
}

}  // namespace

Socket::Socket(Socket&& other) noexcept : handle_(other.release()) {}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.release();
  }
  return *this;
}

void Socket::close() noexcept {
  if (handle_ < 0) return;
#ifdef _WIN32
  ::closesocket(fromHandle(handle_));
#else
  ::close(handle_);
#endif
  handle_ = -1;
}

int Socket::release() noexcept {
  const int handle = handle_;
  handle_ = -1;
  return handle;
}

Status initSockets() {
  std::lock_guard<std::mutex> guard(socketMutex());
  std::atomic<std::uint32_t>& count = socketRefCount();
  if (count.load() == 0) {
    Status status = startPlatformSockets();
    if (!status.ok()) return status;
  }
  count.fetch_add(1);
  return okStatus();
}

void shutdownSockets() noexcept {
  std::atomic<std::uint32_t>& count = socketRefCount();
  std::uint32_t current = count.load();
  while (current != 0) {
    if (!count.compare_exchange_weak(current, current - 1)) continue;
    if (current == 1) {
#ifdef _WIN32
      ::WSACleanup();
#endif
    }
    return;
  }
}

Status socketFailure(std::string_view what) {
  std::string message(what);
  message += " failed: ";
  message += osErrorText(lastSocketError());
  return fail(StatusCode::IoError, std::move(message));
}

Status listenTcp(const ListenOptions& options, Socket& out, std::uint16_t& boundPort) {
  if (options.address.empty()) {
    return fail(StatusCode::InvalidArgument, "listen address must not be empty");
  }
  if (options.backlog == 0) {
    return fail(StatusCode::InvalidArgument, "listen backlog must be at least one");
  }
  in_addr address{};
  if (::inet_pton(AF_INET, options.address.c_str(), &address) != 1) {
    return fail(StatusCode::InvalidArgument,
                "listen address is not an IPv4 literal: " + options.address);
  }
  Status ready = ensureSockets();
  if (!ready.ok()) return ready;
  const NativeSocket handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == kInvalidNativeSocket) return socketFailure("socket");
  Socket owner(toHandle(handle));
#ifdef _WIN32
  const BOOL reuse = TRUE;
  const int reuseResult = ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
                                       reinterpret_cast<const char*>(&reuse),
                                       static_cast<SockLen>(sizeof(reuse)));
#else
  const int reuse = 1;
  const int reuseResult = ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &reuse,
                                       static_cast<SockLen>(sizeof(reuse)));
#endif
  if (reuseResult != 0) return socketFailure("setsockopt(SO_REUSEADDR)");
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr = address;
  local.sin_port = ::htons(options.port);
  if (::bind(handle, reinterpret_cast<const sockaddr*>(&local),
             static_cast<SockLen>(sizeof(local))) != 0) {
    return socketFailure("bind");
  }
  const std::uint32_t backlog = options.backlog > 0x7FFF'FFFFu ? 0x7FFF'FFFFu : options.backlog;
  if (::listen(handle, static_cast<int>(backlog)) != 0) return socketFailure("listen");
  sockaddr_in assigned{};
  SockLen assignedLength = static_cast<SockLen>(sizeof(assigned));
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&assigned), &assignedLength) != 0) {
    return socketFailure("getsockname");
  }
  boundPort = ::ntohs(assigned.sin_port);
  out = std::move(owner);
  return okStatus();
}

Status acceptTcp(const Socket& listener, std::uint32_t pollMillis, Socket& out,
                 std::string& peer) {
  peer.clear();
  if (!listener.valid()) {
    return fail(StatusCode::InvalidArgument, "accept requires an open listening socket");
  }
  const NativeSocket handle = fromHandle(listener.handle());
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(handle, &readable);
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(pollMillis / 1000u);
  timeout.tv_usec = static_cast<long>((pollMillis % 1000u) * 1000u);
  const int ready =
      ::select(static_cast<int>(listener.handle()) + 1, &readable, nullptr, nullptr, &timeout);
  if (ready == 0) return fail(StatusCode::Unavailable, "accept poll timed out");
  if (ready < 0) {
#ifdef _WIN32
    return socketFailure("select");
#else
    if (errno == EINTR) return fail(StatusCode::Unavailable, "accept poll was interrupted");
    return socketFailure("select");
#endif
  }
  sockaddr_in remote{};
  SockLen remoteLength = static_cast<SockLen>(sizeof(remote));
  const NativeSocket accepted =
      ::accept(handle, reinterpret_cast<sockaddr*>(&remote), &remoteLength);
  if (accepted == kInvalidNativeSocket) return socketFailure("accept");
  Socket owner(toHandle(accepted));
  char text[INET_ADDRSTRLEN] = {};
  if (::inet_ntop(AF_INET, &remote.sin_addr, text, static_cast<NameLen>(sizeof(text))) != nullptr) {
    peer.assign(text);
    peer.push_back(':');
    peer += std::to_string(static_cast<unsigned>(::ntohs(remote.sin_port)));
  } else {
    peer.assign("unknown");
  }
  out = std::move(owner);
  return okStatus();
}

Status connectTcp(const std::string& host, std::uint16_t port, Socket& out) {
  if (host.empty()) {
    return fail(StatusCode::InvalidArgument, "connect host must not be empty");
  }
  if (port == 0) {
    return fail(StatusCode::InvalidArgument, "connect port must not be zero");
  }
  Status ready = ensureSockets();
  if (!ready.ok()) return ready;
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = ::htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &remote.sin_addr) != 1) {
    // Not a literal: let the resolver try, so a name such as "localhost" works.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(static_cast<unsigned>(port));
    const int resolved = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
    if (resolved != 0 || results == nullptr) {
      if (results != nullptr) ::freeaddrinfo(results);
      return fail(StatusCode::NotFound, "cannot resolve host: " + host);
    }
    bool found = false;
    for (const addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
      if (entry->ai_addr == nullptr ||
          entry->ai_addrlen < static_cast<NameLen>(sizeof(sockaddr_in))) {
        continue;
      }
      const auto* candidate = reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
      if (candidate->sin_family != AF_INET) continue;
      remote.sin_addr = candidate->sin_addr;
      found = true;
      break;
    }
    ::freeaddrinfo(results);
    if (!found) return fail(StatusCode::NotFound, "no IPv4 address for host: " + host);
  }
  const NativeSocket handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == kInvalidNativeSocket) return socketFailure("socket");
  Socket owner(toHandle(handle));
  if (::connect(handle, reinterpret_cast<const sockaddr*>(&remote),
                static_cast<SockLen>(sizeof(remote))) != 0) {
    return socketFailure("connect");
  }
  out = std::move(owner);
  return okStatus();
}

Status sendAll(const Socket& socket, std::span<const std::uint8_t> data) {
  if (!socket.valid()) {
    return fail(StatusCode::InvalidArgument, "send requires an open socket");
  }
  if (data.size() > kMaxTransferBytes) {
    return fail(StatusCode::LimitExceeded, "send span exceeds the 64 MiB transfer limit");
  }
  if (data.empty()) return okStatus();
  const NativeSocket handle = fromHandle(socket.handle());
  std::size_t offset = 0;
  while (offset < data.size()) {
    const int chunk = static_cast<int>(data.size() - offset);
    const int sent =
        ::send(handle, reinterpret_cast<const char*>(data.data() + offset), chunk, kSendFlags);
    if (sent > 0) {
      offset += static_cast<std::size_t>(sent);
      continue;
    }
    if (sent == 0) return fail(StatusCode::IoError, "send transferred zero bytes");
#ifdef _WIN32
    if (::WSAGetLastError() == WSAEINTR) continue;
#else
    if (errno == EINTR) continue;
#endif
    return socketFailure("send");
  }
  return okStatus();
}

Status recvAll(const Socket& socket, std::span<std::uint8_t> data) {
  if (!socket.valid()) {
    return fail(StatusCode::InvalidArgument, "receive requires an open socket");
  }
  if (data.size() > kMaxTransferBytes) {
    return fail(StatusCode::LimitExceeded, "receive span exceeds the 64 MiB transfer limit");
  }
  if (data.empty()) return okStatus();
  const NativeSocket handle = fromHandle(socket.handle());
  std::size_t offset = 0;
  while (offset < data.size()) {
    const int chunk = static_cast<int>(data.size() - offset);
    const int received = ::recv(handle, reinterpret_cast<char*>(data.data() + offset), chunk, 0);
    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      continue;
    }
    if (received == 0) {
      return fail(StatusCode::IoError, "receive hit end of stream before the count was reached");
    }
#ifdef _WIN32
    if (::WSAGetLastError() == WSAEINTR) continue;
#else
    if (errno == EINTR) continue;
#endif
    return socketFailure("recv");
  }
  return okStatus();
}

void shutdownBoth(const Socket& socket) noexcept {
  if (!socket.valid()) return;
  ::shutdown(fromHandle(socket.handle()), kShutdownBoth);
}

Status setNoDelay(const Socket& socket) {
  if (!socket.valid()) {
    return fail(StatusCode::InvalidArgument, "setNoDelay requires an open socket");
  }
  const NativeSocket handle = fromHandle(socket.handle());
#ifdef _WIN32
  const BOOL enabled = TRUE;
  const int result = ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY,
                                  reinterpret_cast<const char*>(&enabled),
                                  static_cast<SockLen>(sizeof(enabled)));
#else
  const int enabled = 1;
  const int result = ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, &enabled,
                                  static_cast<SockLen>(sizeof(enabled)));
#endif
  if (result != 0) return socketFailure("setsockopt(TCP_NODELAY)");
  return okStatus();
}

std::int64_t wallClockNanos() noexcept {
#ifdef _WIN32
  FILETIME fileTime{};
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0602
  ::GetSystemTimePreciseAsFileTime(&fileTime);
#else
  ::GetSystemTimeAsFileTime(&fileTime);
#endif
  ULARGE_INTEGER ticks{};
  ticks.LowPart = fileTime.dwLowDateTime;
  ticks.HighPart = fileTime.dwHighDateTime;
  // FILETIME counts 100 ns ticks from 1601-01-01; the epoch offset below
  // converts them to nanoseconds since 1970-01-01.
  constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ull;
  if (ticks.QuadPart < kUnixEpochTicks) return 0;
  return static_cast<std::int64_t>((ticks.QuadPart - kUnixEpochTicks) * 100ull);
#else
  timespec now{};
  if (::clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
  return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000ll +
         static_cast<std::int64_t>(now.tv_nsec);
#endif
}

}  // namespace wavelength_fabric
