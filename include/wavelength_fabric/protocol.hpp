#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "wavelength_fabric/error.hpp"
#include "wavelength_fabric/net.hpp"
#include "wavelength_fabric/runtime.hpp"
#include "wavelength_fabric/serialization.hpp"

// Framed, checksummed protocol for the reference TCP deployment.
//
// The frame layout is fixed and fully validated before any payload is used:
//
//   magic    u32   0x5746314D
//   version  u16   kProtocolVersion
//   opcode   u16
//   request  u64
//   length   u32   payload byte count, bounded by the negotiated maximum
//   crc      u32   CRC-32 of the payload
//   payload  length bytes
//
// A frame whose magic, version, length or checksum is wrong is rejected and
// the connection is closed. Nothing is executed for a rejected frame.

namespace wavelength_fabric {

inline constexpr std::uint32_t kFrameMagic = 0x5746314Du;
inline constexpr std::size_t kFrameHeaderBytes = 24;

enum class OpCode : std::uint16_t {
  Ping = 1,
  Describe = 2,

  RegisterGrid = 10,
  RegisterSpan = 11,
  RegisterPort = 12,
  RegisterDomain = 13,
  RegisterExclusionDomain = 14,
  PublishCapability = 15,

  Enumerate = 20,
  Explain = 21,
  Allocate = 22,
  Renew = 23,
  Activate = 24,
  Deactivate = 25,
  Release = 26,
  ExpireLeases = 27,
  ReclaimExpired = 28,

  QueryReservation = 30,
  QueryReservations = 31,
  QueryUsage = 32,
  QueryUsageAll = 33,
  QueryAudit = 34,
  QueryStats = 35,

  Save = 40,
  Reset = 41,

  Shutdown = 90,
};

[[nodiscard]] std::string_view toToken(OpCode op) noexcept;

struct Frame {
  OpCode op{OpCode::Ping};
  std::uint64_t requestId{0};
  std::vector<std::uint8_t> payload;
};

[[nodiscard]] Status writeFrame(const Socket& socket, const Frame& frame, std::uint32_t maxPayloadBytes);
[[nodiscard]] Status readFrame(const Socket& socket, Frame& frame, std::uint32_t maxPayloadBytes);

struct ServerDescription {
  ControllerFence fence{};
  RuntimeGeneration runtimeGeneration{};
  RecoveryGeneration recoveryGeneration{};
  std::size_t grids{0};
  std::size_t domains{0};
  std::size_t exclusionDomains{0};
  std::size_t capabilities{0};
  std::size_t reservations{0};
  std::size_t auditRecords{0};
  AuditSequence lastAudit{};
  RuntimeStats stats{};
};

struct ServerConfig {
  std::string address{"127.0.0.1"};
  std::uint16_t port{0};
  std::uint32_t maxConnections{64};
  std::uint32_t workerThreads{4};
  std::uint32_t maxPayloadBytes{kMaxFramePayloadBytes};
  std::uint32_t backlog{16};
  std::uint32_t acceptPollMillis{50};
  bool allowShutdownOp{true};
};

class SpectrumServer {
 public:
  SpectrumServer(SpectrumRuntime& runtime, ServerConfig config = {});
  ~SpectrumServer();

  SpectrumServer(const SpectrumServer&) = delete;
  SpectrumServer& operator=(const SpectrumServer&) = delete;

