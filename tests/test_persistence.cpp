#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif

// Versioned durable state.
//
// A saved image is a self-describing container: a 40-byte header whose checksum
// covers the first 36 bytes, records with a per-record CRC-32, and a 24-byte
// trailer. A round trip through save()/recover() must restore every record
// exactly, advance the epoch, change the incarnation, demote an ACTIVE
// reservation and leave a loadable image behind; every malformed image must be
// refused with a typed corruption report and must leave the runtime untouched.
//
// State files are written under the OS temporary directory only.

#include "test_common.hpp"

#include <wavelength_fabric/serialization.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace wavelength_fabric;

const Instant kStart = Instant::fromSeconds(1'800'000'000);
const Duration kLease = Duration::minutes(10);

// ---------------------------------------------------------------------------
// Container layout. These offsets mirror the documented container framing and
// are used only to author malformed images for the refusal cases.
// ---------------------------------------------------------------------------
constexpr std::size_t kHeaderBytes = 40;
constexpr std::size_t kHeaderPrefixBytes = 36;
constexpr std::size_t kTrailerBytes = 24;
constexpr std::size_t kTrailerMagicOffset = 16;
// kind u8 | flags u8 | payloadBytes u32 | payloadCrc u32 | payload length u32
constexpr std::size_t kRecordHeaderBytes = 14;

[[nodiscard]] std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[offset + static_cast<std::size_t>(index)]) << (8 * index);
  }
  return value;
}

[[nodiscard]] std::uint64_t readU64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[offset + static_cast<std::size_t>(index)]) << (8 * index);
  }
  return value;
}

void writeU16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value & 0xFFu);
  bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void writeU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    bytes[offset + static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu);
  }
}

void writeU64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    bytes[offset + static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu);
  }
}

[[nodiscard]] std::size_t bodyBytesOf(const std::vector<std::uint8_t>& bytes) {
  return static_cast<std::size_t>(readU64(bytes, 20));
}

[[nodiscard]] std::size_t trailerOffsetOf(const std::vector<std::uint8_t>& bytes) {
  return kHeaderBytes + bodyBytesOf(bytes);
}

// Recomputes the header checksum, the body checksum and the recorded file length
// so that a deliberately edited image is still framed correctly.
void refreshFraming(std::vector<std::uint8_t>& bytes) {
  const std::span<const std::uint8_t> prefix(bytes.data(), kHeaderPrefixBytes);
  writeU32(bytes, kHeaderPrefixBytes, crc32(prefix));
  const std::size_t trailer = trailerOffsetOf(bytes);
  const std::span<const std::uint8_t> body(bytes.data() + kHeaderBytes, bodyBytesOf(bytes));
  writeU32(bytes, trailer, crc32(body));
  writeU64(bytes, trailer + 4, static_cast<std::uint64_t>(bytes.size()));
}

// ---------------------------------------------------------------------------
// A scratch state file under the OS temporary directory.
// ---------------------------------------------------------------------------
[[nodiscard]] const std::string& stateDirectory() {
  static const std::string directory = [] {
    const char* temp = std::getenv("TEMP");
    const std::string base = (temp != nullptr && *temp != '\0') ? std::string(temp) : std::string(".");
    const std::string value = base + "\\wf_persistence_" + std::to_string(systemNow().nanos());
    std::error_code error;
    std::filesystem::create_directories(value, error);
    return value;
  }();
  return directory;
}

