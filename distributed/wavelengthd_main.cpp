// wavelengthd - the reference Wavelength Fabric server process.
//
// The daemon owns one SpectrumRuntime and serves the framed TCP protocol. It is
// a deployment mechanism: the core runtime never opens a socket.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include <wavelength_fabric/process.hpp>
#include <wavelength_fabric/protocol.hpp>
#include <wavelength_fabric/runtime.hpp>
#include <wavelength_fabric/text.hpp>

namespace {

struct Options {
  std::string address{"127.0.0.1"};
  std::uint16_t port{0};
  std::string statePath;
  std::uint32_t workers{4};
  bool durable{false};
  bool fsync{true};
  bool allowShutdown{true};
};

[[nodiscard]] bool parseUnsigned(const char* text, std::uint64_t& value) {
  if (text == nullptr || *text == '\0') return false;
  std::uint64_t result = 0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(*cursor - '0');
    if (result > (0xFFFF'FFFF'FFFF'FFFFull - digit) / 10ull) return false;
    result = result * 10ull + digit;
  }
  value = result;
  return true;
}

[[nodiscard]] bool parseOptions(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto next = [&](const char*& out) -> bool {
      if (index + 1 >= argc) return false;
      out = argv[++index];
      return true;
    };
    const char* value = nullptr;
    if (argument == "--address") {
      if (!next(value)) return false;
      options.address = value;
    } else if (argument == "--port") {
      if (!next(value)) return false;
      std::uint64_t parsed = 0;
      if (!parseUnsigned(value, parsed) || parsed > 65535ull) return false;
      options.port = static_cast<std::uint16_t>(parsed);
    } else if (argument == "--state") {
      if (!next(value)) return false;
      options.statePath = value;
    } else if (argument == "--workers") {
      if (!next(value)) return false;
      std::uint64_t parsed = 0;
      if (!parseUnsigned(value, parsed) || parsed == 0 || parsed > 256ull) return false;
      options.workers = static_cast<std::uint32_t>(parsed);
    } else if (argument == "--durable") {
      options.durable = true;
    } else if (argument == "--no-fsync") {
      options.fsync = false;
    } else if (argument == "--no-shutdown-op") {
      options.allowShutdown = false;
    } else {
      std::cerr << "unrecognised argument: " << argument << "\n";
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parseOptions(argc, argv, options)) {
    std::cerr << "usage: wavelengthd --port <port> [--address <ip>] [--state <path>] "
                 "[--workers <n>] [--durable] [--no-fsync] [--no-shutdown-op]\n";
    return 2;
  }

  wavelength_fabric::RuntimeConfig config;
  config.statePath = options.statePath;
  config.durableCommits = options.durable && !options.statePath.empty();
  config.fsyncState = options.fsync;

  wavelength_fabric::SpectrumRuntime runtime(config);

  if (!options.statePath.empty() && wavelength_fabric::pathExists(options.statePath)) {
    const wavelength_fabric::RecoveryReport report = runtime.recover();
    if (!report.recovered) {
      std::cout << "RECOVER-FAILED " << wavelength_fabric::toToken(report.status.code) << " "
                << report.status.message << "\n";
      std::cout.flush();
      return 3;
    }
    std::cout << "RECOVERED reservations=" << report.reservationsRestored
              << " demoted=" << report.demotedFromActive << "\n";
  }

  wavelength_fabric::ServerConfig serverConfig;
  serverConfig.address = options.address;
  serverConfig.port = options.port;
  serverConfig.workerThreads = options.workers;
  serverConfig.allowShutdownOp = options.allowShutdown;

  wavelength_fabric::SpectrumServer server(runtime, serverConfig);
  const wavelength_fabric::Status started = server.start();
  if (!started.ok()) {
    std::cout << "START-FAILED " << wavelength_fabric::toToken(started.code) << " "
              << started.message << "\n";
    std::cout.flush();
    return 4;
  }

  const wavelength_fabric::ControllerFence fence = runtime.fence();
  std::cout << "READY " << server.port() << " " << fence.epoch.raw() << " "
            << fence.incarnation.raw() << "\n";
  std::cout.flush();

  server.serve();
  std::cout << "STOPPED\n";
  std::cout.flush();
  return 0;
}
