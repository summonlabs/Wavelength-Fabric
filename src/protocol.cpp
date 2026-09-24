#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "wavelength_fabric/protocol.hpp"

namespace wavelength_fabric {

namespace {

constexpr std::uint32_t kMaxReservationListBytes = 4096;
constexpr std::uint32_t kMaxDescriptionCount = 1u << 20;
constexpr std::uint32_t kMaxUsageList = 1u << 20;
constexpr std::uint32_t kMaxAuditList = 1u << 20;

[[nodiscard]] bool knownOp(std::uint16_t value) noexcept {
  switch (static_cast<OpCode>(value)) {
    case OpCode::Ping:
    case OpCode::Describe:
    case OpCode::RegisterGrid:
    case OpCode::RegisterSpan:
    case OpCode::RegisterPort:
    case OpCode::RegisterDomain:
    case OpCode::RegisterExclusionDomain:
    case OpCode::PublishCapability:
    case OpCode::Enumerate:
    case OpCode::Explain:
    case OpCode::Allocate:
    case OpCode::Renew:
    case OpCode::Activate:
    case OpCode::Deactivate:
    case OpCode::Release:
    case OpCode::ExpireLeases:
    case OpCode::ReclaimExpired:
    case OpCode::QueryReservation:
    case OpCode::QueryReservations:
    case OpCode::QueryUsage:
    case OpCode::QueryUsageAll:
    case OpCode::QueryAudit:
    case OpCode::QueryStats:
    case OpCode::Save:
    case OpCode::Reset:
    case OpCode::Shutdown:
      return true;
  }
  return false;
}

[[nodiscard]] std::vector<std::uint8_t> envelope(const Status& status,
                                                 const std::vector<std::uint8_t>& data) {
  Encoder encoder;
  encode(encoder, status);
  std::vector<std::uint8_t> out = encoder.take();
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

void writeStats(Encoder& out, const RuntimeStats& value) {
  const std::uint64_t counters[] = {
      value.allocationsCommitted, value.allocationsRefused,  value.candidatesEvaluated,
      value.enumerationTruncations, value.renewals,          value.releases,
      value.activations,          value.deactivations,       value.expirationSweeps,
      value.reclamations,         value.persistenceWrites,   value.persistenceFailures,
      value.replayRejections,     value.corruptionDetections};
  for (const std::uint64_t counter : counters) out.u64(counter);
}

bool readStats(Decoder& in, RuntimeStats& value) {
  std::uint64_t counters[14] = {};
  for (std::uint64_t& counter : counters) {
    if (!in.u64(counter)) return false;
  }
  value.allocationsCommitted = counters[0];
  value.allocationsRefused = counters[1];
  value.candidatesEvaluated = counters[2];
  value.enumerationTruncations = counters[3];
  value.renewals = counters[4];
  value.releases = counters[5];
  value.activations = counters[6];
  value.deactivations = counters[7];
  value.expirationSweeps = counters[8];
  value.reclamations = counters[9];
  value.persistenceWrites = counters[10];
  value.persistenceFailures = counters[11];
  value.replayRejections = counters[12];
  value.corruptionDetections = counters[13];
  return true;
}

void writeDescription(Encoder& out, const ServerDescription& value) {
  encode(out, value.fence);
  out.u64(value.runtimeGeneration.raw());
  out.u64(value.recoveryGeneration.raw());
  out.u64(value.grids);
  out.u64(value.domains);
  out.u64(value.exclusionDomains);
  out.u64(value.capabilities);
  out.u64(value.reservations);
  out.u64(value.auditRecords);
  out.u64(value.lastAudit.raw());
  writeStats(out, value.stats);
}

bool readDescription(Decoder& in, ServerDescription& value) {
  std::uint64_t runtimeGeneration = 0;
  std::uint64_t recoveryGeneration = 0;
  std::uint64_t grids = 0;
  std::uint64_t domains = 0;
  std::uint64_t exclusionDomains = 0;
  std::uint64_t capabilities = 0;
  std::uint64_t reservations = 0;
  std::uint64_t auditRecords = 0;
  std::uint64_t lastAudit = 0;
  if (!decode(in, value.fence) || !in.u64(runtimeGeneration) || !in.u64(recoveryGeneration) ||
      !in.u64(grids) || !in.u64(domains) || !in.u64(exclusionDomains) || !in.u64(capabilities) ||
      !in.u64(reservations) || !in.u64(auditRecords) || !in.u64(lastAudit) ||
      !readStats(in, value.stats)) {
    return false;
  }
  if (grids > kMaxDescriptionCount || domains > kMaxDescriptionCount ||
      exclusionDomains > kMaxDescriptionCount || capabilities > kMaxDescriptionCount ||
      reservations > kMaxDescriptionCount || auditRecords > kMaxDescriptionCount) {
    in.fail();
    return false;
  }
  value.runtimeGeneration = RuntimeGeneration(runtimeGeneration);
  value.recoveryGeneration = RecoveryGeneration(recoveryGeneration);
  value.grids = static_cast<std::size_t>(grids);
  value.domains = static_cast<std::size_t>(domains);
  value.exclusionDomains = static_cast<std::size_t>(exclusionDomains);
  value.capabilities = static_cast<std::size_t>(capabilities);
  value.reservations = static_cast<std::size_t>(reservations);
  value.auditRecords = static_cast<std::size_t>(auditRecords);
  value.lastAudit = AuditSequence(lastAudit);
  return true;
}

template <class T, class EncodeFn>
void writeList(Encoder& out, const std::vector<T>& values, EncodeFn encodeOne) {
  out.u32(static_cast<std::uint32_t>(values.size()));
  for (const T& value : values) encodeOne(out, value);
}

template <class T, class DecodeFn>
bool readList(Decoder& in, std::vector<T>& values, std::uint32_t maxCount, DecodeFn decodeOne) {
  std::uint32_t count = 0;
  if (!in.u32(count)) return false;
  if (count > maxCount) {
    in.fail();
    return false;
  }
  values.clear();
  values.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    T value{};
    if (!decodeOne(in, value)) {
      values.clear();
      return false;
    }
    values.push_back(std::move(value));
  }
  return true;
}

}  // namespace

std::string_view toToken(OpCode op) noexcept {
  switch (op) {
    case OpCode::Ping: return "ping";
    case OpCode::Describe: return "describe";
    case OpCode::RegisterGrid: return "register-grid";
    case OpCode::RegisterSpan: return "register-span";
    case OpCode::RegisterPort: return "register-port";
    case OpCode::RegisterDomain: return "register-domain";
    case OpCode::RegisterExclusionDomain: return "register-exclusion-domain";
    case OpCode::PublishCapability: return "publish-capability";
    case OpCode::Enumerate: return "enumerate";
    case OpCode::Explain: return "explain";
    case OpCode::Allocate: return "allocate";
    case OpCode::Renew: return "renew";
    case OpCode::Activate: return "activate";
    case OpCode::Deactivate: return "deactivate";
    case OpCode::Release: return "release";
    case OpCode::ExpireLeases: return "expire-leases";
    case OpCode::ReclaimExpired: return "reclaim-expired";
    case OpCode::QueryReservation: return "query-reservation";
    case OpCode::QueryReservations: return "query-reservations";
    case OpCode::QueryUsage: return "query-usage";
    case OpCode::QueryUsageAll: return "query-usage-all";
    case OpCode::QueryAudit: return "query-audit";
    case OpCode::QueryStats: return "query-stats";
    case OpCode::Save: return "save";
    case OpCode::Reset: return "reset";
    case OpCode::Shutdown: return "shutdown";
  }
  return "unknown";
}

Status writeFrame(const Socket& socket, const Frame& frame, std::uint32_t maxPayloadBytes) {
  if (frame.payload.size() > maxPayloadBytes || frame.payload.size() > kMaxFramePayloadBytes) {
    return fail(StatusCode::LimitExceeded,
                "frame payload of " + std::to_string(frame.payload.size()) +
                    " bytes exceeds the negotiated bound");
  }
  Encoder header;
  header.u32(kFrameMagic);
  header.u16(kProtocolVersion);
  header.u16(static_cast<std::uint16_t>(frame.op));
  header.u64(frame.requestId);
  header.u32(static_cast<std::uint32_t>(frame.payload.size()));
  header.u32(crc32(frame.payload));
  const Status sent = sendAll(socket, header.buffer());
  if (!sent.ok()) return sent;
  if (frame.payload.empty()) return okStatus();
  return sendAll(socket, frame.payload);
}

Status readFrame(const Socket& socket, Frame& frame, std::uint32_t maxPayloadBytes) {
  std::vector<std::uint8_t> header(kFrameHeaderBytes);
  const Status received = recvAll(socket, header);
  if (!received.ok()) return received;

  Decoder decoder(header);
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  std::uint16_t op = 0;
  std::uint64_t requestId = 0;
  std::uint32_t length = 0;
  std::uint32_t checksum = 0;
  if (!decoder.u32(magic) || !decoder.u16(version) || !decoder.u16(op) ||
      !decoder.u64(requestId) || !decoder.u32(length) || !decoder.u32(checksum)) {
    return fail(StatusCode::Corruption, "frame header is truncated");
  }
  if (magic != kFrameMagic) {
    return fail(StatusCode::Corruption, "frame magic does not match");
  }
  if (version != kProtocolVersion) {
    return fail(StatusCode::Corruption, "protocol version " + std::to_string(version) +
                                            " is not supported by this build (expected " +
                                            std::to_string(kProtocolVersion) + ")");
  }
  if (!knownOp(op)) {
    return fail(StatusCode::Corruption, "frame names an unknown operation");
  }
  if (length > maxPayloadBytes || length > kMaxFramePayloadBytes) {
    return fail(StatusCode::Corruption, "frame payload length is out of range");
  }
  frame.op = static_cast<OpCode>(op);
  frame.requestId = requestId;
  frame.payload.assign(length, 0);
  if (length > 0) {
    const Status body = recvAll(socket, frame.payload);
    if (!body.ok()) return body;
  }
  if (crc32(frame.payload) != checksum) {
    return fail(StatusCode::Corruption, "frame checksum does not match");
  }
  return okStatus();
}

Status waitUntil(std::int64_t startAtNanos, std::int64_t giveUpAtNanos) {
  for (;;) {
    const std::int64_t now = wallClockNanos();
    if (now >= startAtNanos) return okStatus();
    if (now > giveUpAtNanos) {
      return fail(StatusCode::Unavailable,
                  "the synchronization instant was not reached before the give-up instant");
    }
    std::this_thread::yield();
  }
}

namespace {

// Request argument codecs. Kept next to the dispatch switch so that the wire
// shape of every operation is readable in one place.

void writeFenceOnly(Encoder& out, const ControllerFence& fence) { encode(out, fence); }

bool readFenceOnly(Decoder& in, ControllerFence& fence) { return decode(in, fence); }

bool readAuthorityPair(Decoder& in, std::uint64_t& generation, ControllerFence& fence) {
  return in.u64(generation) && readFenceOnly(in, fence);
}

bool readRenewArgs(Decoder& in, RenewArgs& value) {
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  std::uint64_t authorityGeneration = 0;
  std::int64_t extension = 0;
  std::int64_t now = 0;
  if (!in.u64(id) || !in.u64(generation) || !in.i64(extension) ||
      !readAuthorityPair(in, authorityGeneration, value.authority.fence) || !in.i64(now) ||
      !in.u32(value.maxRenewals)) {
    return false;
  }
  if (extension <= 0 || now < 0) {
    in.fail();
    return false;
  }
  value.id = ReservationId(id);
  value.generation = ReservationGeneration(generation);
  value.authority.generation = ReservationAuthorityGeneration(authorityGeneration);
  value.extension = Duration::nanos(extension);
  value.now = Instant::fromNanos(now);
  return true;
}

void writeRenewArgs(Encoder& out, const RenewArgs& value) {
  out.u64(value.id.raw());
  out.u64(value.generation.raw());
  out.i64(value.extension.nanos());
  out.u64(value.authority.generation.raw());
  writeFenceOnly(out, value.authority.fence);
  out.i64(value.now.nanos());
  out.u32(value.maxRenewals);
}

bool readActivateArgs(Decoder& in, ActivateArgs& value) {
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  std::uint64_t authorityGeneration = 0;
  std::int64_t now = 0;
  if (!in.u64(id) || !in.u64(generation) ||
      !readAuthorityPair(in, authorityGeneration, value.authority.fence) || !in.i64(now)) {
    return false;
  }
  if (now < 0) {
    in.fail();
    return false;
  }
  value.id = ReservationId(id);
  value.generation = ReservationGeneration(generation);
  value.authority.generation = ActivationAuthorityGeneration(authorityGeneration);
  value.now = Instant::fromNanos(now);
  return true;
}

void writeActivateArgs(Encoder& out, const ActivateArgs& value) {
  out.u64(value.id.raw());
  out.u64(value.generation.raw());
  out.u64(value.authority.generation.raw());
  writeFenceOnly(out, value.authority.fence);
  out.i64(value.now.nanos());
}

bool readReleaseArgs(Decoder& in, ReleaseArgs& value) {
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  std::uint64_t authorityGeneration = 0;
  std::int64_t now = 0;
  if (!in.u64(id) || !in.u64(generation) ||
      !readAuthorityPair(in, authorityGeneration, value.authority.fence) || !in.i64(now)) {
    return false;
  }
  if (now < 0) {
    in.fail();
    return false;
  }
  value.id = ReservationId(id);
  value.generation = ReservationGeneration(generation);
  value.authority.generation = ReleaseAuthorityGeneration(authorityGeneration);
  value.now = Instant::fromNanos(now);
  return true;
}

void writeReleaseArgs(Encoder& out, const ReleaseArgs& value) {
  out.u64(value.id.raw());
  out.u64(value.generation.raw());
  out.u64(value.authority.generation.raw());
  writeFenceOnly(out, value.authority.fence);
  out.i64(value.now.nanos());
}

bool readSweepArgs(Decoder& in, SweepArgs& value) {
  std::int64_t now = 0;
  if (!in.i64(now) || now < 0) {
    in.fail();
    return false;
  }
  value.now = Instant::fromNanos(now);
  return true;
}

void writeSweepArgs(Encoder& out, const SweepArgs& value) { out.i64(value.now.nanos()); }

bool readAuditArgs(Decoder& in, AuditArgs& value) {
  std::uint64_t since = 0;
  if (!in.u64(since) || !in.u32(value.limit)) return false;
  value.since = AuditSequence(since);
  return true;
}

void writeAuditArgs(Encoder& out, const AuditArgs& value) {
  out.u64(value.since.raw());
  out.u32(value.limit);
}

}  // namespace

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

SpectrumServer::SpectrumServer(SpectrumRuntime& runtime, ServerConfig config)
    : runtime_(runtime), config_(std::move(config)) {
  if (config_.workerThreads == 0) config_.workerThreads = 1;
  if (config_.maxConnections == 0) config_.maxConnections = 1;
  if (config_.acceptPollMillis == 0) config_.acceptPollMillis = 1;
}

SpectrumServer::~SpectrumServer() {
  stop();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();
  listener_.close();
}

Status SpectrumServer::start() {
  if (started_) return fail(StatusCode::Duplicate, "the server is already started");
  const Status initialized = initSockets();
  if (!initialized.ok()) return initialized;

  ListenOptions options;
  options.address = config_.address;
  options.port = config_.port;
  options.backlog = config_.backlog;
  const Status listened = listenTcp(options, listener_, port_);
  if (!listened.ok()) return listened;

  stopping_.store(false, std::memory_order_release);
  workers_.reserve(config_.workerThreads);
  for (std::uint32_t index = 0; index < config_.workerThreads; ++index) {
    workers_.emplace_back([this]() { workerLoop(); });
  }
  started_ = true;
  return okStatus();
}

void SpectrumServer::stop() {
  stopping_.store(true, std::memory_order_release);
  wakeup_.notify_all();
}

void SpectrumServer::serve() {
  while (!stopping_.load(std::memory_order_acquire)) {
    Socket connection;
    std::string peer;
    const Status accepted = acceptTcp(listener_, config_.acceptPollMillis, connection, peer);
    if (accepted.code == StatusCode::Unavailable) continue;
    if (!accepted.ok()) {
      if (stopping_.load(std::memory_order_acquire)) break;
      continue;
    }
    // Disabling Nagle is a latency optimisation only; a failure here is not fatal.
    (void)setNoDelay(connection);
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      if (pending_.size() < config_.maxConnections) {
        pending_.push_back(std::move(connection));
        const std::uint32_t depth =
            static_cast<std::uint32_t>(pending_.size()) + activeConnections_.load();
        std::uint32_t peak = peakConnections_.load();
        while (depth > peak && !peakConnections_.compare_exchange_weak(peak, depth)) {
        }
      }
    }
    wakeup_.notify_one();
  }

