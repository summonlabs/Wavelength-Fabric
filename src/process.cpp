#include "wavelength_fabric/process.hpp"

#include "wavelength_fabric/quantity.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Real child processes behind the opaque ChildProcess handle.
//
// The state is a pimpl so no platform header reaches a caller. Captured output
// is bounded before it is stored, every handle is released on every path, and
// the reader threads are joined exactly once: a harness that leaks a pipe or
// joins a thread twice would wedge the crash/restart proofs.

namespace wavelength_fabric {
namespace {

// Captured output lives in memory, so it is bounded. A reader keeps draining
// past the bound (the child must never block on a full pipe) but drops the
// excess and records that it did.
constexpr std::size_t kMaxCaptureBytes = 1024u * 1024u;
constexpr std::string_view kTruncationMarker = "\n[output truncated]\n";

#ifdef _WIN32
constexpr char kPathSeparator = '\\';
#else
constexpr char kPathSeparator = '/';
#endif

struct CaptureBuffer {
  std::mutex mutex;
  std::string text;
  bool truncated{false};
};

void appendCapture(CaptureBuffer& buffer, const char* data, std::size_t size) {
  if (size == 0) return;
  std::lock_guard<std::mutex> guard(buffer.mutex);
  const std::size_t room =
      buffer.text.size() < kMaxCaptureBytes ? kMaxCaptureBytes - buffer.text.size() : 0;
  if (room == 0) {
    buffer.truncated = true;
    return;
  }
  buffer.text.append(data, size < room ? size : room);
  if (size > room) buffer.truncated = true;
}

void finishCapture(CaptureBuffer& buffer) {
  std::lock_guard<std::mutex> guard(buffer.mutex);
  if (buffer.truncated) buffer.text.append(kTruncationMarker);
}

#ifdef _WIN32

// Windows caps a command line at 32767 characters, and a byte of UTF-8 becomes
// at least one character, so bounding bytes first keeps the wide form inside
// the limit. The same bound caps every path this file converts.
constexpr std::int64_t kMaxCommandLineBytes = 32767;
constexpr std::size_t kMaxWideChars = 32768;
constexpr std::size_t kInitialPathChars = 260;
constexpr std::size_t kMaxPathChars = 32768;
constexpr int kKilledExitCode = 1;

[[nodiscard]] int lastOsError() noexcept {
  return static_cast<int>(::GetLastError());
}

[[nodiscard]] std::string osErrorText(int code) {
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
}

[[nodiscard]] bool toWide(const std::string& text, std::wstring& out) {
  if (text.empty()) {
    out.clear();
    return true;
  }
  if (text.size() >= kMaxWideChars) return false;
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return false;
  out.resize(static_cast<std::size_t>(size));
  const int written = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()), out.data(), size);
  return written == size;
}

[[nodiscard]] std::string toUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  if (text.size() >= kMaxWideChars) return {};
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  const int written = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()), result.data(), size,
                                            nullptr, nullptr);
  if (written != size) return {};
  return result;
}