class Store {
 public:
  explicit Store(const std::string& name) : path_(stateDirectory() + "\\" + name + ".state") {
    remove();
  }

  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] std::string temporaryPath() const { return path_ + ".tmp"; }

  [[nodiscard]] std::vector<std::uint8_t> readBytes() const { return readBytesFrom(path_); }

  [[nodiscard]] static std::vector<std::uint8_t> readBytesFrom(const std::string& file) {
    std::ifstream stream(file, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                     std::istreambuf_iterator<char>());
  }

  void writeBytes(const std::vector<std::uint8_t>& bytes) const {
    std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }

  void writeText(const std::string& text) const {
    std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  }

  void writeTemporaryText(const std::string& text) const {
    std::ofstream stream(temporaryPath(), std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  }

  [[nodiscard]] std::string readText(const std::string& file) const {
    std::ifstream stream(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }

  void remove() const {
    std::error_code error;
    std::filesystem::remove(path_, error);
    std::filesystem::remove(temporaryPath(), error);
  }

  [[nodiscard]] bool temporaryExists() const {
    std::error_code error;
    return std::filesystem::exists(temporaryPath(), error);
  }

 private:
  std::string path_;
};

[[nodiscard]] RuntimeConfig configFor(const std::string& path) {
  RuntimeConfig config;
  config.statePath = path;
  config.fsyncState = false;
  return config;
}

// ---------------------------------------------------------------------------
// Record comparisons. No runtime record defines operator==, and the round trip
// has to prove every persisted field survived, so each record is compared field
// by field.
// ---------------------------------------------------------------------------
[[nodiscard]] bool sameLease(const Lease& left, const Lease& right) {
  return left.generation == right.generation && left.grantedAt == right.grantedAt &&
         left.expiresAt == right.expiresAt && left.renewalCount == right.renewalCount &&
         left.maxRenewals == right.maxRenewals;
}

[[nodiscard]] bool sameGrid(const ChannelGrid& left, const ChannelGrid& right) {
  return left.id == right.id && left.generation == right.generation && left.kind == right.kind &&
         left.anchorMhz == right.anchorMhz && left.slotWidthMhz == right.slotWidthMhz &&
         left.slotCount == right.slotCount &&
         left.minSlotsPerChannel == right.minSlotsPerChannel &&
         left.maxSlotsPerChannel == right.maxSlotsPerChannel && left.label == right.label;
}

[[nodiscard]] bool sameDomain(const SpectrumDomain& left, const SpectrumDomain& right) {
  return left.id == right.id && left.generation == right.generation && left.klass == right.klass &&
         left.grid == right.grid && left.gridGeneration == right.gridGeneration &&
         left.span == right.span && left.spanGeneration == right.spanGeneration &&
         left.portA == right.portA && left.portAGeneration == right.portAGeneration &&
         left.portB == right.portB && left.portBGeneration == right.portBGeneration &&
         left.requiresContiguity == right.requiresContiguity && left.label == right.label;
}

[[nodiscard]] bool sameExclusion(const ExclusionDomain& left, const ExclusionDomain& right) {
  return left.id == right.id && left.generation == right.generation && left.label == right.label &&
         left.members == right.members && left.guardBandMhz == right.guardBandMhz;
}

[[nodiscard]] bool sameEvidence(const CapabilityEvidence& left, const CapabilityEvidence& right) {
  return left.present == right.present && left.digest == right.digest && left.source == right.source;
}

[[nodiscard]] bool sameCapability(const SpectrumCapability& left, const SpectrumCapability& right) {
  return left.domain == right.domain && left.domainGeneration == right.domainGeneration &&
         left.grid == right.grid && left.gridGeneration == right.gridGeneration &&
         left.support == right.support &&
         left.firstAllocatableSlot == right.firstAllocatableSlot &&
         left.allocatableSlots == right.allocatableSlots &&
         left.minTunableMhz == right.minTunableMhz && left.maxTunableMhz == right.maxTunableMhz &&
         left.contiguityEnforced == right.contiguityEnforced &&
         left.conversionSupported == right.conversionSupported &&
         sameEvidence(left.conversionEvidence, right.conversionEvidence) &&
         sameEvidence(left.presenceEvidence, right.presenceEvidence) &&
         left.generation == right.generation && left.publisher == right.publisher &&
         left.fence == right.fence && left.publishedAt == right.publishedAt &&
         left.detail == right.detail;
}

[[nodiscard]] bool sameReservation(const SpectrumReservation& left,
                                   const SpectrumReservation& right) {
  return left.id == right.id && left.generation == right.generation &&
         left.requestId == right.requestId &&
         left.requestGeneration == right.requestGeneration && left.owner == right.owner &&
         left.ownerGeneration == right.ownerGeneration && left.state == right.state &&
         left.domains == right.domains &&
         left.domainGenerations == right.domainGenerations &&
         left.anchorDomain == right.anchorDomain && left.grid == right.grid &&
         left.gridGeneration == right.gridGeneration && left.slots == right.slots &&
         left.frequency == right.frequency && left.perDomainSlots == right.perDomainSlots &&
         left.crossGrid == right.crossGrid &&
         left.contiguityRequired == right.contiguityRequired &&
         left.continuityRequired == right.continuityRequired &&
         left.guardBandMhz == right.guardBandMhz &&
         left.exclusionDomain == right.exclusionDomain && sameLease(left.lease, right.lease) &&
         left.createdAt == right.createdAt && left.updatedAt == right.updatedAt &&
         left.activatedAt == right.activatedAt && left.deactivatedAt == right.deactivatedAt &&
         left.releasedAt == right.releasedAt && left.reclaimedAt == right.reclaimedAt &&
         left.capabilityGeneration == right.capabilityGeneration &&
         left.commitFence == right.commitFence &&
         left.lastOperation == right.lastOperation &&
         left.needsRevalidation == right.needsRevalidation && left.detail == right.detail;
}

[[nodiscard]] bool sameAudit(const AuditRecord& left, const AuditRecord& right) {
  return left.sequence == right.sequence && left.at == right.at && left.kind == right.kind &&
         left.fence == right.fence && left.outcome == right.outcome &&
         left.reservation == right.reservation &&
         left.reservationGeneration == right.reservationGeneration &&
         left.domain == right.domain && left.slots == right.slots &&
         left.frequency == right.frequency && left.detail == right.detail;
}

// ---------------------------------------------------------------------------
// A writer state that exercises every persisted record kind.
// ---------------------------------------------------------------------------
struct Ids {
  ReservationId reserved{};
  ReservationId active{};
  ReservationId released{};
  std::size_t auditsAtSave{0};
};

[[nodiscard]] EligibilityAuthority eligibilityOf(const SpectrumRuntime& runtime,
                                                 std::uint64_t generation = 1) {
  EligibilityAuthority token;
  token.generation = EligibilityAuthorityGeneration(generation);
  token.fence = runtime.fence();
  return token;
}

[[nodiscard]] ReservationAuthority reservationOf(const SpectrumRuntime& runtime,
                                                 std::uint64_t generation = 1) {
  ReservationAuthority token;
  token.generation = ReservationAuthorityGeneration(generation);
  token.fence = runtime.fence();
  return token;
}

[[nodiscard]] ActivationAuthority activationOf(const SpectrumRuntime& runtime,
                                               std::uint64_t generation = 1) {
  ActivationAuthority token;
  token.generation = ActivationAuthorityGeneration(generation);
  token.fence = runtime.fence();
  return token;
}

[[nodiscard]] ReleaseAuthority releaseOf(const SpectrumRuntime& runtime,
                                         std::uint64_t generation = 1) {
  ReleaseAuthority token;
  token.generation = ReleaseAuthorityGeneration(generation);
  token.fence = runtime.fence();
  return token;
}

[[nodiscard]] SpectrumRequest requestFor(const SpectrumRuntime& runtime, AllocationRequestId id,
                                         SpectrumDomainId domain, ChannelGridId grid,
                                         Instant at, Duration lease) {
  return wf_test::makeRequest(id, OwnerId(7), {domain}, {SpectrumDomainGeneration(1)}, grid,
                              GridGeneration(1), 1, at, lease, eligibilityOf(runtime),
                              reservationOf(runtime));
}

// Registers two grids, three domains, one exclusion domain and three
// capabilities, then commits a Reserved, an Active and a Released reservation.
[[nodiscard]] Ids populate(SpectrumRuntime& runtime) {
  Ids ids;
  (void)runtime.registerGrid(wf_test::fixedGrid(ChannelGridId(1), GridGeneration(1)));
  (void)runtime.registerGrid(wf_test::flexGrid(ChannelGridId(2), GridGeneration(1)));
  (void)runtime.registerDomain(wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1),
                                                   SpectrumDomainGeneration(1), GridGeneration(1),
                                                   ResourceClass::FiberSpan, true));
  (void)runtime.registerDomain(wf_test::makeDomain(SpectrumDomainId(2), ChannelGridId(2),
                                                   SpectrumDomainGeneration(1), GridGeneration(1),
                                                   ResourceClass::MediaChannel, true));
  (void)runtime.registerDomain(wf_test::makeDomain(SpectrumDomainId(3), ChannelGridId(1),
                                                   SpectrumDomainGeneration(1), GridGeneration(1),
                                                   ResourceClass::AbstractDomain, false));

  ExclusionDomain exclusion;
  exclusion.id = ExclusionDomainId(1);
  exclusion.generation = ExclusionDomainGeneration(1);
  exclusion.label = "exclusion-1";
  exclusion.members = {SpectrumDomainId(1), SpectrumDomainId(3)};
  exclusion.guardBandMhz = 12'500;
  (void)runtime.registerExclusionDomain(exclusion);

  (void)runtime.publishCapability(wf_test::makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, 96, runtime.fence(), ControllerId(1), CapabilityGeneration(1),
      true, 0xA1));
  (void)runtime.publishCapability(wf_test::makeCapability(
      SpectrumDomainId(2), ChannelGridId(2), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, 384, runtime.fence(), ControllerId(1), CapabilityGeneration(1),
      true, 0xA2));
  (void)runtime.publishCapability(wf_test::makeCapability(
      SpectrumDomainId(3), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, 96, runtime.fence(), ControllerId(1), CapabilityGeneration(1),
      true, 0xA3));

  const AllocationDecision first = runtime.allocate(
      requestFor(runtime, AllocationRequestId(1), SpectrumDomainId(1), ChannelGridId(1), kStart, kLease));
  ids.reserved = first.reservation;
  const AllocationDecision second = runtime.allocate(
      requestFor(runtime, AllocationRequestId(2), SpectrumDomainId(2), ChannelGridId(2), kStart, kLease));
  ids.active = second.reservation;
  const AllocationDecision third = runtime.allocate(
      requestFor(runtime, AllocationRequestId(3), SpectrumDomainId(3), ChannelGridId(1), kStart, kLease));
  ids.released = third.reservation;

  ids.auditsAtSave = runtime.auditSize();
  return ids;
}