  wakeup_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();
  listener_.close();
}

void SpectrumServer::workerLoop() {
  for (;;) {
    Socket connection;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wakeup_.wait(lock, [this]() {
        return stopping_.load(std::memory_order_acquire) || !pending_.empty();
      });
      if (pending_.empty()) {
        if (stopping_.load(std::memory_order_acquire)) return;
        continue;
      }
      connection = std::move(pending_.front());
      pending_.pop_front();
    }
    activeConnections_.fetch_add(1);
    serveConnection(connection);
    activeConnections_.fetch_sub(1);
  }
}

void SpectrumServer::serveConnection(Socket& connection) {
  for (;;) {
    if (stopping_.load(std::memory_order_acquire)) break;
    Frame frame;
    const Status read = readFrame(connection, frame, config_.maxPayloadBytes);
    if (!read.ok()) {
      if (read.code == StatusCode::Corruption) rejected_.fetch_add(1);
      break;
    }
    std::vector<std::uint8_t> data;
    bool shutdown = false;
    const Status status = dispatch(frame.op, frame.payload, data, shutdown);
    served_.fetch_add(1);

    Frame reply;
    reply.op = frame.op;
    reply.requestId = frame.requestId;
    reply.payload = envelope(status, data);
    const Status written = writeFrame(connection, reply, config_.maxPayloadBytes);
    if (!written.ok()) break;
    if (shutdown) {
      stop();
      break;
    }
  }
}

