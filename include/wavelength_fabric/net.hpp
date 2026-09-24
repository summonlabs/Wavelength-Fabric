#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "wavelength_fabric/error.hpp"

// Minimal loopback TCP helpers.
//
// This is the reference deployment transport. The core runtime never includes
// it, never opens a socket, and never spawns a thread.

namespace wavelength_fabric {

class Socket {
 public:
  Socket() noexcept = default;
  explicit Socket(int handle) noexcept : handle_(handle) {}
  ~Socket() { close(); }

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept { return handle_ >= 0; }
  [[nodiscard]] int handle() const noexcept { return handle_; }

  void close() noexcept;
  // Relinquishes ownership without closing.
  [[nodiscard]] int release() noexcept;

 private:
  int handle_{-1};
};

// Process-wide socket subsystem. Calling this more than once is harmless.
[[nodiscard]] Status initSockets();
void shutdownSockets() noexcept;

[[nodiscard]] Status socketFailure(std::string_view what);

struct ListenOptions {
  std::string address{"127.0.0.1"};
  std::uint16_t port{0};
  std::uint32_t backlog{16};
};

[[nodiscard]] Status listenTcp(const ListenOptions& options, Socket& out, std::uint16_t& boundPort);

// Waits up to pollMillis for an inbound connection. Returns a Status with code
// Unavailable when nothing arrived, which is not an error condition for the
// caller's accept loop.
[[nodiscard]] Status acceptTcp(const Socket& listener, std::uint32_t pollMillis, Socket& out, std::string& peer);

[[nodiscard]] Status connectTcp(const std::string& host, std::uint16_t port, Socket& out);

// Sends/receives exactly count bytes. A short transfer is a failure, never a
// silently truncated success.
[[nodiscard]] Status sendAll(const Socket& socket, std::span<const std::uint8_t> data);
[[nodiscard]] Status recvAll(const Socket& socket, std::span<std::uint8_t> data);

void shutdownBoth(const Socket& socket) noexcept;
[[nodiscard]] Status setNoDelay(const Socket& socket);

// Wall-clock nanoseconds since the Unix epoch, used only for readiness
// handshakes between real processes.
[[nodiscard]] std::int64_t wallClockNanos() noexcept;

}  // namespace wavelength_fabric
