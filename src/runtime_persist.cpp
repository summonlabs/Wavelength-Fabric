#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "impl.hpp"

namespace wavelength_fabric {
namespace detail {

namespace {

constexpr char kHeaderMagic[8] = {'W', 'V', 'L', 'F', 'A', 'B', '0', '1'};
constexpr char kTrailerMagic[8] = {'W', 'V', 'L', 'F', 'E', 'N', 'D', '1'};
// Layout: "WVLFAB01" (8) | formatVersion u16 | flags u16 | headerBytes u32 |
// reserved u32 | bodyBytes u64 | recordCount u64 | headerCrc u32  == 40 bytes.
// The header checksum covers the first 36 bytes and is stored in the last 4.
constexpr std::size_t kHeaderBytes = 40;
constexpr std::size_t kHeaderPrefixBytes = 36;
// Trailer: bodyCrc u32 | fileBytes u64 | reserved u32 | "WVLFEND1" (8) == 24 bytes.
constexpr std::size_t kTrailerBytes = 24;
constexpr std::size_t kTrailerMagicOffset = 16;
constexpr std::uint64_t kMaxRecords = 8u * 1024u * 1024u;
constexpr std::uint32_t kMaxRecordBytes = 64u * 1024u * 1024u;

enum class RecordKind : std::uint8_t {
  None = 0,
  Metadata = 1,
  Grid = 2,
  Span = 3,
  Port = 4,
  Domain = 5,
  ExclusionDomain = 6,
  Capability = 7,
  Reservation = 8,
  Audit = 9,
};

[[nodiscard]] bool isDurableState(ReservationState state) noexcept {
  switch (state) {
    case ReservationState::Reserved:
    case ReservationState::Active:
    case ReservationState::Expired:
    case ReservationState::Released:
    case ReservationState::Reclaimed:
    case ReservationState::Superseded:
    case ReservationState::Refused:
    case ReservationState::Retired:
      return true;
    default:
      return false;
  }
}

void appendRecord(Encoder& body, RecordKind kind, const std::vector<std::uint8_t>& payload) {
  body.u8(static_cast<std::uint8_t>(kind));
  body.u8(0);
  body.u32(static_cast<std::uint32_t>(payload.size()));
  body.u32(crc32(payload));
  body.bytes(payload);
}

void syncFile(std::FILE* handle) {
#ifdef _WIN32
  ::_commit(::_fileno(handle));
#else
  ::fsync(::fileno(handle));
#endif
}

[[nodiscard]] std::string loadFailure(const char* what) { return std::string(what); }

}  // namespace

Status encodeState(const RuntimeState& state, std::vector<std::uint8_t>& out) {
  Encoder body;
  std::uint64_t records = 0;

  {
    Encoder payload;
    payload.u16(kPersistenceFormatVersion);
    payload.u16(0);
    payload.u64(state.fence.epoch.raw());
    payload.u64(state.fence.incarnation.raw());
    payload.u64(state.runtimeGeneration.raw());
    payload.u64(state.recoveryGeneration.raw());
    payload.u64(state.nextReservationId.raw());
    payload.u64(state.auditSequence.raw());
    payload.i64(state.lastActivityAt.nanos());
    payload.u64(state.config.controller.raw());
    payload.u64(state.authority.eligibilityGeneration.raw());
    payload.u64(state.authority.reservationGeneration.raw());
    payload.u64(state.authority.activationGeneration.raw());
    payload.u64(state.authority.releaseGeneration.raw());
    if (payload.overflowed()) return fail(StatusCode::Overflow, "metadata record overflowed");
    appendRecord(body, RecordKind::Metadata, payload.take());
    records += 1;
  }

  const auto emit = [&](RecordKind kind, const auto& value) -> void {
    Encoder payload;
    encode(payload, value);
    appendRecord(body, kind, payload.take());
    records += 1;
  };

  for (const auto& entry : state.grids) emit(RecordKind::Grid, entry.second);
  for (const auto& entry : state.spans) emit(RecordKind::Span, entry.second);
  for (const auto& entry : state.ports) emit(RecordKind::Port, entry.second);
  for (const auto& entry : state.domains) emit(RecordKind::Domain, entry.second);
  for (const auto& entry : state.exclusionDomains) {
    emit(RecordKind::ExclusionDomain, entry.second);
  }
  for (const auto& entry : state.capabilities) emit(RecordKind::Capability, entry.second);
  for (const auto& entry : state.reservations) emit(RecordKind::Reservation, entry.second);
  for (const AuditRecord& record : state.audit) emit(RecordKind::Audit, record);

  if (body.overflowed()) {
    return fail(StatusCode::Overflow, "encoded state exceeded the encoder bound");
  }

  Encoder header;
  for (const char value : kHeaderMagic) header.u8(static_cast<std::uint8_t>(value));
  header.u16(kPersistenceFormatVersion);
  header.u16(0);
  header.u32(static_cast<std::uint32_t>(kHeaderBytes));
  header.u32(0);
  header.u64(static_cast<std::uint64_t>(body.size()));
  header.u64(records);
  std::vector<std::uint8_t> headerBytes = header.take();
  const std::uint32_t headerCrc = crc32(headerBytes);

  const std::uint64_t totalBytes = static_cast<std::uint64_t>(kHeaderBytes) +
                                   static_cast<std::uint64_t>(body.size()) +
                                   static_cast<std::uint64_t>(kTrailerBytes);

  out.clear();
  out.reserve(static_cast<std::size_t>(totalBytes));
  out.insert(out.end(), headerBytes.begin(), headerBytes.end());
  Encoder crcField;
  crcField.u32(headerCrc);
  const std::vector<std::uint8_t> crcBytes = crcField.take();
  out.insert(out.end(), crcBytes.begin(), crcBytes.end());

  const std::vector<std::uint8_t>& bodyBytes = body.buffer();
  out.insert(out.end(), bodyBytes.begin(), bodyBytes.end());

  Encoder trailer;
  trailer.u32(crc32(bodyBytes));
  trailer.u64(totalBytes);
  trailer.u32(0);
  for (const char value : kTrailerMagic) trailer.u8(static_cast<std::uint8_t>(value));
  const std::vector<std::uint8_t> trailerBytes = trailer.take();
  out.insert(out.end(), trailerBytes.begin(), trailerBytes.end());
  return okStatus();
}

bool decodeState(std::span<const std::uint8_t> bytes, RuntimeState& out, RecoveryReport& report) {
  if (bytes.size() < kHeaderBytes + kTrailerBytes) {
    report.diagnostics.push_back(loadFailure("state container is shorter than its fixed framing"));
    return false;
  }
  if (std::memcmp(bytes.data(), kHeaderMagic, sizeof(kHeaderMagic)) != 0) {
    report.diagnostics.push_back(loadFailure("state container magic is wrong"));
    return false;
  }
  Decoder header(bytes.subspan(8, 28));
  std::uint16_t formatVersion = 0;
  std::uint16_t flags = 0;
  std::uint32_t headerBytes = 0;
  std::uint32_t reserved = 0;
  std::uint64_t bodyBytes = 0;
  std::uint64_t recordCount = 0;
  if (!header.u16(formatVersion) || !header.u16(flags) || !header.u32(headerBytes) ||
      !header.u32(reserved) || !header.u64(bodyBytes) || !header.u64(recordCount)) {
    report.diagnostics.push_back(loadFailure("state header is truncated"));
    return false;
  }
  if (!header.exhausted()) {
    report.diagnostics.push_back(loadFailure("state header carries trailing fields"));
    return false;
  }
  if (headerBytes != kHeaderBytes) {
    report.diagnostics.push_back(loadFailure("state header size field is wrong"));
    return false;
  }
  if (formatVersion != kPersistenceFormatVersion) {
    report.diagnostics.push_back("state format version " + std::to_string(formatVersion) +
                                 " is not supported by this build (expected " +
                                 std::to_string(kPersistenceFormatVersion) + ")");
    return false;
  }
  if (flags != 0) {
    report.diagnostics.push_back(loadFailure("state header declares unsupported flags"));
    return false;
  }
  {
    Decoder crc(bytes.subspan(kHeaderPrefixBytes, 4));
    std::uint32_t stored = 0;
    if (!crc.u32(stored) || crc32(bytes.subspan(0, kHeaderPrefixBytes)) != stored) {
      report.diagnostics.push_back(loadFailure("state header checksum does not match"));
      return false;
    }
  }
  if (recordCount > kMaxRecords) {
    report.diagnostics.push_back(loadFailure("state declares more records than are supported"));
    return false;
  }
  if (bodyBytes != static_cast<std::uint64_t>(bytes.size() - kHeaderBytes - kTrailerBytes)) {
    report.diagnostics.push_back(
        loadFailure("state body length does not match the container length"));
    return false;
  }
  const std::span<const std::uint8_t> body = bytes.subspan(kHeaderBytes, bodyBytes);
  const std::span<const std::uint8_t> trailer = bytes.subspan(kHeaderBytes + bodyBytes);
  {
    Decoder trailerDecoder(trailer);
    std::uint32_t bodyCrc = 0;
    std::uint64_t fileBytes = 0;
    std::uint32_t trailerReserved = 0;
    if (!trailerDecoder.u32(bodyCrc) || !trailerDecoder.u64(fileBytes) ||
        !trailerDecoder.u32(trailerReserved)) {
      report.diagnostics.push_back(loadFailure("state trailer is truncated"));
      return false;
    }
    if (trailerReserved != 0) {
      report.diagnostics.push_back(loadFailure("state trailer declares unsupported flags"));
      return false;
    }
    if (crc32(body) != bodyCrc) {
      report.diagnostics.push_back(loadFailure("state body checksum does not match"));
      return false;
    }
    if (fileBytes != bytes.size()) {
      report.diagnostics.push_back(loadFailure("state trailer length does not match the file"));
      return false;
    }
    if (std::memcmp(trailer.data() + kTrailerMagicOffset, kTrailerMagic, sizeof(kTrailerMagic)) != 0) {
      report.diagnostics.push_back(loadFailure("state trailer magic is wrong"));
      return false;
    }
  }

  std::map<ChannelGridId, ChannelGrid> grids;
  std::map<SpanId, Span> spans;
  std::map<PortId, OpticalPort> ports;
  std::map<SpectrumDomainId, SpectrumDomain> domains;
  std::map<ExclusionDomainId, ExclusionDomain> exclusionDomains;
  std::map<SpectrumDomainId, SpectrumCapability> capabilities;
  std::map<ReservationId, SpectrumReservation> reservations;
  std::deque<AuditRecord> audits;
  std::uint64_t seenRecords = 0;
  bool sawMetadata = false;

  Decoder reader(body);
  while (!reader.exhausted()) {
    std::uint8_t kindValue = 0;
    std::uint8_t recordFlags = 0;
    std::uint32_t payloadBytes = 0;
    std::uint32_t payloadCrc = 0;
    if (!reader.u8(kindValue) || !reader.u8(recordFlags) || !reader.u32(payloadBytes) ||
        !reader.u32(payloadCrc)) {
      report.diagnostics.push_back(loadFailure("state record header is truncated"));
      return false;
    }
    if (recordFlags != 0) {
      report.diagnostics.push_back(loadFailure("state record declares unsupported flags"));
      return false;
    }
    if (payloadBytes > kMaxRecordBytes || payloadBytes > reader.remaining()) {
      report.diagnostics.push_back(loadFailure("state record length is out of range"));
      return false;
    }
    std::vector<std::uint8_t> payload;
    if (!reader.bytes(payload, kMaxRecordBytes)) {
      report.diagnostics.push_back(loadFailure("state record payload is truncated"));
      return false;
    }
    if (crc32(payload) != payloadCrc) {
      report.diagnostics.push_back(loadFailure("state record checksum does not match"));
      return false;
    }
    seenRecords += 1;
    report.recordsRead += 1;

    Decoder record(payload);
    bool decoded = false;
    switch (static_cast<RecordKind>(kindValue)) {
      case RecordKind::Metadata: {
        std::uint16_t version = 0;
        std::uint16_t recordReserved = 0;
        std::uint64_t epoch = 0;
        std::uint64_t incarnation = 0;
        std::uint64_t runtimeGeneration = 0;
        std::uint64_t recoveryGeneration = 0;
        std::uint64_t nextReservation = 0;
        std::uint64_t auditSequence = 0;
        std::int64_t lastActivity = 0;
        std::uint64_t controller = 0;
        std::uint64_t eligibility = 0;
        std::uint64_t reservation = 0;
        std::uint64_t activation = 0;
        std::uint64_t release = 0;
        decoded = record.u16(version) && record.u16(recordReserved) && record.u64(epoch) &&
                  record.u64(incarnation) && record.u64(runtimeGeneration) &&
                  record.u64(recoveryGeneration) && record.u64(nextReservation) &&
                  record.u64(auditSequence) && record.i64(lastActivity) &&
                  record.u64(controller) && record.u64(eligibility) && record.u64(reservation) &&
                  record.u64(activation) && record.u64(release) && record.exhausted();
        if (decoded && (version != kPersistenceFormatVersion || recordReserved != 0)) {
          decoded = false;
        }
        if (decoded) {
          out.fence.epoch = ControllerEpoch(epoch);
          out.fence.incarnation = ControllerIncarnation(incarnation);
          out.runtimeGeneration = RuntimeGeneration(runtimeGeneration);
          out.recoveryGeneration = RecoveryGeneration(recoveryGeneration);
          out.nextReservationId = ReservationId(nextReservation);
          out.auditSequence = AuditSequence(auditSequence);
          out.lastActivityAt = Instant::fromNanos(lastActivity);
          (void)controller;
          out.authority.eligibilityGeneration = EligibilityAuthorityGeneration(eligibility);
          out.authority.reservationGeneration = ReservationAuthorityGeneration(reservation);
          out.authority.activationGeneration = ActivationAuthorityGeneration(activation);
          out.authority.releaseGeneration = ReleaseAuthorityGeneration(release);
          sawMetadata = true;
        }
        break;
      }
      case RecordKind::Grid: {
        ChannelGrid value;
        decoded = decode(record, value) && record.exhausted() && value.id.valid() &&
                  grids.emplace(value.id, value).second;
        break;
      }
      case RecordKind::Span: {
        Span value;
        decoded = decode(record, value) && record.exhausted() && value.id.valid() &&
                  spans.emplace(value.id, value).second;
        break;
      }
      case RecordKind::Port: {
        OpticalPort value;
        decoded = decode(record, value) && record.exhausted() && value.id.valid() &&
                  ports.emplace(value.id, value).second;
        break;
      }
      case RecordKind::Domain: {
        SpectrumDomain value;
        decoded = decode(record, value) && record.exhausted() && value.id.valid() &&
                  domains.emplace(value.id, value).second;
        break;
      }
      case RecordKind::ExclusionDomain: {
        ExclusionDomain value;
        decoded = decode(record, value) && record.exhausted() && value.id.valid() &&
                  exclusionDomains.emplace(value.id, value).second;
        break;
      }
      case RecordKind::Capability: {
        SpectrumCapability value;
        decoded = decode(record, value) && record.exhausted() && value.domain.valid() &&
                  capabilities.emplace(value.domain, value).second;
        break;
      }
      case RecordKind::Reservation: {
        SpectrumReservation value;
        decoded = decode(record, value) && record.exhausted() && value.id.valid() &&
                  isDurableState(value.state) &&
                  reservations.emplace(value.id, value).second;
        break;
      }
      case RecordKind::Audit: {
        AuditRecord value;
        decoded = decode(record, value) && record.exhausted() && value.sequence.valid() &&
                  value.at.nanos() >= 0;
        if (decoded) audits.push_back(std::move(value));
        break;
      }
      case RecordKind::None:
      default:
        decoded = false;
        break;
    }

    if (!decoded) {
      report.diagnostics.push_back("state record " + std::to_string(seenRecords) +
                                   " (kind " + std::to_string(kindValue) +
                                   ") is malformed or duplicated");
      return false;
    }
    report.recordsAccepted += 1;
  }

  if (!sawMetadata) {
    report.diagnostics.push_back(loadFailure("state container has no metadata record"));
    return false;
  }
  if (seenRecords != recordCount) {
    report.diagnostics.push_back(loadFailure("state record count does not match the header"));
    return false;
  }

  // ---- semantic validation -------------------------------------------------
  for (const auto& entry : domains) {
    const SpectrumDomain& domain = entry.second;
    const auto gridIt = grids.find(domain.grid);
    if (gridIt == grids.end()) {
      report.diagnostics.push_back("domain " + typedToken("domain", domain.id) +
                                   " references grid " + typedToken("grid", domain.grid) +
                                   " that is not present in the state");
      return false;
    }
    if (gridIt->second.generation != domain.gridGeneration) {
      report.diagnostics.push_back("domain " + typedToken("domain", domain.id) +
                                   " has a grid generation that does not match");
      return false;
    }
    if (domain.span.valid() && spans.find(domain.span) == spans.end()) {
      report.diagnostics.push_back("domain " + typedToken("domain", domain.id) +
                                   " references a span that is not present in the state");
      return false;
    }
  }
  for (const auto& entry : exclusionDomains) {
    for (const SpectrumDomainId member : entry.second.members) {
      if (domains.find(member) == domains.end()) {
        report.diagnostics.push_back("exclusion domain " + typedToken("exclusion", entry.first) +
                                     " references a domain that is not present in the state");
        return false;
      }
    }
  }
  for (const auto& entry : capabilities) {
    const SpectrumCapability& capability = entry.second;
    const auto domainIt = domains.find(capability.domain);
    if (domainIt == domains.end()) {
      report.diagnostics.push_back("capability references a domain that is not present");
      return false;
    }
    if (capability.grid != domainIt->second.grid ||
        capability.gridGeneration != domainIt->second.gridGeneration) {
      report.diagnostics.push_back("capability grid does not match its domain grid");
      return false;
    }
  }

  Instant checkAt = out.lastActivityAt;
  for (const auto& entry : reservations) {
    const SpectrumReservation& reservation = entry.second;
    if (reservation.domains.size() != reservation.domainGenerations.size() ||
        reservation.domains.size() != reservation.perDomainSlots.size()) {
      report.diagnostics.push_back("reservation " + typedToken("reservation", reservation.id) +
                                   " has inconsistent domain vectors");
      return false;
    }
    for (std::size_t index = 0; index < reservation.domains.size(); ++index) {
      const auto domainIt = domains.find(reservation.domains[index]);
      if (domainIt == domains.end()) {
        report.diagnostics.push_back("reservation " + typedToken("reservation", reservation.id) +
                                     " references a domain that is not present");
        return false;
      }
      if (domainIt->second.generation != reservation.domainGenerations[index]) {
        report.diagnostics.push_back("reservation " + typedToken("reservation", reservation.id) +
                                     " names a domain generation that does not match the state");
        return false;
      }
    }
    if (reservation.updatedAt > checkAt) checkAt = reservation.updatedAt;
    if (reservation.id.raw() >= out.nextReservationId.raw()) {
      report.diagnostics.push_back("reservation identity counter regressed");
      return false;
    }
  }

  // No two reservations that are simultaneously live may overlap. This is the
  // core ownership invariant and it is re-proved on every load.
  std::vector<const SpectrumReservation*> liveOnes;
  for (const auto& entry : reservations) {
    if (isLiveAt(entry.second, checkAt)) liveOnes.push_back(&entry.second);
  }
  for (std::size_t left = 0; left < liveOnes.size(); ++left) {
    for (std::size_t right = left + 1; right < liveOnes.size(); ++right) {
      if (reservationsConflict(*liveOnes[left], *liveOnes[right])) {
        report.diagnostics.push_back(
            "state contains two conflicting live reservations; refusing to load it");
        return false;
      }
    }
  }

  AuditSequence previousSequence{};
  for (const AuditRecord& record : audits) {
    if (record.sequence <= previousSequence) {
      report.diagnostics.push_back("audit history is not strictly increasing");
      return false;
    }
    previousSequence = record.sequence;
  }
  if (out.auditSequence < previousSequence) out.auditSequence = previousSequence;

  out.grids = std::move(grids);
  out.spans = std::move(spans);
  out.ports = std::move(ports);
  out.domains = std::move(domains);
  out.exclusionDomains = std::move(exclusionDomains);
  out.capabilities = std::move(capabilities);
  out.reservations = std::move(reservations);
  out.audit = std::move(audits);

  report.gridsRestored = out.grids.size();
  report.domainsRestored = out.domains.size();
  report.exclusionDomainsRestored = out.exclusionDomains.size();
  report.capabilitiesRestored = out.capabilities.size();
  report.reservationsRestored = out.reservations.size();
  report.auditsRestored = out.audit.size();
  return true;
}

Status writeStateFile(const std::string& path, const std::vector<std::uint8_t>& bytes, bool fsync) {
  if (path.empty()) return fail(StatusCode::InvalidArgument, "no state path configured");
  const std::string temporary = path + ".tmp";
  std::error_code ignored;
  std::filesystem::remove(temporary, ignored);

  std::FILE* handle = std::fopen(temporary.c_str(), "wb");
  if (handle == nullptr) {
    return fail(StatusCode::IoError, "cannot open " + temporary + " for writing");
  }
  const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), handle);
  if (written != bytes.size()) {
    std::fclose(handle);
    std::filesystem::remove(temporary, ignored);
    return fail(StatusCode::IoError, "short write to " + temporary);
  }
  if (fsync) syncFile(handle);
  if (std::fclose(handle) != 0) {
    std::filesystem::remove(temporary, ignored);
    return fail(StatusCode::IoError, "cannot flush " + temporary);
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, ignored);
    return fail(StatusCode::IoError,
                "cannot publish " + path + ": " + error.message());
  }
  return okStatus();
}