Status SpectrumServer::dispatch(OpCode op, const std::vector<std::uint8_t>& payload,
                                std::vector<std::uint8_t>& data, bool& shutdownRequested) {
  Decoder in(payload);
  Encoder out;
  shutdownRequested = false;

  switch (op) {
    case OpCode::Ping: {
      if (!in.exhausted()) return fail(StatusCode::InvalidArgument, "ping takes no payload");
      return okStatus();
    }
    case OpCode::Describe: {
      if (!in.exhausted()) return fail(StatusCode::InvalidArgument, "describe takes no payload");
      ServerDescription description;
      description.fence = runtime_.fence();
      description.runtimeGeneration = runtime_.runtimeGeneration();
      description.recoveryGeneration = runtime_.recoveryGeneration();
      description.grids = runtime_.grids().size();
      description.domains = runtime_.domains().size();
      description.exclusionDomains = runtime_.exclusionDomains().size();
      description.reservations = runtime_.reservations().size();
      std::size_t capabilities = 0;
      for (const SpectrumDomain& domain : runtime_.domains()) {
        if (runtime_.capability(domain.id).has_value()) capabilities += 1;
      }
      description.capabilities = capabilities;
      description.auditRecords = runtime_.auditSize();
      description.lastAudit = runtime_.lastAuditSequence();
      description.stats = runtime_.stats();
      writeDescription(out, description);
      break;
    }
    case OpCode::RegisterGrid: {
      ChannelGrid grid;
      if (!decode(in, grid) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "register-grid payload is malformed");
      }
      return runtime_.registerGrid(grid);
    }
    case OpCode::RegisterSpan: {
      Span span;
      if (!decode(in, span) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "register-span payload is malformed");
      }
      return runtime_.registerSpan(span);
    }
    case OpCode::RegisterPort: {
      OpticalPort port;
      if (!decode(in, port) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "register-port payload is malformed");
      }
      return runtime_.registerPort(port);
    }
    case OpCode::RegisterDomain: {
      SpectrumDomain domain;
      if (!decode(in, domain) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "register-domain payload is malformed");
      }
      return runtime_.registerDomain(domain);
    }
    case OpCode::RegisterExclusionDomain: {
      ExclusionDomain exclusion;
      if (!decode(in, exclusion) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "register-exclusion-domain payload is malformed");
      }
      return runtime_.registerExclusionDomain(exclusion);
    }
    case OpCode::PublishCapability: {
      SpectrumCapability capability;
      if (!decode(in, capability) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "publish-capability payload is malformed");
      }
      return runtime_.publishCapability(capability);
    }
    case OpCode::Enumerate: {
      SpectrumRequest request;
      if (!decode(in, request) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "enumerate payload is malformed");
      }
      const CandidateSet candidates = runtime_.enumerateCandidates(request);
      encode(out, candidates);
      data = out.take();
      return candidates.status;
    }
    case OpCode::Explain: {
      SpectrumRequest request;
      if (!decode(in, request) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "explain payload is malformed");
      }
      const DecisionExplanation explanation = runtime_.explain(request);
      encode(out, explanation);
      data = out.take();
      return okStatus();
    }
    case OpCode::Allocate: {
      SpectrumRequest request;
      if (!decode(in, request) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "allocate payload is malformed");
      }
      const AllocationDecision decision = runtime_.allocate(request);
      encode(out, decision);
      data = out.take();
      return decision.status;
    }
    case OpCode::Renew: {
      RenewArgs args;
      if (!readRenewArgs(in, args) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "renew payload is malformed");
      }
      const Status status = runtime_.renew(args.id, args.generation, args.extension, args.authority,
                                           args.now, args.maxRenewals);
      if (!status.ok()) return status;
      const auto reservation = runtime_.reservation(args.id);
      if (!reservation.has_value()) return fail(StatusCode::NotFound, "reservation disappeared");
      encode(out, *reservation);
      break;
    }
    case OpCode::Activate:
    case OpCode::Deactivate: {
      ActivateArgs args;
      if (!readActivateArgs(in, args) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "activation payload is malformed");
      }
      const Status status = op == OpCode::Activate
                                ? runtime_.activate(args.id, args.generation, args.authority, args.now)
                                : runtime_.deactivate(args.id, args.generation, args.authority,
                                                      args.now);
      if (!status.ok()) return status;
      const auto reservation = runtime_.reservation(args.id);
      if (!reservation.has_value()) return fail(StatusCode::NotFound, "reservation disappeared");
      encode(out, *reservation);
      break;
    }
    case OpCode::Release: {
      ReleaseArgs args;
      if (!readReleaseArgs(in, args) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "release payload is malformed");
      }
      const Status status =
          runtime_.release(args.id, args.generation, args.authority, args.now);
      if (!status.ok()) return status;
      const auto reservation = runtime_.reservation(args.id);
      if (!reservation.has_value()) return fail(StatusCode::NotFound, "reservation disappeared");
      encode(out, *reservation);
      break;
    }
    case OpCode::ExpireLeases:
    case OpCode::ReclaimExpired: {
      SweepArgs args;
      if (!readSweepArgs(in, args) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "sweep payload is malformed");
      }
      const ReclaimReport report = op == OpCode::ExpireLeases
                                       ? runtime_.expireLeases(args.now)
                                       : runtime_.reclaimExpired(args.now);
      encode(out, report);
      break;
    }
    case OpCode::QueryReservation: {
      std::uint64_t id = 0;
      if (!in.u64(id) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "query-reservation payload is malformed");
      }
      const auto found = runtime_.reservation(ReservationId(id));
      if (!found.has_value()) {
        return fail(StatusCode::NotFound,
                    "reservation " + typedToken("reservation", ReservationId(id)) +
                        " is not known to this runtime");
      }
      encode(out, *found);
      break;
    }
    case OpCode::QueryReservations: {
      if (!in.exhausted()) {
        return fail(StatusCode::InvalidArgument, "query-reservations takes no payload");
      }
      const std::vector<SpectrumReservation> all = runtime_.reservations();
      writeList(out, all, [](Encoder& target, const SpectrumReservation& value) {
        encode(target, value);
      });
      break;
    }
    case OpCode::QueryUsage: {
      std::uint64_t domain = 0;
      std::int64_t now = 0;
      if (!in.u64(domain) || !in.i64(now) || !in.exhausted() || now < 0) {
        return fail(StatusCode::Corruption, "query-usage payload is malformed");
      }
      const auto found = runtime_.usage(SpectrumDomainId(domain), Instant::fromNanos(now));
      if (!found.has_value()) {
        return fail(StatusCode::NotFound, "domain " + typedToken("domain", SpectrumDomainId(domain)) +
                                              " is not registered");
      }
      encode(out, *found);
      break;
    }
    case OpCode::QueryUsageAll: {
      std::int64_t now = 0;
      if (!in.i64(now) || !in.exhausted() || now < 0) {
        return fail(StatusCode::Corruption, "query-usage-all payload is malformed");
      }
      const std::vector<SpectrumUsage> all = runtime_.usageAll(Instant::fromNanos(now));
      writeList(out, all, [](Encoder& target, const SpectrumUsage& value) {
        encode(target, value);
      });
      break;
    }
    case OpCode::QueryAudit: {
      AuditArgs args;
      if (!readAuditArgs(in, args) || !in.exhausted()) {
        return fail(StatusCode::Corruption, "query-audit payload is malformed");
      }
      const std::vector<AuditRecord> records = runtime_.audit(args.since, args.limit);
      writeList(out, records, [](Encoder& target, const AuditRecord& value) {
        encode(target, value);
      });
      break;
    }
    case OpCode::QueryStats: {
      if (!in.exhausted()) {
        return fail(StatusCode::InvalidArgument, "query-stats takes no payload");
      }
      writeStats(out, runtime_.stats());
      break;
    }
    case OpCode::Save: {
      if (!in.exhausted()) return fail(StatusCode::InvalidArgument, "save takes no payload");
      return runtime_.save();
    }
    case OpCode::Reset: {
      if (!in.exhausted()) return fail(StatusCode::InvalidArgument, "reset takes no payload");
      return runtime_.reset();
    }
    case OpCode::Shutdown: {
      if (!in.exhausted()) return fail(StatusCode::InvalidArgument, "shutdown takes no payload");
      if (!config_.allowShutdownOp) {
        return fail(StatusCode::NotPermitted, "this server does not accept shutdown requests");
      }
      shutdownRequested = true;
      break;
    }
  }

  data = out.take();
  return okStatus();
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