[[nodiscard]] std::vector<std::uint8_t> writeBaseline(Store& store) {
  SpectrumRuntime writer(configFor(store.path()));
  (void)populate(writer);
  (void)writer.save();
  return store.readBytes();
}

// Loads one image with a fresh runtime and asserts the typed refusal together
// with the exact accounting of a refused load.
void expectRefused(const Store& store, const std::vector<std::uint8_t>& image,
                   const std::string& expectedDiagnostic) {
  store.writeBytes(image);
  SpectrumRuntime runtime(configFor(store.path()));
  const RecoveryReport report = runtime.recover();
  WF_CHECK(!report.recovered);
  WF_CHECK(report.status.code == StatusCode::Corruption);
  WF_CHECK_EQ(report.status.message, expectedDiagnostic);
  WF_REQUIRE(!report.diagnostics.empty());
  WF_CHECK_EQ(report.diagnostics.back(), expectedDiagnostic);
  WF_CHECK_EQ(report.reservationsRestored, std::size_t{0});
  WF_CHECK_EQ(runtime.stats().corruptionDetections, std::uint64_t{1});
  WF_CHECK(runtime.grids().empty());
  WF_CHECK(runtime.domains().empty());
  WF_CHECK(runtime.reservations().empty());
  WF_CHECK_EQ(runtime.auditSize(), std::size_t{2});
  WF_CHECK_EQ(runtime.stats().allocationsCommitted, std::uint64_t{0});
  WF_CHECK(store.readBytes() == image);
}

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------
WF_TEST(a_saved_image_restores_every_record_and_advances_the_fence) {
  Store store("round_trip");
  SpectrumRuntime writer(configFor(store.path()));
  const Ids ids = populate(writer);
  WF_REQUIRE(ids.reserved.valid());
  WF_REQUIRE(ids.active.valid());
  WF_REQUIRE(ids.released.valid());
  WF_CHECK(writer
               .activate(ids.active, ReservationGeneration(1), activationOf(writer),
                         kStart + Duration::minutes(1))
               .ok());
  WF_CHECK(writer
               .release(ids.released, ReservationGeneration(1), releaseOf(writer),
                        kStart + Duration::minutes(2))
               .ok());

  const std::vector<ChannelGrid> grids = writer.grids();
  const std::vector<SpectrumDomain> domains = writer.domains();
  const std::vector<ExclusionDomain> exclusions = writer.exclusionDomains();
  const std::optional<SpectrumCapability> capabilityOne = writer.capability(SpectrumDomainId(1));
  const std::optional<SpectrumCapability> capabilityTwo = writer.capability(SpectrumDomainId(2));
  const std::optional<SpectrumCapability> capabilityThree = writer.capability(SpectrumDomainId(3));
  const std::optional<SpectrumReservation> reservationOne = writer.reservation(ids.reserved);
  const std::optional<SpectrumReservation> reservationTwo = writer.reservation(ids.active);
  const std::optional<SpectrumReservation> reservationThree = writer.reservation(ids.released);
  const std::vector<AuditRecord> audits = writer.audit(AuditSequence(0), 0);
  const std::optional<SpectrumUsage> usageOne = writer.usage(SpectrumDomainId(1), kStart);
  const std::optional<SpectrumUsage> usageTwo = writer.usage(SpectrumDomainId(2), kStart);
  const std::optional<SpectrumUsage> usageThree = writer.usage(SpectrumDomainId(3), kStart);
  WF_REQUIRE(grids.size() == std::size_t{2});
  WF_REQUIRE(domains.size() == std::size_t{3});
  WF_REQUIRE(exclusions.size() == std::size_t{1});
  WF_REQUIRE(capabilityOne.has_value() && capabilityTwo.has_value() && capabilityThree.has_value());
  WF_REQUIRE(reservationOne.has_value() && reservationTwo.has_value() &&
             reservationThree.has_value());
  WF_REQUIRE(usageOne.has_value() && usageTwo.has_value() && usageThree.has_value());
  WF_CHECK(reservationOne->state == ReservationState::Reserved);
  WF_CHECK(reservationTwo->state == ReservationState::Active);
  WF_CHECK(reservationThree->state == ReservationState::Released);
  const std::size_t persistedAudits = audits.size();
  WF_REQUIRE(persistedAudits > std::size_t{0});

  const Status saved = writer.save();
  WF_CHECK(saved.ok());
  WF_CHECK_EQ(writer.stats().persistenceWrites, std::uint64_t{1});
  WF_CHECK_EQ(writer.stats().persistenceFailures, std::uint64_t{0});

  SpectrumRuntime reader(configFor(store.path()));
  const RecoveryReport report = reader.recover();
  WF_CHECK(report.status.ok());
  WF_CHECK(report.recovered);
  WF_CHECK_EQ(report.source, store.path());
  WF_CHECK(report.diagnostics.empty());
  WF_CHECK_EQ(report.recordsRejected, std::uint64_t{0});
  WF_CHECK_EQ(report.recordsRead, report.recordsAccepted);
  WF_CHECK_EQ(report.recordsRead, std::uint64_t{13} + persistedAudits);
  WF_CHECK_EQ(report.gridsRestored, std::size_t{2});
  WF_CHECK_EQ(report.domainsRestored, std::size_t{3});
  WF_CHECK_EQ(report.exclusionDomainsRestored, std::size_t{1});
  WF_CHECK_EQ(report.capabilitiesRestored, std::size_t{3});
  WF_CHECK_EQ(report.reservationsRestored, std::size_t{3});
  WF_CHECK_EQ(report.auditsRestored, persistedAudits);
  WF_CHECK_EQ(report.demotedFromActive, std::size_t{1});

  // The fence advances: a new epoch and a new incarnation, never the old one.
  WF_CHECK_EQ(reader.epoch().raw(), writer.epoch().raw() + 1);
  WF_CHECK(!(reader.incarnation() == writer.incarnation()));
  WF_CHECK(reader.fence() == report.fence);
  WF_CHECK_EQ(report.fence.epoch.raw(), writer.epoch().raw() + 1);
  WF_CHECK_EQ(report.runtimeGeneration.raw(), writer.runtimeGeneration().raw() + 1);
  WF_CHECK_EQ(reader.runtimeGeneration().raw(), writer.runtimeGeneration().raw() + 1);
  WF_CHECK_EQ(report.recoveryGeneration.raw(), std::uint64_t{1});
  WF_CHECK_EQ(reader.recoveryGeneration().raw(), std::uint64_t{1});
  WF_CHECK_EQ(reader.authorityState().eligibilityGeneration.raw(), std::uint64_t{1});
  WF_CHECK_EQ(reader.authorityState().releaseGeneration.raw(), std::uint64_t{1});
  WF_CHECK(reader.authorityState().fence == reader.fence());
  WF_CHECK_EQ(reader.stats().persistenceWrites, std::uint64_t{0});

  // Registrations are restored exactly.
  const std::vector<ChannelGrid> restoredGrids = reader.grids();
  WF_REQUIRE(restoredGrids.size() == grids.size());
  for (std::size_t index = 0; index < grids.size(); ++index) {
    WF_CHECK(sameGrid(restoredGrids[index], grids[index]));
  }
  const std::vector<SpectrumDomain> restoredDomains = reader.domains();
  WF_REQUIRE(restoredDomains.size() == domains.size());
  for (std::size_t index = 0; index < domains.size(); ++index) {
    WF_CHECK(sameDomain(restoredDomains[index], domains[index]));
  }
  const std::vector<ExclusionDomain> restoredExclusions = reader.exclusionDomains();
  WF_REQUIRE(restoredExclusions.size() == exclusions.size());
  WF_CHECK(sameExclusion(restoredExclusions[0], exclusions[0]));
  const std::optional<SpectrumCapability> restoredOne = reader.capability(SpectrumDomainId(1));
  const std::optional<SpectrumCapability> restoredTwo = reader.capability(SpectrumDomainId(2));
  const std::optional<SpectrumCapability> restoredThree = reader.capability(SpectrumDomainId(3));
  WF_REQUIRE(restoredOne.has_value() && restoredTwo.has_value() && restoredThree.has_value());
  WF_CHECK(sameCapability(*restoredOne, *capabilityOne));
  WF_CHECK(sameCapability(*restoredTwo, *capabilityTwo));
  WF_CHECK(sameCapability(*restoredThree, *capabilityThree));

  // A reservation that was merely RESERVED is restored untouched.
  const std::optional<SpectrumReservation> restoredReserved = reader.reservation(ids.reserved);
  WF_REQUIRE(restoredReserved.has_value());
  WF_CHECK(sameReservation(*restoredReserved, *reservationOne));
  WF_CHECK(!restoredReserved->needsRevalidation);

  // A reservation that was RELEASED stays terminal and unchanged.
  const std::optional<SpectrumReservation> restoredReleased = reader.reservation(ids.released);
  WF_REQUIRE(restoredReleased.has_value());
  WF_CHECK(sameReservation(*restoredReleased, *reservationThree));
  WF_CHECK(restoredReleased->state == ReservationState::Released);

  // A reservation that was ACTIVE is conservatively demoted: Reserved, flagged
  // for revalidation, generation advanced, every other field intact.
  const std::optional<SpectrumReservation> restoredActive = reader.reservation(ids.active);
  WF_REQUIRE(restoredActive.has_value());
  WF_CHECK(restoredActive->state == ReservationState::Reserved);
  WF_CHECK(restoredActive->needsRevalidation);
  WF_CHECK_EQ(restoredActive->generation.raw(), reservationTwo->generation.raw() + 1);
  WF_CHECK(restoredActive->id == reservationTwo->id);
  WF_CHECK_EQ(restoredActive->requestId.raw(), reservationTwo->requestId.raw());
  WF_CHECK_EQ(restoredActive->requestGeneration.raw(), reservationTwo->requestGeneration.raw());
  WF_CHECK_EQ(restoredActive->owner.raw(), reservationTwo->owner.raw());
  WF_CHECK(restoredActive->domains == reservationTwo->domains);
  WF_CHECK(restoredActive->domainGenerations == reservationTwo->domainGenerations);
  WF_CHECK(restoredActive->anchorDomain == reservationTwo->anchorDomain);
  WF_CHECK(restoredActive->grid == reservationTwo->grid);
  WF_CHECK(restoredActive->slots == reservationTwo->slots);
  WF_CHECK(restoredActive->frequency == reservationTwo->frequency);
  WF_CHECK(restoredActive->perDomainSlots == reservationTwo->perDomainSlots);
  WF_CHECK(sameLease(restoredActive->lease, reservationTwo->lease));
  WF_CHECK(restoredActive->createdAt == reservationTwo->createdAt);
  WF_CHECK(restoredActive->activatedAt == reservationTwo->activatedAt);
  WF_CHECK(restoredActive->commitFence == reservationTwo->commitFence);
  WF_CHECK(restoredActive->capabilityGeneration == reservationTwo->capabilityGeneration);

  // Audit history is restored in order, then the recovery itself is recorded.
  const std::vector<AuditRecord> restoredAudits = reader.audit(AuditSequence(0), 0);
  WF_REQUIRE(restoredAudits.size() == persistedAudits + 2);
  for (std::size_t index = 0; index < persistedAudits; ++index) {
    WF_CHECK(sameAudit(restoredAudits[index], audits[index]));
  }
  WF_CHECK(restoredAudits[persistedAudits].kind == AuditKind::ReservationDeactivated);
  WF_CHECK(restoredAudits[persistedAudits].reservation == ids.active);
  WF_CHECK_EQ(restoredAudits[persistedAudits].reservationGeneration.raw(),
              restoredActive->generation.raw());
  WF_CHECK(restoredAudits[persistedAudits + 1].kind == AuditKind::RecoveryPerformed);
  WF_CHECK_EQ(reader.lastAuditSequence().raw(), restoredAudits.back().sequence.raw());

  // Occupancy is rebuilt from the restored records: the live/free accounting is
  // identical, and the demotion is visible as reserved rather than active slots.
  const std::optional<SpectrumUsage> readerOne = reader.usage(SpectrumDomainId(1), kStart);
  const std::optional<SpectrumUsage> readerTwo = reader.usage(SpectrumDomainId(2), kStart);
  const std::optional<SpectrumUsage> readerThree = reader.usage(SpectrumDomainId(3), kStart);
  WF_REQUIRE(readerOne.has_value() && readerTwo.has_value() && readerThree.has_value());
  WF_CHECK_EQ(readerOne->liveSlots, usageOne->liveSlots);
  WF_CHECK_EQ(readerOne->freeSlots, usageOne->freeSlots);
  WF_CHECK(readerOne->freeRuns == usageOne->freeRuns);
  WF_CHECK_EQ(readerTwo->liveSlots, usageTwo->liveSlots);
  WF_CHECK_EQ(readerTwo->freeSlots, usageTwo->freeSlots);
  WF_CHECK(readerTwo->freeRuns == usageTwo->freeRuns);
  WF_CHECK_EQ(readerThree->freeSlots, usageThree->freeSlots);
  WF_CHECK_EQ(usageTwo->activeReservations, std::uint32_t{1});
  WF_CHECK_EQ(readerTwo->activeReservations, std::uint32_t{0});
  WF_CHECK_EQ(readerTwo->reservedSlots, std::uint32_t{1});
  WF_CHECK_EQ(readerThree->releasedReservations, std::uint32_t{1});

  // The request index is rebuilt: an identical replay returns the restored
  // reservation, and the identity counter continues where the image left off.
  const AllocationDecision replay = reader.allocate(requestFor(
      reader, AllocationRequestId(1), SpectrumDomainId(1), ChannelGridId(1), kStart, kLease));
  WF_CHECK(replay.allocated());
  WF_CHECK(replay.reservation == ids.reserved);
  WF_CHECK_EQ(reader.reservations().size(), std::size_t{3});
  WF_CHECK_EQ(reader.stats().allocationsCommitted, std::uint64_t{0});
  const AllocationDecision next = reader.allocate(requestFor(
      reader, AllocationRequestId(9), SpectrumDomainId(1), ChannelGridId(1), kStart, kLease));
  WF_REQUIRE(next.allocated());
  WF_CHECK_EQ(next.reservation.raw(), std::uint64_t{4});
  WF_CHECK_EQ(reader.reservations().size(), std::size_t{4});
}