Status readStateFile(const std::string& path, std::vector<std::uint8_t>& out,
                     std::uint64_t maxBytes) {
  if (path.empty()) return fail(StatusCode::InvalidArgument, "no state path configured");
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return fail(StatusCode::NotFound, "no durable state exists at " + path);
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) return fail(StatusCode::IoError, "cannot size " + path + ": " + error.message());
  if (size > maxBytes) {
    return fail(StatusCode::LimitExceeded, "durable state at " + path + " is " +
                                               std::to_string(size) +
                                               " bytes, above the configured bound of " +
                                               std::to_string(maxBytes));
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return fail(StatusCode::IoError, "cannot open " + path + " for reading");
  out.assign(static_cast<std::size_t>(size), 0);
  if (size > 0) {
    stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (stream.gcount() != static_cast<std::streamsize>(size)) {
      out.clear();
      return fail(StatusCode::IoError, "short read from " + path);
    }
  }
  return okStatus();
}

Status persistAfterMutation(RuntimeState& state) {
  if (!state.config.durableCommits || state.config.statePath.empty()) return okStatus();
  return saveLocked(state);
}

Status saveLocked(RuntimeState& state) {
  if (state.config.statePath.empty()) {
    return fail(StatusCode::InvalidArgument, "the runtime has no configured state path");
  }
  std::vector<std::uint8_t> bytes;
  const Status encoded = encodeState(state, bytes);
  if (!encoded.ok()) {
    state.stats.persistenceFailures += 1;
    appendAudit(state, AuditKind::PersistenceFailed, systemNow(), AllocationOutcome::Unknown,
                ReservationId{}, ReservationGeneration{}, SpectrumDomainId{}, SlotRange{},
                FrequencyRange{}, "encode failed: " + encoded.message);
    return encoded;
  }
  if (bytes.size() > state.config.maxStateBytes) {
    state.stats.persistenceFailures += 1;
    const Status status = fail(StatusCode::LimitExceeded,
                               "encoded state is " + std::to_string(bytes.size()) +
                                   " bytes, above the configured bound of " +
                                   std::to_string(state.config.maxStateBytes));
    appendAudit(state, AuditKind::PersistenceFailed, systemNow(), AllocationOutcome::Unknown,
                ReservationId{}, ReservationGeneration{}, SpectrumDomainId{}, SlotRange{},
                FrequencyRange{}, status.message);
    return status;
  }
  const Status written = writeStateFile(state.config.statePath, bytes, state.config.fsyncState);
  if (!written.ok()) {
    state.stats.persistenceFailures += 1;
    appendAudit(state, AuditKind::PersistenceFailed, systemNow(), AllocationOutcome::Unknown,
                ReservationId{}, ReservationGeneration{}, SpectrumDomainId{}, SlotRange{},
                FrequencyRange{}, written.message);
    return written;
  }
  state.stats.persistenceWrites += 1;
  appendAudit(state, AuditKind::PersistenceFlushed, systemNow(), AllocationOutcome::Unknown,
              ReservationId{}, ReservationGeneration{}, SpectrumDomainId{}, SlotRange{},
              FrequencyRange{},
              "wrote " + std::to_string(bytes.size()) + " bytes to " + state.config.statePath);
  return okStatus();
}

}  // namespace detail