SpectrumClient::~SpectrumClient() { close(); }

void SpectrumClient::close() {
  const std::lock_guard<std::mutex> guard(mutex_);
  socket_.close();
}

Status SpectrumClient::connect(const std::string& host, std::uint16_t port,
                               std::uint32_t maxPayloadBytes) {
  const std::lock_guard<std::mutex> guard(mutex_);
  socket_.close();
  maxPayloadBytes_ = maxPayloadBytes == 0 ? kMaxFramePayloadBytes : maxPayloadBytes;
  if (maxPayloadBytes_ > kMaxFramePayloadBytes) maxPayloadBytes_ = kMaxFramePayloadBytes;
  const Status initialized = initSockets();
  if (!initialized.ok()) return initialized;
  return connectTcp(host, port, socket_);
}

Status SpectrumClient::call(OpCode op, const std::vector<std::uint8_t>& payload,
                            std::vector<std::uint8_t>& reply) {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (!socket_.valid()) {
    return fail(StatusCode::IoError, "the client is not connected");
  }

  Frame frame;
  frame.op = op;
  frame.requestId = nextRequestId_;
  nextRequestId_ += 1;
  frame.payload = payload;

  const Status written = writeFrame(socket_, frame, maxPayloadBytes_);
  if (!written.ok()) {
    socket_.close();
    return written;
  }

  Frame response;
  const Status read = readFrame(socket_, response, maxPayloadBytes_);
  if (!read.ok()) {
    socket_.close();
    return read;
  }
  if (response.requestId != frame.requestId) {
    socket_.close();
    return fail(StatusCode::Corruption, "the reply carries a different request identity");
  }
  if (response.op != op) {
    socket_.close();
    return fail(StatusCode::Corruption, "the reply carries an unexpected operation");
  }

  Decoder decoder(response.payload);
  Status serverStatus;
  if (!decode(decoder, serverStatus)) {
    socket_.close();
    return fail(StatusCode::Corruption, "the reply envelope is malformed");
  }
  reply.assign(response.payload.begin() + static_cast<std::ptrdiff_t>(decoder.offset()),
               response.payload.end());
  return serverStatus;
}