// Quotes one argument for a Windows command line: the rule CreateProcessW
// documents, where backslashes are doubled only in front of a quote.
[[nodiscard]] std::string quoteArgument(const std::string& argument) {
  if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string::npos) {
    return argument;
  }
  std::string quoted;
  quoted.push_back('"');
  std::size_t backslashes = 0;
  for (const char character : argument) {
    if (character == '\\') {
      ++backslashes;
      continue;
    }
    if (character == '"') {
      quoted.append(backslashes * 2 + 1, '\\');
      quoted.push_back('"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, '\\');
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes * 2, '\\');
  quoted.push_back('"');
  return quoted;
}

// Appends one quoted argument while tracking the exact length, so an oversized
// command line is rejected before it is converted or handed to the system.
[[nodiscard]] bool appendArgument(std::string& commandLine, const std::string& argument,
                                  std::int64_t& length) {
  if (argument.size() > static_cast<std::size_t>(kMaxCommandLineBytes)) return false;
  const std::string quoted = quoteArgument(argument);
  std::int64_t next = 0;
  if (addOverflow(length, static_cast<std::int64_t>(quoted.size()), next)) return false;
  if (length != 0 && addOverflow(next, 1, next)) return false;
  if (next > kMaxCommandLineBytes) return false;
  if (length != 0) commandLine.push_back(' ');
  commandLine += quoted;
  length = next;
  return true;
}

void readPipeInto(HANDLE pipe, CaptureBuffer& buffer) noexcept {
  char chunk[4096];
  try {
    for (;;) {
      DWORD received = 0;
      const BOOL ok =
          ::ReadFile(pipe, chunk, static_cast<DWORD>(sizeof(chunk)), &received, nullptr);
      if (ok == FALSE || received == 0) break;
      appendCapture(buffer, chunk, static_cast<std::size_t>(received));
    }
    finishCapture(buffer);
  } catch (...) {
    // A reader thread must never let an exception escape; the captured text
    // stays bounded and the tail is simply lost.
  }
  ::CloseHandle(pipe);
}

#else

constexpr std::size_t kInitialPathChars = 260;
constexpr std::size_t kMaxPathChars = 32768;

[[nodiscard]] int lastOsError() noexcept { return errno; }

[[nodiscard]] std::string osErrorText(int code) {
  if (code == 0) return "success";
  return std::string(std::strerror(code)) + " (errno " + std::to_string(code) + ")";
}

void readPipeInto(int descriptor, CaptureBuffer& buffer) noexcept {
  char chunk[4096];
  try {
    for (;;) {
      const ssize_t received = ::read(descriptor, chunk, sizeof(chunk));
      if (received > 0) {
        appendCapture(buffer, chunk, static_cast<std::size_t>(received));
        continue;
      }
      if (received < 0 && errno == EINTR) continue;
      break;
    }
    finishCapture(buffer);
  } catch (...) {
    // A reader thread must never let an exception escape; the captured text
    // stays bounded and the tail is simply lost.
  }
  ::close(descriptor);
}

#endif

[[nodiscard]] Status processFailure(std::string_view what) {
  std::string message(what);
  message += " failed: ";
  message += osErrorText(lastOsError());
  return fail(StatusCode::IoError, std::move(message));
}

}  // namespace

struct ChildProcess::Impl {
#ifdef _WIN32
  HANDLE process{nullptr};
  HANDLE stdoutRead{nullptr};
  HANDLE stderrRead{nullptr};
#else
  int stdoutRead{-1};
  int stderrRead{-1};
#endif
  CaptureBuffer out;
  CaptureBuffer err;
  std::thread outReader;
  std::thread errReader;

  // Closes only what a reader thread never took over: a handle handed to a
  // reader is closed by that reader when it reaches end of file.
  ~Impl() {
#ifdef _WIN32
    if (process != nullptr) ::CloseHandle(process);
    if (stdoutRead != nullptr) ::CloseHandle(stdoutRead);
    if (stderrRead != nullptr) ::CloseHandle(stderrRead);
#else
    if (stdoutRead >= 0) ::close(stdoutRead);
    if (stderrRead >= 0) ::close(stderrRead);
#endif
  }
};

ChildProcess::~ChildProcess() {
  if (impl_ == nullptr) return;
  if (started_ && !waited_) {
    // A live child would leave both readers blocked on a pipe that its own
    // exit closes, so the destructor terminates it first: the joins below stay
    // bounded and no process, handle or thread outlives this object.
    int ignored = 0;
    (void)kill();
    (void)wait(ignored);
  }
  joinReaders();
  delete impl_;
  impl_ = nullptr;
  pid_ = 0;
  started_ = false;
  waited_ = false;
  exitCode_ = 0;
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : impl_(other.impl_),
      pid_(other.pid_),
      started_(other.started_),
      waited_(other.waited_),
      exitCode_(other.exitCode_) {
  other.impl_ = nullptr;
  other.pid_ = 0;
  other.started_ = false;
  other.waited_ = false;
  other.exitCode_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    // The displaced child is torn down by this temporary's destructor, which
    // keeps one implementation of the release path instead of two.
    ChildProcess previous;
    previous.impl_ = impl_;
    previous.pid_ = pid_;
    previous.started_ = started_;
    previous.waited_ = waited_;
    previous.exitCode_ = exitCode_;
    impl_ = other.impl_;
    pid_ = other.pid_;
    started_ = other.started_;
    waited_ = other.waited_;
    exitCode_ = other.exitCode_;
    other.impl_ = nullptr;
    other.pid_ = 0;
    other.started_ = false;
    other.waited_ = false;
    other.exitCode_ = 0;
  }
  return *this;
}