WF_TEST(an_active_reservation_is_demoted_and_can_be_reactivated_after_recovery) {
  Store store("demotion");
  SpectrumRuntime writer(configFor(store.path()));
  (void)writer.registerGrid(wf_test::fixedGrid(ChannelGridId(1), GridGeneration(1)));
  (void)writer.registerDomain(wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1)));
  (void)writer.publishCapability(wf_test::makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, 96, writer.fence()));
  const AllocationDecision committed = writer.allocate(requestFor(
      writer, AllocationRequestId(1), SpectrumDomainId(1), ChannelGridId(1), kStart, kLease));
  WF_REQUIRE(committed.allocated());
  WF_CHECK(writer
               .activate(committed.reservation, ReservationGeneration(1), activationOf(writer),
                         kStart + Duration::minutes(1))
               .ok());
  const std::optional<SpectrumReservation> before = writer.reservation(committed.reservation);
  WF_REQUIRE(before.has_value());
  WF_CHECK(before->state == ReservationState::Active);
  WF_CHECK(!before->needsRevalidation);
  WF_CHECK(writer.save().ok());

  SpectrumRuntime reader(configFor(store.path()));
  const RecoveryReport report = reader.recover();
  WF_REQUIRE(report.status.ok());
  WF_CHECK_EQ(report.demotedFromActive, std::size_t{1});
  const std::optional<SpectrumReservation> demoted = reader.reservation(committed.reservation);
  WF_REQUIRE(demoted.has_value());
  WF_CHECK(demoted->state == ReservationState::Reserved);
  WF_CHECK(demoted->needsRevalidation);
  WF_CHECK_EQ(demoted->generation.raw(), std::uint64_t{2});
  WF_CHECK(sameLease(demoted->lease, before->lease));
  WF_CHECK(demoted->activatedAt == before->activatedAt);
  WF_CHECK(demoted->commitFence == before->commitFence);

  // A token minted by the previous incarnation cannot activate it any more. The
  // epoch moved on recovery, so the epoch is the first field that disagrees.
  const Status stale = reader.activate(committed.reservation, ReservationGeneration(2),
                                       activationOf(writer), kStart + Duration::minutes(2));
  WF_CHECK_EQ(std::string(toToken(stale.code)), std::string("stale-epoch"));
  ControllerFence previousEpoch = reader.fence();
  previousEpoch.epoch = writer.epoch();
  ActivationAuthority previousEpochToken;
  previousEpochToken.generation = ActivationAuthorityGeneration(1);
  previousEpochToken.fence = previousEpoch;
  const Status wrongEpoch = reader.activate(committed.reservation, ReservationGeneration(2),
                                            previousEpochToken, kStart + Duration::minutes(2));
  WF_CHECK_EQ(std::string(toToken(wrongEpoch.code)), std::string("stale-epoch"));
  WF_CHECK_EQ(reader.stats().activations, std::uint64_t{0});

  // Fresh activation authority, named at the advanced generation, clears the flag.
  WF_CHECK(reader
               .activate(committed.reservation, ReservationGeneration(2), activationOf(reader),
                         kStart + Duration::minutes(2))
               .ok());
  const std::optional<SpectrumReservation> reactivated = reader.reservation(committed.reservation);
  WF_REQUIRE(reactivated.has_value());
  WF_CHECK(reactivated->state == ReservationState::Active);
  WF_CHECK(!reactivated->needsRevalidation);
  WF_CHECK_EQ(reader.stats().activations, std::uint64_t{1});
  const std::optional<SpectrumUsage> usage = reader.usage(SpectrumDomainId(1), kStart);
  WF_REQUIRE(usage.has_value());
  WF_CHECK_EQ(usage->activeReservations, std::uint32_t{1});
  WF_CHECK_EQ(usage->reservedSlots, std::uint32_t{0});
}

