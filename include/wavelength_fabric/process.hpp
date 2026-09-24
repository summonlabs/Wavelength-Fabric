#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wavelength_fabric/error.hpp"

// Real operating-system child processes.
//
// This is a deployment-mechanism utility used by the multiprocess proofs and
// the crash/restart tests. It is not part of the core runtime: the core never
// spawns, kills or waits on a process.

namespace wavelength_fabric {

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;

  // Starts executable with args (args excludes argv[0]). Standard output and
  // standard error are captured into bounded in-memory buffers.
  [[nodiscard]] Status start(const std::string& executable, const std::vector<std::string>& args);

  [[nodiscard]] bool running();
  [[nodiscard]] bool started() const noexcept { return started_; }
  [[nodiscard]] std::uint64_t pid() const noexcept { return pid_; }

  // Abrupt termination: TerminateProcess on Windows, SIGKILL elsewhere. This
  // is a real kill with no cooperative shutdown, used to prove that a restarted
  // runtime fences the dead incarnation.
  [[nodiscard]] Status kill();

  // Waits for termination and records the exit code. Idempotent.
  [[nodiscard]] Status wait(int& exitCode);

  [[nodiscard]] std::string stdoutText() const;
  [[nodiscard]] std::string stderrText() const;

 private:
  void joinReaders();

  struct Impl;
  Impl* impl_{nullptr};
  std::uint64_t pid_{0};
  bool started_{false};
  bool waited_{false};
  int exitCode_{0};
};

// Host facts used by the proofs.
[[nodiscard]] std::uint64_t currentProcessId() noexcept;
[[nodiscard]] std::string currentExecutablePath();
[[nodiscard]] bool pathExists(const std::string& path);
[[nodiscard]] Status removeFile(const std::string& path);
[[nodiscard]] std::string joinPath(const std::string& directory, const std::string& name);
[[nodiscard]] std::string temporaryDirectory();

}  // namespace wavelength_fabric