Status ChildProcess::start(const std::string& executable, const std::vector<std::string>& args) {
  if (started_) {
    return fail(StatusCode::IllegalTransition, "child process has already been started");
  }
  if (executable.empty()) {
    return fail(StatusCode::InvalidArgument, "executable path must not be empty");
  }
  std::unique_ptr<Impl> impl(new (std::nothrow) Impl());
  if (impl == nullptr) {
    return fail(StatusCode::LimitExceeded, "cannot allocate child process state");
  }

#ifdef _WIN32
  std::string commandLine;
  std::int64_t commandLength = 0;
  if (!appendArgument(commandLine, executable, commandLength)) {
    return fail(StatusCode::LimitExceeded, "command line exceeds the 32767 byte limit");
  }
  for (const std::string& argument : args) {
    if (!appendArgument(commandLine, argument, commandLength)) {
      return fail(StatusCode::LimitExceeded, "command line exceeds the 32767 byte limit");
    }
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = static_cast<DWORD>(sizeof(attributes));
  attributes.lpSecurityDescriptor = nullptr;
  attributes.bInheritHandle = TRUE;

  HANDLE outRead = nullptr;
  HANDLE outWrite = nullptr;
  HANDLE errRead = nullptr;
  HANDLE errWrite = nullptr;
  // Releases whichever pipe ends this function still owns.
  const auto abandonPipes = [&outRead, &outWrite, &errRead, &errWrite]() noexcept {
    if (outRead != nullptr) {
      ::CloseHandle(outRead);
      outRead = nullptr;
    }
    if (outWrite != nullptr) {
      ::CloseHandle(outWrite);
      outWrite = nullptr;
    }
    if (errRead != nullptr) {
      ::CloseHandle(errRead);
      errRead = nullptr;
    }
    if (errWrite != nullptr) {
      ::CloseHandle(errWrite);
      errWrite = nullptr;
    }
  };

  if (::CreatePipe(&outRead, &outWrite, &attributes, 0) == FALSE) {
    return processFailure("CreatePipe");
  }
  if (::CreatePipe(&errRead, &errWrite, &attributes, 0) == FALSE) {
    const Status status = processFailure("CreatePipe");
    abandonPipes();
    return status;
  }
  if (::SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0) == FALSE ||
      ::SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0) == FALSE) {
    const Status status = processFailure("SetHandleInformation");
    abandonPipes();
    return status;
  }

  std::wstring wideExecutable;
  std::wstring wideCommandLine;
  if (!toWide(executable, wideExecutable) || !toWide(commandLine, wideCommandLine)) {
    const Status status =
        fail(StatusCode::InvalidArgument, "executable or arguments are not valid UTF-8");
    abandonPipes();
    return status;
  }

  // A bare name is searched for on PATH, which is what execvp does on the
  // POSIX side; a name that already carries a directory is used verbatim.
  std::wstring resolvedExecutable;
  if (executable.find_first_of("\\/") == std::string::npos) {
    std::wstring searchBuffer(kMaxPathChars, L'\0');
    const DWORD found =
        ::SearchPathW(nullptr, wideExecutable.c_str(), L".exe",
                      static_cast<DWORD>(searchBuffer.size()), searchBuffer.data(), nullptr);
    if (found != 0 && static_cast<std::size_t>(found) < searchBuffer.size()) {
      resolvedExecutable.assign(searchBuffer.data(), static_cast<std::size_t>(found));
    }
  }
  const wchar_t* applicationName =
      resolvedExecutable.empty() ? wideExecutable.c_str() : resolvedExecutable.c_str();

  STARTUPINFOW startup{};
  startup.cb = static_cast<DWORD>(sizeof(startup));
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = outWrite;
  startup.hStdError = errWrite;

  PROCESS_INFORMATION info{};
  const BOOL created = ::CreateProcessW(applicationName, wideCommandLine.data(), nullptr,
                                        nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                        &startup, &info);
  // The parent must drop the write ends now, or the readers never see the end
  // of the stream once the child exits.
  if (outWrite != nullptr) {
    ::CloseHandle(outWrite);
    outWrite = nullptr;
  }
  if (errWrite != nullptr) {
    ::CloseHandle(errWrite);
    errWrite = nullptr;
  }
  if (created == FALSE) {
    const Status status = processFailure("CreateProcessW");
    abandonPipes();
    return status;
  }
  ::CloseHandle(info.hThread);
  impl->process = info.hProcess;

  try {
    impl->outReader = std::thread([buffer = &impl->out, pipe = outRead]() noexcept {
      readPipeInto(pipe, *buffer);
    });
    outRead = nullptr;
    impl->errReader = std::thread([buffer = &impl->err, pipe = errRead]() noexcept {
      readPipeInto(pipe, *buffer);
    });
    errRead = nullptr;
  } catch (...) {
    // Nothing drains the pipes of a child that is already running, so it is
    // terminated first; the reader that did start then reaches end of file and
    // can be joined without waiting on a live process.
    ::TerminateProcess(impl->process, static_cast<UINT>(kKilledExitCode));
    ::WaitForSingleObject(impl->process, INFINITE);
    if (impl->outReader.joinable()) impl->outReader.join();
    if (impl->errReader.joinable()) impl->errReader.join();
    abandonPipes();
    ::CloseHandle(impl->process);
    impl->process = nullptr;
    return fail(StatusCode::IoError, "cannot start the pipe reader threads");
  }
  pid_ = static_cast<std::uint64_t>(info.dwProcessId);