WF_TEST(a_save_after_recovery_produces_a_loadable_image) {
  Store store("resave");
  SpectrumRuntime writer(configFor(store.path()));
  const Ids ids = populate(writer);
  WF_REQUIRE(ids.reserved.valid());
  WF_CHECK(writer.save().ok());

  SpectrumRuntime reader(configFor(store.path()));
  const RecoveryReport first = reader.recover();
  WF_REQUIRE(first.status.ok());
  WF_CHECK_EQ(first.reservationsRestored, std::size_t{3});
  WF_CHECK_EQ(first.fence.epoch.raw(), std::uint64_t{2});
  const AllocationDecision fourth = reader.allocate(requestFor(
      reader, AllocationRequestId(9), SpectrumDomainId(1), ChannelGridId(1), kStart, kLease));
  WF_REQUIRE(fourth.allocated());
  WF_CHECK_EQ(fourth.reservation.raw(), std::uint64_t{4});
  WF_CHECK(reader.save().ok());
  WF_CHECK_EQ(reader.stats().persistenceWrites, std::uint64_t{1});

  SpectrumRuntime third(configFor(store.path()));
  const RecoveryReport second = third.recover();
  WF_CHECK(second.status.ok());
  WF_CHECK(second.recovered);
  WF_CHECK_EQ(second.fence.epoch.raw(), std::uint64_t{3});
  WF_CHECK_EQ(third.epoch().raw(), std::uint64_t{3});
  WF_CHECK_EQ(second.runtimeGeneration.raw(), std::uint64_t{3});
  WF_CHECK_EQ(second.recoveryGeneration.raw(), std::uint64_t{2});
  WF_CHECK_EQ(second.reservationsRestored, std::size_t{4});
  WF_CHECK_EQ(second.gridsRestored, std::size_t{2});
  WF_CHECK_EQ(second.domainsRestored, std::size_t{3});
  WF_CHECK_EQ(second.exclusionDomainsRestored, std::size_t{1});
  WF_CHECK_EQ(second.capabilitiesRestored, std::size_t{3});
  // The reader's image carries the restored history plus the recovery record and
  // the commit of the fourth reservation.
  WF_CHECK_EQ(second.auditsRestored, first.auditsRestored + 2);
  WF_CHECK(!(third.incarnation() == reader.incarnation()));
  WF_CHECK(!(third.incarnation() == writer.incarnation()));
  WF_CHECK(third.reservation(ids.reserved).has_value());
  WF_CHECK(third.reservation(ids.active).has_value());
  WF_CHECK(third.reservation(ids.released).has_value());
  const std::optional<SpectrumReservation> carried = third.reservation(fourth.reservation);
  WF_REQUIRE(carried.has_value());
  WF_CHECK(carried->state == ReservationState::Reserved);
  WF_CHECK(carried->requestId == AllocationRequestId(9));
  WF_CHECK(third.reservation(ids.released)->state == ReservationState::Reserved);
  WF_CHECK_EQ(third.auditSize(), second.auditsRestored + 1);
  const std::vector<AuditRecord> thirdAudits = third.audit(AuditSequence(0), 0);
  WF_REQUIRE(!thirdAudits.empty());
  WF_CHECK(thirdAudits.back().kind == AuditKind::RecoveryPerformed);
}