Status SpectrumClient::ping() {
  std::vector<std::uint8_t> reply;
  return call(OpCode::Ping, {}, reply);
}

Status SpectrumClient::describe(ServerDescription& out) {
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::Describe, {}, reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!readDescription(decoder, out) || !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the describe reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::registerGrid(const ChannelGrid& grid) {
  Encoder encoder;
  encode(encoder, grid);
  std::vector<std::uint8_t> reply;
  return call(OpCode::RegisterGrid, encoder.take(), reply);
}

Status SpectrumClient::registerSpan(const Span& span) {
  Encoder encoder;
  encode(encoder, span);
  std::vector<std::uint8_t> reply;
  return call(OpCode::RegisterSpan, encoder.take(), reply);
}

Status SpectrumClient::registerPort(const OpticalPort& port) {
  Encoder encoder;
  encode(encoder, port);
  std::vector<std::uint8_t> reply;
  return call(OpCode::RegisterPort, encoder.take(), reply);
}

Status SpectrumClient::registerDomain(const SpectrumDomain& domain) {
  Encoder encoder;
  encode(encoder, domain);
  std::vector<std::uint8_t> reply;
  return call(OpCode::RegisterDomain, encoder.take(), reply);
}

Status SpectrumClient::registerExclusionDomain(const ExclusionDomain& domain) {
  Encoder encoder;
  encode(encoder, domain);
  std::vector<std::uint8_t> reply;
  return call(OpCode::RegisterExclusionDomain, encoder.take(), reply);
}

Status SpectrumClient::publishCapability(const SpectrumCapability& capability) {
  Encoder encoder;
  encode(encoder, capability);
  std::vector<std::uint8_t> reply;
  return call(OpCode::PublishCapability, encoder.take(), reply);
}

Status SpectrumClient::enumerate(const SpectrumRequest& request, CandidateSet& out) {
  Encoder encoder;
  encode(encoder, request);
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::Enumerate, encoder.take(), reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!decode(decoder, out) || !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the enumerate reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::explain(const SpectrumRequest& request, DecisionExplanation& out) {
  Encoder encoder;
  encode(encoder, request);
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::Explain, encoder.take(), reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!decode(decoder, out) || !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the explain reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::allocate(const SpectrumRequest& request, AllocationDecision& out) {
  Encoder encoder;
  encode(encoder, request);
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::Allocate, encoder.take(), reply);
  Decoder decoder(reply);
  if (!decode(decoder, out) || !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the allocate reply is malformed");
  }
  return status;
}

Status SpectrumClient::renew(const RenewArgs& args, SpectrumReservation& out) {
  Encoder encoder;
  writeRenewArgs(encoder, args);
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::Renew, encoder.take(), reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!decode(decoder, out) || !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the renew reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::activate(const ActivateArgs& args, SpectrumReservation& out) {
  Encoder encoder;
  writeActivateArgs(encoder, args);
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::Activate, encoder.take(), reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!decode(decoder, out) || !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the activate reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::deactivate(const ActivateArgs& args, SpectrumReservation& out) {
  Encoder encoder;
  writeActivateArgs(encoder, args);
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::Deactivate, encoder.take(), reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!decode(decoder, out) || !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the deactivate reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::release(const ReleaseArgs& args, SpectrumReservation& out) {
  Encoder encoder;
  writeReleaseArgs(encoder, args);
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::Release, encoder.take(), reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!decode(decoder, out) || !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the release reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::expireLeases(const SweepArgs& args, ReclaimReport& out) {
  Encoder encoder;
  writeSweepArgs(encoder, args);
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::ExpireLeases, encoder.take(), reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!decode(decoder, out) || !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the expire-leases reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::reclaimExpired(const SweepArgs& args, ReclaimReport& out) {
  Encoder encoder;
  writeSweepArgs(encoder, args);
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::ReclaimExpired, encoder.take(), reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!decode(decoder, out) || !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the reclaim-expired reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::queryReservation(ReservationId id, SpectrumReservation& out) {
  Encoder encoder;
  encoder.u64(id.raw());
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::QueryReservation, encoder.take(), reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!decode(decoder, out) || !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the query-reservation reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::queryReservations(std::vector<SpectrumReservation>& out) {
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::QueryReservations, {}, reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!readList(decoder, out, kMaxReservationListBytes,
                [](Decoder& source, SpectrumReservation& value) {
                  return decode(source, value);
                }) ||
      !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the query-reservations reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::queryUsage(SpectrumDomainId id, Instant now, SpectrumUsage& out) {
  Encoder encoder;
  encoder.u64(id.raw());
  encoder.i64(now.nanos());
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::QueryUsage, encoder.take(), reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!decode(decoder, out) || !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the query-usage reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::queryUsageAll(Instant now, std::vector<SpectrumUsage>& out) {
  Encoder encoder;
  encoder.i64(now.nanos());
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::QueryUsageAll, encoder.take(), reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!readList(decoder, out, kMaxUsageList,
                [](Decoder& source, SpectrumUsage& value) { return decode(source, value); }) ||
      !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the query-usage-all reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::queryAudit(const AuditArgs& args, std::vector<AuditRecord>& out) {
  Encoder encoder;
  writeAuditArgs(encoder, args);
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::QueryAudit, encoder.take(), reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!readList(decoder, out, kMaxAuditList,
                [](Decoder& source, AuditRecord& value) { return decode(source, value); }) ||
      !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the query-audit reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::queryStats(RuntimeStats& out) {
  std::vector<std::uint8_t> reply;
  const Status status = call(OpCode::QueryStats, {}, reply);
  if (!status.ok()) return status;
  Decoder decoder(reply);
  if (!readStats(decoder, out) || !decoder.exhausted()) {
    close();
    return fail(StatusCode::Corruption, "the query-stats reply is malformed");
  }
  return okStatus();
}

Status SpectrumClient::save() {
  std::vector<std::uint8_t> reply;
  return call(OpCode::Save, {}, reply);
}

Status SpectrumClient::requestShutdown() {
  std::vector<std::uint8_t> reply;
  return call(OpCode::Shutdown, {}, reply);
}

}  // namespace wavelength_fabric