#else
  int outPipe[2] = {-1, -1};
  int errPipe[2] = {-1, -1};
  if (::pipe(outPipe) != 0) return processFailure("pipe");
  if (::pipe(errPipe) != 0) {
    const Status status = processFailure("pipe");
    ::close(outPipe[0]);
    ::close(outPipe[1]);
    return status;
  }
  // The read ends belong to this process alone and must not leak into any
  // later exec; the child closes them explicitly before it execs.
  (void)::fcntl(outPipe[0], F_SETFD, FD_CLOEXEC);
  (void)::fcntl(errPipe[0], F_SETFD, FD_CLOEXEC);

  std::vector<char*> argv;
  argv.reserve(args.size() + 2);
  argv.push_back(const_cast<char*>(executable.c_str()));
  for (const std::string& argument : args) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);

  const pid_t child = ::fork();
  if (child < 0) {
    const Status status = processFailure("fork");
    ::close(outPipe[0]);
    ::close(outPipe[1]);
    ::close(errPipe[0]);
    ::close(errPipe[1]);
    return status;
  }
  if (child == 0) {
    // Only async-signal-safe calls run between fork and exec: dup2, close and
    // execvp take no lock that another thread could have held at fork time.
    ::close(outPipe[0]);
    ::close(errPipe[0]);
    if (::dup2(outPipe[1], STDOUT_FILENO) < 0 || ::dup2(errPipe[1], STDERR_FILENO) < 0) {
      ::_exit(127);
    }
    if (outPipe[1] > STDERR_FILENO) ::close(outPipe[1]);
    if (errPipe[1] > STDERR_FILENO) ::close(errPipe[1]);
    ::execvp(executable.c_str(), argv.data());
    ::_exit(127);
  }
  (void)::close(outPipe[1]);
  (void)::close(errPipe[1]);
  impl->stdoutRead = outPipe[0];
  impl->stderrRead = errPipe[0];

  try {
    impl->outReader = std::thread([descriptor = outPipe[0], buffer = &impl->out]() noexcept {
      readPipeInto(descriptor, *buffer);
    });
    impl->stdoutRead = -1;
    impl->errReader = std::thread([descriptor = errPipe[0], buffer = &impl->err]() noexcept {
      readPipeInto(descriptor, *buffer);
    });
    impl->stderrRead = -1;
  } catch (...) {
    ::kill(child, SIGKILL);
    int childStatus = 0;
    while (::waitpid(child, &childStatus, 0) < 0 && errno == EINTR) {
    }
    if (impl->outReader.joinable()) impl->outReader.join();
    if (impl->errReader.joinable()) impl->errReader.join();
    if (impl->stdoutRead >= 0) {
      ::close(impl->stdoutRead);
      impl->stdoutRead = -1;
    }
    if (impl->stderrRead >= 0) {
      ::close(impl->stderrRead);
      impl->stderrRead = -1;
    }
    return fail(StatusCode::IoError, "cannot start the pipe reader threads");
  }
  pid_ = static_cast<std::uint64_t>(child);