WF_TEST(a_missing_image_is_reported_as_missing_not_as_corruption) {
  Store store("missing");
  SpectrumRuntime runtime(configFor(store.path()));
  WF_CHECK(!std::filesystem::exists(store.path()));
  const RecoveryReport report = runtime.recover();
  WF_CHECK(!report.recovered);
  WF_CHECK_EQ(std::string(toToken(report.status.code)), std::string("not-found"));
  WF_CHECK_EQ(report.source, store.path());
  WF_REQUIRE(!report.diagnostics.empty());
  WF_CHECK_EQ(runtime.stats().corruptionDetections, std::uint64_t{0});
  WF_CHECK_EQ(runtime.auditSize(), std::size_t{1});
  WF_CHECK(runtime.grids().empty());
}

WF_TEST(a_runtime_without_a_state_path_cannot_save_or_recover) {
  SpectrumRuntime runtime(RuntimeConfig{});
  WF_CHECK_EQ(std::string(toToken(runtime.save().code)), std::string("invalid-argument"));
  const RecoveryReport report = runtime.recover();
  WF_CHECK(!report.recovered);
  WF_CHECK_EQ(std::string(toToken(report.status.code)), std::string("invalid-argument"));
  WF_CHECK_EQ(report.status.message, std::string("the runtime has no configured state path"));
  WF_CHECK_EQ(runtime.stats().persistenceWrites, std::uint64_t{0});
  WF_CHECK_EQ(runtime.stats().corruptionDetections, std::uint64_t{0});
}

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------
WF_TEST(a_truncated_image_is_refused) {
  Store store("truncated");
  const std::vector<std::uint8_t> valid = writeBaseline(store);
  const std::size_t framing = kHeaderBytes + kTrailerBytes;
  WF_REQUIRE(valid.size() > framing);

  expectRefused(store, std::vector<std::uint8_t>{},
                std::string("state container is shorter than its fixed framing"));

  const std::vector<std::uint8_t> tooShort(valid.begin(), valid.begin() + 30);
  expectRefused(store, tooShort,
                std::string("state container is shorter than its fixed framing"));

  // One byte short of the fixed framing is still "too short"; exactly the
  // framing size parses as a header and is refused on its body length.
  const std::vector<std::uint8_t> oneByteShort(valid.begin(), valid.begin() + (framing - 1));
  expectRefused(store, oneByteShort,
                std::string("state container is shorter than its fixed framing"));
  const std::vector<std::uint8_t> framingOnly(valid.begin(), valid.begin() + framing);
  expectRefused(store, framingOnly,
                std::string("state body length does not match the container length"));

  const std::vector<std::uint8_t> droppedTail(valid.begin(), valid.end() - 8);
  expectRefused(store, droppedTail,
                std::string("state body length does not match the container length"));

  std::vector<std::uint8_t> trailing = valid;
  trailing.insert(trailing.end(), {0x00, 0x01, 0x02, 0x03});
  expectRefused(store, trailing,
                std::string("state body length does not match the container length"));
}