  [[nodiscard]] Status start();
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  void serve();
  void stop();
  [[nodiscard]] bool stopped() const noexcept { return stopping_.load(std::memory_order_acquire); }
  [[nodiscard]] std::uint64_t servedRequests() const noexcept { return served_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t rejectedFrames() const noexcept { return rejected_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint32_t peakConnections() const noexcept { return peakConnections_.load(std::memory_order_relaxed); }

 private:
  void workerLoop();
  void serveConnection(Socket& connection);
  [[nodiscard]] Status dispatch(OpCode op, const std::vector<std::uint8_t>& payload,
                                std::vector<std::uint8_t>& data, bool& shutdownRequested);

  SpectrumRuntime& runtime_;
  ServerConfig config_;
  Socket listener_;
  std::uint16_t port_{0};

  mutable std::mutex mutex_;
  std::condition_variable wakeup_;
  std::deque<Socket> pending_;
  std::vector<std::thread> workers_;
  bool started_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint64_t> served_{0};
  std::atomic<std::uint64_t> rejected_{0};
  std::atomic<std::uint32_t> peakConnections_{0};
  std::atomic<std::uint32_t> activeConnections_{0};
};

// Client-side request arguments.
struct RenewArgs {
  ReservationId id{};
  ReservationGeneration generation{};
  Duration extension{};
  ReservationAuthority authority{};
  Instant now{};
  std::uint32_t maxRenewals{0};
};

struct ActivateArgs {
  ReservationId id{};
  ReservationGeneration generation{};
  ActivationAuthority authority{};
  Instant now{};
};

struct ReleaseArgs {
  ReservationId id{};
  ReservationGeneration generation{};
  ReleaseAuthority authority{};
  Instant now{};
};

struct SweepArgs {
  Instant now{};
};

struct AuditArgs {
  AuditSequence since{};
  std::uint32_t limit{0};
};

class SpectrumClient {
 public:
  SpectrumClient() = default;
  ~SpectrumClient();

  SpectrumClient(const SpectrumClient&) = delete;
  SpectrumClient& operator=(const SpectrumClient&) = delete;

  [[nodiscard]] Status connect(const std::string& host, std::uint16_t port,
                               std::uint32_t maxPayloadBytes = kMaxFramePayloadBytes);
  void close();
  [[nodiscard]] bool connected() const noexcept { return socket_.valid(); }

  [[nodiscard]] Status describe(ServerDescription& out);
  [[nodiscard]] Status ping();

  [[nodiscard]] Status registerGrid(const ChannelGrid& grid);
  [[nodiscard]] Status registerSpan(const Span& span);
  [[nodiscard]] Status registerPort(const OpticalPort& port);
  [[nodiscard]] Status registerDomain(const SpectrumDomain& domain);
  [[nodiscard]] Status registerExclusionDomain(const ExclusionDomain& domain);
  [[nodiscard]] Status publishCapability(const SpectrumCapability& capability);

  [[nodiscard]] Status enumerate(const SpectrumRequest& request, CandidateSet& out);
  [[nodiscard]] Status explain(const SpectrumRequest& request, DecisionExplanation& out);
  [[nodiscard]] Status allocate(const SpectrumRequest& request, AllocationDecision& out);
  [[nodiscard]] Status renew(const RenewArgs& args, SpectrumReservation& out);
  [[nodiscard]] Status activate(const ActivateArgs& args, SpectrumReservation& out);
  [[nodiscard]] Status deactivate(const ActivateArgs& args, SpectrumReservation& out);
  [[nodiscard]] Status release(const ReleaseArgs& args, SpectrumReservation& out);
  [[nodiscard]] Status expireLeases(const SweepArgs& args, ReclaimReport& out);
  [[nodiscard]] Status reclaimExpired(const SweepArgs& args, ReclaimReport& out);

  [[nodiscard]] Status queryReservation(ReservationId id, SpectrumReservation& out);
  [[nodiscard]] Status queryReservations(std::vector<SpectrumReservation>& out);
  [[nodiscard]] Status queryUsage(SpectrumDomainId id, Instant now, SpectrumUsage& out);
  [[nodiscard]] Status queryUsageAll(Instant now, std::vector<SpectrumUsage>& out);
  [[nodiscard]] Status queryAudit(const AuditArgs& args, std::vector<AuditRecord>& out);
  [[nodiscard]] Status queryStats(RuntimeStats& out);

  [[nodiscard]] Status save();
  [[nodiscard]] Status requestShutdown();

  // Raw framed call. Returns the transport status and, on success, the
  // server-side status decoded from the reply envelope.
  [[nodiscard]] Status call(OpCode op, const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& reply);

 private:
  Socket socket_;
  std::uint32_t maxPayloadBytes_{kMaxFramePayloadBytes};
  std::uint64_t nextRequestId_{1};
  std::mutex mutex_;
};

// Spins until the wall clock reaches startAt, then returns. Used by concurrent
// proof clients to make their attempts genuinely simultaneous. Bounded by the
// caller-supplied deadline; exceeding it is reported, never silently ignored.
[[nodiscard]] Status waitUntil(std::int64_t startAtNanos, std::int64_t giveUpAtNanos);

}  // namespace wavelength_fabric