#endif

  impl_ = impl.release();
  started_ = true;
  waited_ = false;
  exitCode_ = 0;
  return okStatus();
}

bool ChildProcess::running() {
  if (!started_ || waited_ || impl_ == nullptr) return false;
#ifdef _WIN32
  DWORD code = 0;
  if (::GetExitCodeProcess(impl_->process, &code) == FALSE) return false;
  return code == STILL_ACTIVE;
#else
  if (::kill(static_cast<pid_t>(pid_), 0) == 0) return true;
  return errno == EPERM;
#endif
}

Status ChildProcess::kill() {
  if (!started_) {
    return fail(StatusCode::InvalidArgument, "child process was never started");
  }
  if (waited_ || impl_ == nullptr) return okStatus();
#ifdef _WIN32
  if (impl_->process == nullptr) return okStatus();
  DWORD code = 0;
  if (::GetExitCodeProcess(impl_->process, &code) == FALSE) {
    return processFailure("GetExitCodeProcess");
  }
  // A child that already ended is not an error: kill is idempotent, and
  // terminating an exited process would only report access denied.
  if (code != STILL_ACTIVE) return okStatus();
  if (::TerminateProcess(impl_->process, static_cast<UINT>(kKilledExitCode)) == FALSE) {
    return processFailure("TerminateProcess");
  }
  return okStatus();
#else
  if (::kill(static_cast<pid_t>(pid_), SIGKILL) == 0) return okStatus();
  if (errno == ESRCH) return okStatus();
  return processFailure("kill");
#endif
}

Status ChildProcess::wait(int& exitCode) {
  if (!started_) {
    return fail(StatusCode::InvalidArgument, "child process was never started");
  }
  if (waited_) {
    exitCode = exitCode_;
    return okStatus();
  }
#ifdef _WIN32
  if (impl_ == nullptr || impl_->process == nullptr) {
    return fail(StatusCode::Unknown, "child process handle is not available");
  }
  const DWORD result = ::WaitForSingleObject(impl_->process, INFINITE);
  if (result == WAIT_FAILED) return processFailure("WaitForSingleObject");
  if (result != WAIT_OBJECT_0) {
    return fail(StatusCode::Unknown, "unexpected result from WaitForSingleObject");
  }
  DWORD code = 0;
  if (::GetExitCodeProcess(impl_->process, &code) == FALSE) {
    return processFailure("GetExitCodeProcess");
  }
  ::CloseHandle(impl_->process);
  impl_->process = nullptr;
  joinReaders();
  exitCode_ = static_cast<int>(code);
#else
  int status = 0;
  for (;;) {
    const pid_t reaped = ::waitpid(static_cast<pid_t>(pid_), &status, 0);
    if (reaped == static_cast<pid_t>(pid_)) break;
    if (reaped < 0 && errno == EINTR) continue;
    return processFailure("waitpid");
  }
  int code = 0;
  if (WIFEXITED(status)) {
    code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    code = 128 + WTERMSIG(status);
  }
  joinReaders();
  exitCode_ = code;
#endif
  waited_ = true;
  exitCode = exitCode_;
  return okStatus();
}

void ChildProcess::joinReaders() {
  if (impl_ == nullptr) return;
  const std::thread::id self = std::this_thread::get_id();
  std::thread* readers[2] = {&impl_->outReader, &impl_->errReader};
  for (std::thread* reader : readers) {
    if (!reader->joinable()) continue;
    if (reader->get_id() == self) {
      // Joining a thread from itself would deadlock, so a reader that reaches
      // this path detaches instead; the guard exists for exactly that case.
      reader->detach();
      continue;
    }
    reader->join();
  }
}