WF_TEST(a_corrupted_header_is_refused) {
  Store store("header");
  const std::vector<std::uint8_t> valid = writeBaseline(store);
  WF_REQUIRE(valid.size() > kHeaderBytes + kTrailerBytes);

  {
    std::vector<std::uint8_t> image = valid;
    image[0] = 'X';
    expectRefused(store, image, std::string("state container magic is wrong"));
  }
  {
    std::vector<std::uint8_t> image = valid;
    writeU16(image, 8, 2);
    expectRefused(store, image,
                  std::string("state format version 2 is not supported by this build (expected 1)"));
  }
  {
    std::vector<std::uint8_t> image = valid;
    writeU16(image, 10, 1);
    expectRefused(store, image, std::string("state header declares unsupported flags"));
  }
  {
    std::vector<std::uint8_t> image = valid;
    writeU32(image, 12, 64);
    expectRefused(store, image, std::string("state header size field is wrong"));
  }
  {
    std::vector<std::uint8_t> image = valid;
    writeU32(image, 16, 7);
    expectRefused(store, image, std::string("state header checksum does not match"));
  }
  {
    std::vector<std::uint8_t> image = valid;
    image[kHeaderPrefixBytes] ^= 0xFFu;
    expectRefused(store, image, std::string("state header checksum does not match"));
  }
  {
    // A body length that disagrees with the container length, with the header
    // checksum repaired so that the length check is the one that fires.
    std::vector<std::uint8_t> image = valid;
    writeU64(image, 20, static_cast<std::uint64_t>(bodyBytesOf(valid)) + 4);
    refreshFraming(image);
    expectRefused(store, image,
                  std::string("state body length does not match the container length"));
  }
  {
    std::vector<std::uint8_t> image = valid;
    writeU64(image, 28, readU64(valid, 28) + 1);
    refreshFraming(image);
    expectRefused(store, image, std::string("state record count does not match the header"));
  }
}