Status SpectrumRuntime::save() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return detail::saveLocked(*state_);
}

RecoveryReport SpectrumRuntime::recover() {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  RecoveryReport report;
  report.source = state.config.statePath;
  report.fence = state.fence;

  if (state.config.statePath.empty()) {
    report.status = fail(StatusCode::InvalidArgument, "the runtime has no configured state path");
    return report;
  }

  std::vector<std::uint8_t> bytes;
  const Status read = detail::readStateFile(state.config.statePath, bytes, state.config.maxStateBytes);
  if (!read.ok()) {
    if (read.code == StatusCode::NotFound) {
      report.status = read;
      report.recovered = false;
      report.diagnostics.push_back(read.message);
      return report;
    }
    state.stats.corruptionDetections += 1;
    detail::appendAudit(state, AuditKind::CorruptionDetected, systemNow(),
                        AllocationOutcome::Unknown, ReservationId{}, ReservationGeneration{},
                        SpectrumDomainId{}, SlotRange{}, FrequencyRange{}, read.message);
    report.status = read;
    return report;
  }

  detail::RuntimeState loaded;
  loaded.config = state.config;
  if (!detail::decodeState(bytes, loaded, report)) {
    state.stats.corruptionDetections += 1;
    detail::appendAudit(state, AuditKind::CorruptionDetected, systemNow(),
                        AllocationOutcome::Unknown, ReservationId{}, ReservationGeneration{},
                        SpectrumDomainId{}, SlotRange{}, FrequencyRange{},
                        report.diagnostics.empty() ? std::string("state rejected")
                                                   : report.diagnostics.back());
    report.status = fail(StatusCode::Corruption, report.diagnostics.empty()
                                                     ? std::string("durable state was rejected")
                                                     : report.diagnostics.back());
    return report;
  }

  const std::uint64_t nextEpoch = loaded.fence.epoch.raw() + 1;
  if (nextEpoch > kMaxFenceValue) {
    report.status = fail(StatusCode::LimitExceeded, "controller epoch space is exhausted");
    return report;
  }

  state.grids = std::move(loaded.grids);
  state.spans = std::move(loaded.spans);
  state.ports = std::move(loaded.ports);
  state.domains = std::move(loaded.domains);
  state.exclusionDomains = std::move(loaded.exclusionDomains);
  state.capabilities = std::move(loaded.capabilities);
  state.reservations = std::move(loaded.reservations);
  state.audit = std::move(loaded.audit);
  state.auditSequence = loaded.auditSequence;
  state.nextReservationId = loaded.nextReservationId;
  state.lastActivityAt = loaded.lastActivityAt;
  state.runtimeGeneration = RuntimeGeneration(loaded.runtimeGeneration.raw() + 1);
  state.recoveryGeneration = RecoveryGeneration(loaded.recoveryGeneration.raw() + 1);
  state.fence.epoch = ControllerEpoch(nextEpoch);
  state.fence.incarnation = ControllerIncarnation(detail::nextIncarnation());
  state.authority = loaded.authority;
  state.authority.fence = state.fence;

  // Activation evidence is not durable. Every reservation that was Active in
  // the persisted image is conservatively returned to Reserved and flagged for
  // revalidation; its generation advances so that references minted by the
  // previous incarnation are fenced.
  for (auto& entry : state.reservations) {
    SpectrumReservation& reservation = entry.second;
    if (reservation.state != ReservationState::Active) continue;
    reservation.state = ReservationState::Reserved;
    reservation.needsRevalidation = true;
    reservation.generation.bump();
    reservation.updatedAt = systemNow();
    report.demotedFromActive += 1;
    detail::appendAudit(state, AuditKind::ReservationDeactivated, reservation.updatedAt,
                        AllocationOutcome::Unknown, reservation.id, reservation.generation,
                        reservation.anchorDomain, reservation.slots, reservation.frequency,
                        "demoted from ACTIVE during recovery; fresh activation authority is "
                        "required before the reservation is active again");
  }

  detail::rebuildIndices(state);
  detail::appendAudit(state, AuditKind::RecoveryPerformed, systemNow(),
                      AllocationOutcome::Unknown, ReservationId{}, ReservationGeneration{},
                      SpectrumDomainId{}, SlotRange{}, FrequencyRange{},
                      "recovered " + std::to_string(report.reservationsRestored) +
                          " reservation(s) from " + state.config.statePath + " into epoch " +
                          std::to_string(state.fence.epoch.raw()) + " incarnation " +
                          std::to_string(state.fence.incarnation.raw()));

  report.recovered = true;
  report.status = okStatus();
  report.fence = state.fence;
  report.runtimeGeneration = state.runtimeGeneration;
  report.recoveryGeneration = state.recoveryGeneration;
  return report;
}

}  // namespace wavelength_fabric