std::string ChildProcess::stdoutText() const {
  if (impl_ == nullptr) return {};
  std::lock_guard<std::mutex> guard(impl_->out.mutex);
  return impl_->out.text;
}

std::string ChildProcess::stderrText() const {
  if (impl_ == nullptr) return {};
  std::lock_guard<std::mutex> guard(impl_->err.mutex);
  return impl_->err.text;
}

std::uint64_t currentProcessId() noexcept {
#ifdef _WIN32
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string currentExecutablePath() {
#ifdef _WIN32
  std::wstring buffer(kInitialPathChars, L'\0');
  for (;;) {
    const DWORD written =
        ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0) return {};
    if (static_cast<std::size_t>(written) < buffer.size()) {
      buffer.resize(static_cast<std::size_t>(written));
      break;
    }
    if (buffer.size() >= kMaxPathChars) return {};
    buffer.resize(buffer.size() * 2);
  }
  return toUtf8(buffer);
#else
  std::string buffer(kInitialPathChars, '\0');
  for (;;) {
    const ssize_t written = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (written < 0) return {};
    if (static_cast<std::size_t>(written) < buffer.size()) {
      buffer.resize(static_cast<std::size_t>(written));
      return buffer;
    }
    if (buffer.size() >= kMaxPathChars) return {};
    buffer.resize(buffer.size() * 2);
  }
#endif
}

bool pathExists(const std::string& path) {
  if (path.empty()) return false;
#ifdef _WIN32
  std::wstring wide;
  if (!toWide(path, wide)) return false;
  return ::GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
  struct stat info{};
  return ::stat(path.c_str(), &info) == 0;
#endif
}

Status removeFile(const std::string& path) {
  if (path.empty()) {
    return fail(StatusCode::InvalidArgument, "path must not be empty");
  }
#ifdef _WIN32
  std::wstring wide;
  if (!toWide(path, wide)) {
    return fail(StatusCode::InvalidArgument, "path is not valid UTF-8");
  }
  if (::DeleteFileW(wide.c_str()) != FALSE) return okStatus();
  const int code = lastOsError();
  if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
    return fail(StatusCode::NotFound, "cannot remove a missing file: " + path);
  }
  return fail(StatusCode::IoError, "DeleteFileW failed: " + osErrorText(code));
#else
  if (::remove(path.c_str()) == 0) return okStatus();
  const int code = errno;
  if (code == ENOENT) return fail(StatusCode::NotFound, "cannot remove a missing file: " + path);
  return fail(StatusCode::IoError, "remove failed: " + osErrorText(code));
#endif
}

std::string joinPath(const std::string& directory, const std::string& name) {
  if (directory.empty()) return name;
  if (name.empty()) return directory;
  const char last = directory.back();
  if (last == '/' || last == '\\') return directory + name;
  return directory + kPathSeparator + name;
}

std::string temporaryDirectory() {
#ifdef _WIN32
  std::wstring buffer(kInitialPathChars, L'\0');
  for (;;) {
    const DWORD length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0) return ".";
    if (static_cast<std::size_t>(length) < buffer.size()) {
      buffer.resize(static_cast<std::size_t>(length));
      break;
    }
    if (buffer.size() >= kMaxPathChars) return ".";
    buffer.resize(buffer.size() * 2);
  }
  // GetTempPathW keeps the trailing separator; joinPath accepts either shape,
  // but one canonical spelling keeps proof output reproducible.
  while (!buffer.empty() && (buffer.back() == L'\\' || buffer.back() == L'/')) {
    buffer.pop_back();
  }
  if (buffer.empty()) return ".";
  return toUtf8(buffer);
#else
  const char* fromEnvironment = std::getenv("TMPDIR");
  if (fromEnvironment != nullptr && fromEnvironment[0] != '\0') {
    return std::string(fromEnvironment);
  }
  return "/tmp";
#endif
}

}  // namespace wavelength_fabric