WF_TEST(a_corrupted_body_is_refused) {
  Store store("body");
  const std::vector<std::uint8_t> valid = writeBaseline(store);
  WF_REQUIRE(valid.size() > kHeaderBytes + kTrailerBytes);

  {
    std::vector<std::uint8_t> image = valid;
    image[kHeaderBytes + 5] ^= 0x40u;
    expectRefused(store, image, std::string("state body checksum does not match"));
  }
  {
    // The same byte flip with the framing repaired: the per-record checksum is
    // what refuses it now.
    std::vector<std::uint8_t> image = valid;
    image[kHeaderBytes + 30] ^= 0x40u;
    refreshFraming(image);
    expectRefused(store, image, std::string("state record checksum does not match"));
  }
  {
    std::vector<std::uint8_t> image = valid;
    writeU32(image, kHeaderBytes + 2, 0x00FF'FFFFu);
    refreshFraming(image);
    expectRefused(store, image, std::string("state record length is out of range"));
  }
  {
    std::vector<std::uint8_t> image = valid;
    image[kHeaderBytes] = 0;
    refreshFraming(image);
    expectRefused(store, image, std::string("state record 1 (kind 0) is malformed or duplicated"));
  }
}

WF_TEST(a_corrupted_trailer_is_refused) {
  Store store("trailer");
  const std::vector<std::uint8_t> valid = writeBaseline(store);
  const std::size_t trailer = trailerOffsetOf(valid);
  WF_REQUIRE(trailer + kTrailerBytes == valid.size());
  WF_CHECK_EQ(readU32(valid, trailer + 12), std::uint32_t{0});

  {
    std::vector<std::uint8_t> image = valid;
    writeU32(image, trailer, readU32(valid, trailer) ^ 0x0000'FFFFu);
    expectRefused(store, image, std::string("state body checksum does not match"));
  }
  {
    std::vector<std::uint8_t> image = valid;
    writeU64(image, trailer + 4, static_cast<std::uint64_t>(valid.size()) + 1);
    expectRefused(store, image, std::string("state trailer length does not match the file"));
  }
  {
    std::vector<std::uint8_t> image = valid;
    writeU32(image, trailer + 12, 1);
    expectRefused(store, image, std::string("state trailer declares unsupported flags"));
  }
  {
    std::vector<std::uint8_t> image = valid;
    image[trailer + kTrailerMagicOffset] = 'X';
    expectRefused(store, image, std::string("state trailer magic is wrong"));
  }
}

WF_TEST(a_duplicated_record_is_refused) {
  Store store("duplicate");
  const std::vector<std::uint8_t> valid = writeBaseline(store);
  WF_REQUIRE(valid.size() > kHeaderBytes + kTrailerBytes);

  const std::size_t metadataPayload = readU32(valid, kHeaderBytes + 2);
  const std::size_t gridStart = kHeaderBytes + kRecordHeaderBytes + metadataPayload;
  WF_REQUIRE(static_cast<std::size_t>(valid[gridStart]) == 2);
  WF_REQUIRE(readU32(valid, gridStart + 2) > 0);
  const std::size_t gridRecordBytes = kRecordHeaderBytes + readU32(valid, gridStart + 2);

  std::vector<std::uint8_t> image = valid;
  image.insert(image.begin() + static_cast<std::ptrdiff_t>(gridStart + gridRecordBytes),
               valid.begin() + static_cast<std::ptrdiff_t>(gridStart),
               valid.begin() + static_cast<std::ptrdiff_t>(gridStart + gridRecordBytes));
  writeU64(image, 20, static_cast<std::uint64_t>(bodyBytesOf(valid) + gridRecordBytes));
  writeU64(image, 28, readU64(valid, 28) + 1);
  refreshFraming(image);
  WF_CHECK_EQ(image.size(), valid.size() + gridRecordBytes);
  expectRefused(store, image, std::string("state record 3 (kind 2) is malformed or duplicated"));
}

WF_TEST(two_conflicting_live_reservations_make_the_image_unloadable) {
  Store store("conflict");
  SpectrumRuntime writer(configFor(store.path()));
  (void)writer.registerGrid(wf_test::fixedGrid(ChannelGridId(1), GridGeneration(1)));
  (void)writer.registerDomain(wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1)));
  (void)writer.publishCapability(wf_test::makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, 96, writer.fence()));

  const AllocationDecision first = writer.allocate(requestFor(
      writer, AllocationRequestId(1), SpectrumDomainId(1), ChannelGridId(1), kStart, kLease));
  WF_REQUIRE(first.allocated());
  // A second identity evaluated after the first lease lapsed reuses the slot.
  const AllocationDecision second = writer.allocate(
      requestFor(writer, AllocationRequestId(2), SpectrumDomainId(1), ChannelGridId(1),
                 kStart + kLease, kLease));
  WF_REQUIRE(second.allocated());
  const std::optional<SpectrumReservation> firstStored = writer.reservation(first.reservation);
  const std::optional<SpectrumReservation> secondStored = writer.reservation(second.reservation);
  WF_REQUIRE(firstStored.has_value() && secondStored.has_value());
  WF_CHECK_EQ(firstStored->slots.first, std::uint32_t{0});
  WF_CHECK_EQ(secondStored->slots.first, std::uint32_t{0});

  // Control: the two leases do not overlap in time, so the image is loadable.
  WF_CHECK(writer.save().ok());
  {
    SpectrumRuntime control(configFor(store.path()));
    const RecoveryReport report = control.recover();
    WF_CHECK(report.status.ok());
    WF_CHECK_EQ(report.reservationsRestored, std::size_t{2});
  }

  // Extending the first lease past the start of the second makes both live at
  // the last recorded activity instant while both own the same slot.
  WF_CHECK(writer
               .renew(first.reservation, ReservationGeneration(1), Duration::minutes(5),
                      reservationOf(writer), kStart + Duration::minutes(5), 0)
               .ok());
  const std::optional<SpectrumReservation> extended = writer.reservation(first.reservation);
  WF_REQUIRE(extended.has_value());
  WF_CHECK(extended->lease.expiresAt == kStart + Duration::minutes(15));
  WF_CHECK(secondStored->lease.expiresAt == kStart + Duration::minutes(20));
  WF_CHECK(writer.save().ok());

  expectRefused(store, store.readBytes(),
                std::string("state contains two conflicting live reservations; refusing to load it"));
}

WF_TEST(a_leftover_temporary_file_is_ignored) {
  Store store("temporary");
  const std::vector<std::uint8_t> published = writeBaseline(store);

  // An interrupted write leaves a partial temporary behind.
  const std::string leftover = "partial temporary from an interrupted write";
  store.writeTemporaryText(leftover);
  WF_CHECK(store.temporaryExists());

  SpectrumRuntime reader(configFor(store.path()));
  const RecoveryReport report = reader.recover();
  WF_CHECK(report.status.ok());
  WF_CHECK(report.recovered);
  WF_CHECK_EQ(report.source, store.path());
  WF_CHECK_EQ(report.reservationsRestored, std::size_t{3});
  WF_CHECK(store.readBytes() == published);
  WF_CHECK_EQ(store.readText(store.temporaryPath()), leftover);

  // The reverse direction: a valid image parked in the temporary never rescues a
  // corrupted published image.
  std::vector<std::uint8_t> corrupted = published;
  corrupted.resize(20);
  store.writeBytes(corrupted);
  store.writeBytes(corrupted);
  {
    std::ofstream stream(store.temporaryPath(), std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(published.data()),
                 static_cast<std::streamsize>(published.size()));
  }
  SpectrumRuntime refusing(configFor(store.path()));
  const RecoveryReport refused = refusing.recover();
  WF_CHECK(!refused.recovered);
  WF_CHECK(refused.status.code == StatusCode::Corruption);
  WF_CHECK(Store::readBytesFrom(store.temporaryPath()) == published);
  WF_CHECK(store.readBytes() == corrupted);

  // A save on the same path publishes a fresh image and clears the temporary.
  SpectrumRuntime saviour(configFor(store.path()));
  WF_CHECK(saviour.save().ok());
  WF_CHECK(!store.temporaryExists());
  const RecoveryReport reloaded = saviour.recover();
  WF_CHECK(reloaded.status.ok());
  WF_CHECK(reloaded.recovered);
  WF_CHECK_EQ(reloaded.gridsRestored, std::size_t{0});
  WF_CHECK_EQ(reloaded.reservationsRestored, std::size_t{0});
}

}  // namespace

WF_TEST_MAIN()
