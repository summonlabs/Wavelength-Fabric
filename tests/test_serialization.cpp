#include "test_common.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <wavelength_fabric/serialization.hpp>

// Proofs for the bounded binary codec.
//
// Every claim here is about an exact byte sequence: a value that survives a
// round trip bit for bit, a malformed buffer that is refused, or a bound that
// is enforced before anything is allocated. A rejection is always checked
// together with the decoder state and the untouched output value, and an
// acceptance is always checked against the exact bytes it produced.

namespace {

using namespace wavelength_fabric;

#define WF_EXPECT(expr, detail)                                         \
  do {                                                                  \
    ::wf_test::Harness::instance().noteCheck();                         \
    if (!(expr)) {                                                      \
      ::wf_test::Harness::instance().fail(                              \
          __FILE__, __LINE__,                                           \
          std::string("expected: " #expr " : ") + std::string(detail)); \
    }                                                                   \
  } while (false)

// ---------------------------------------------------------------------------
// Structural comparison. The codec types deliberately have no operator==, so
// every field is compared explicitly: a field the decoder dropped cannot hide
// behind a byte comparison of the re-encoded value alone.
// ---------------------------------------------------------------------------

bool sameSlotRange(const SlotRange& a, const SlotRange& b) {
  return a.first == b.first && a.count == b.count;
}

bool sameSlotRanges(const std::vector<SlotRange>& a, const std::vector<SlotRange>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!sameSlotRange(a[i], b[i])) return false;
  }
  return true;
}

bool sameFrequencyRange(const FrequencyRange& a, const FrequencyRange& b) {
  return a.lowMhz == b.lowMhz && a.highMhz == b.highMhz;
}

bool sameFrequencyRanges(const std::vector<FrequencyRange>& a,
                         const std::vector<FrequencyRange>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!sameFrequencyRange(a[i], b[i])) return false;
  }
  return true;
}

bool sameDomainIds(const std::vector<SpectrumDomainId>& a,
                   const std::vector<SpectrumDomainId>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

bool sameReservationIds(const std::vector<ReservationId>& a,
                        const std::vector<ReservationId>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

bool sameStrings(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  return a == b;
}

bool sameFence(const ControllerFence& a, const ControllerFence& b) {
  return a.epoch == b.epoch && a.incarnation == b.incarnation;
}

bool sameAuthorityState(const AuthorityState& a, const AuthorityState& b) {
  return a.eligibilityGeneration == b.eligibilityGeneration &&
         a.reservationGeneration == b.reservationGeneration &&
         a.activationGeneration == b.activationGeneration &&
         a.releaseGeneration == b.releaseGeneration && sameFence(a.fence, b.fence);
}

bool sameStatus(const Status& a, const Status& b) {
  return a.code == b.code && a.message == b.message;
}

template <class Token>
bool sameAuthorityToken(const Token& a, const Token& b) {
  return a.generation == b.generation && sameFence(a.fence, b.fence);
}

bool sameGrid(const ChannelGrid& a, const ChannelGrid& b) {
  return a.id == b.id && a.generation == b.generation && a.kind == b.kind &&
         a.anchorMhz == b.anchorMhz && a.slotWidthMhz == b.slotWidthMhz &&
         a.slotCount == b.slotCount && a.minSlotsPerChannel == b.minSlotsPerChannel &&
         a.maxSlotsPerChannel == b.maxSlotsPerChannel && a.label == b.label;
}

bool sameSpan(const Span& a, const Span& b) {
  return a.id == b.id && a.generation == b.generation && a.label == b.label;
}

bool samePort(const OpticalPort& a, const OpticalPort& b) {
  return a.id == b.id && a.generation == b.generation && a.label == b.label;
}

bool sameDomain(const SpectrumDomain& a, const SpectrumDomain& b) {
  return a.id == b.id && a.generation == b.generation && a.klass == b.klass &&
         a.grid == b.grid && a.gridGeneration == b.gridGeneration && a.span == b.span &&
         a.spanGeneration == b.spanGeneration && a.portA == b.portA &&
         a.portAGeneration == b.portAGeneration && a.portB == b.portB &&
         a.portBGeneration == b.portBGeneration && a.requiresContiguity == b.requiresContiguity &&
         a.label == b.label;
}

bool sameExclusionDomain(const ExclusionDomain& a, const ExclusionDomain& b) {
  return a.id == b.id && a.generation == b.generation && a.label == b.label &&
         sameDomainIds(a.members, b.members) && a.guardBandMhz == b.guardBandMhz;
}

bool sameEvidence(const CapabilityEvidence& a, const CapabilityEvidence& b) {
  return a.present == b.present && a.digest == b.digest && a.source == b.source;
}

bool sameCapability(const SpectrumCapability& a, const SpectrumCapability& b) {
  return a.domain == b.domain && a.domainGeneration == b.domainGeneration && a.grid == b.grid &&
         a.gridGeneration == b.gridGeneration && a.support == b.support &&
         a.firstAllocatableSlot == b.firstAllocatableSlot &&
         a.allocatableSlots == b.allocatableSlots && a.minTunableMhz == b.minTunableMhz &&
         a.maxTunableMhz == b.maxTunableMhz && a.contiguityEnforced == b.contiguityEnforced &&
         a.conversionSupported == b.conversionSupported &&
         sameEvidence(a.conversionEvidence, b.conversionEvidence) &&
         sameEvidence(a.presenceEvidence, b.presenceEvidence) && a.generation == b.generation &&
         a.publisher == b.publisher && sameFence(a.fence, b.fence) &&
         a.publishedAt == b.publishedAt && a.detail == b.detail;
}

bool sameConstraints(const SpectrumConstraints& a, const SpectrumConstraints& b) {
  return sameSlotRanges(a.excludedSlots, b.excludedSlots) &&
         sameFrequencyRanges(a.excludedFrequencies, b.excludedFrequencies) &&
         sameReservationIds(a.mustNotConflictWith, b.mustNotConflictWith);
}

bool sameRequest(const SpectrumRequest& a, const SpectrumRequest& b) {
  return a.requestId == b.requestId && a.requestGeneration == b.requestGeneration &&
         a.owner == b.owner && a.ownerGeneration == b.ownerGeneration &&
         sameDomainIds(a.domains, b.domains) && a.domainGenerations == b.domainGenerations &&
         a.grid == b.grid && a.gridGeneration == b.gridGeneration && a.slots == b.slots &&
         a.contiguity == b.contiguity && a.continuity == b.continuity &&
         sameFrequencyRanges(a.frequencyWindows, b.frequencyWindows) &&
         a.guardBandMhz == b.guardBandMhz && a.leaseDuration == b.leaseDuration &&
         a.maxRenewals == b.maxRenewals && a.requestedAt == b.requestedAt &&
         a.notBefore == b.notBefore && a.policyGeneration == b.policyGeneration &&
         a.priorityGeneration == b.priorityGeneration &&
         sameConstraints(a.constraints, b.constraints) &&
         sameAuthorityToken(a.eligibilityAuthority, b.eligibilityAuthority) &&
         sameAuthorityToken(a.reservationAuthority, b.reservationAuthority);
}

bool sameCandidate(const SpectrumCandidate& a, const SpectrumCandidate& b) {
  return a.ordinal == b.ordinal && a.anchorDomain == b.anchorDomain &&
         sameSlotRange(a.slots, b.slots) && sameFrequencyRange(a.frequency, b.frequency) &&
         a.eligibility == b.eligibility && sameReservationIds(a.conflicts, b.conflicts) &&
         sameSlotRanges(a.perDomainSlots, b.perDomainSlots) && a.crossGrid == b.crossGrid &&
         a.detail == b.detail;
}

bool sameCandidateSet(const CandidateSet& a, const CandidateSet& b) {
  if (!sameStatus(a.status, b.status) || a.candidates.size() != b.candidates.size() ||
      a.eligibleCount != b.eligibleCount || a.omitted != b.omitted || a.complete != b.complete ||
      a.summary != b.summary) {
    return false;
  }
  for (std::size_t i = 0; i < a.candidates.size(); ++i) {
    if (!sameCandidate(a.candidates[i], b.candidates[i])) return false;
  }
  return true;
}

bool sameExplanation(const DecisionExplanation& a, const DecisionExplanation& b) {
  if (a.outcome != b.outcome || a.reservation != b.reservation || a.generation != b.generation ||
      !sameFence(a.fence, b.fence) || a.candidatesEnumerated != b.candidatesEnumerated ||
      a.candidatesRejected != b.candidatesRejected || a.candidatesOmitted != b.candidatesOmitted ||
      a.candidateOrdinal != b.candidateOrdinal || !sameCandidate(a.selected, b.selected) ||
      !sameReservationIds(a.conflicts, b.conflicts) || !sameStrings(a.reasons, b.reasons) ||
      a.rejected.size() != b.rejected.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.rejected.size(); ++i) {
    if (!sameCandidate(a.rejected[i], b.rejected[i])) return false;
  }
  return true;
}

bool sameDecision(const AllocationDecision& a, const AllocationDecision& b) {
  return sameStatus(a.status, b.status) && a.outcome == b.outcome &&
         a.reservation == b.reservation && a.generation == b.generation &&
         sameExplanation(a.explanation, b.explanation);
}

bool sameReclaimReport(const ReclaimReport& a, const ReclaimReport& b) {
  return sameStatus(a.status, b.status) && a.evaluatedAt == b.evaluatedAt &&
         sameFence(a.fence, b.fence) && a.scanned == b.scanned && a.expired == b.expired &&
         sameReservationIds(a.reclaimed, b.reclaimed) && sameReservationIds(a.lapsed, b.lapsed);
}

bool sameRecoveryReport(const RecoveryReport& a, const RecoveryReport& b) {
  return sameStatus(a.status, b.status) && a.recovered == b.recovered && a.source == b.source &&
         sameFence(a.fence, b.fence) && a.runtimeGeneration == b.runtimeGeneration &&
         a.recoveryGeneration == b.recoveryGeneration && a.recordsRead == b.recordsRead &&
         a.recordsAccepted == b.recordsAccepted && a.recordsRejected == b.recordsRejected &&
         a.gridsRestored == b.gridsRestored && a.domainsRestored == b.domainsRestored &&
         a.exclusionDomainsRestored == b.exclusionDomainsRestored &&
         a.capabilitiesRestored == b.capabilitiesRestored &&
         a.reservationsRestored == b.reservationsRestored && a.auditsRestored == b.auditsRestored &&
         a.demotedFromActive == b.demotedFromActive && sameStrings(a.diagnostics, b.diagnostics);
}

bool sameLease(const Lease& a, const Lease& b) {
  return a.generation == b.generation && a.grantedAt == b.grantedAt &&
         a.expiresAt == b.expiresAt && a.renewalCount == b.renewalCount &&
         a.maxRenewals == b.maxRenewals;
}

bool sameReservation(const SpectrumReservation& a, const SpectrumReservation& b) {
  return a.id == b.id && a.generation == b.generation && a.requestId == b.requestId &&
         a.requestGeneration == b.requestGeneration && a.owner == b.owner &&
         a.ownerGeneration == b.ownerGeneration && a.state == b.state &&
         sameDomainIds(a.domains, b.domains) && a.domainGenerations == b.domainGenerations &&
         a.anchorDomain == b.anchorDomain && a.grid == b.grid &&
         a.gridGeneration == b.gridGeneration && sameSlotRange(a.slots, b.slots) &&
         sameFrequencyRange(a.frequency, b.frequency) &&
         sameSlotRanges(a.perDomainSlots, b.perDomainSlots) && a.crossGrid == b.crossGrid &&
         a.contiguityRequired == b.contiguityRequired &&
         a.continuityRequired == b.continuityRequired && a.guardBandMhz == b.guardBandMhz &&
         a.exclusionDomain == b.exclusionDomain && sameLease(a.lease, b.lease) &&
         a.createdAt == b.createdAt && a.updatedAt == b.updatedAt &&
         a.activatedAt == b.activatedAt && a.deactivatedAt == b.deactivatedAt &&
         a.releasedAt == b.releasedAt && a.reclaimedAt == b.reclaimedAt &&
         a.capabilityGeneration == b.capabilityGeneration &&
         sameFence(a.commitFence, b.commitFence) && a.lastOperation == b.lastOperation &&
         a.needsRevalidation == b.needsRevalidation && a.detail == b.detail;
}

bool sameUsage(const SpectrumUsage& a, const SpectrumUsage& b) {
  return a.domain == b.domain && a.domainGeneration == b.domainGeneration && a.grid == b.grid &&
         a.gridGeneration == b.gridGeneration && a.totalSlots == b.totalSlots &&
         a.allocatableSlots == b.allocatableSlots && a.liveSlots == b.liveSlots &&
         a.activeSlots == b.activeSlots && a.reservedSlots == b.reservedSlots &&
         a.freeSlots == b.freeSlots && a.lapsedSlots == b.lapsedSlots &&
         a.liveReservations == b.liveReservations &&
         a.activeReservations == b.activeReservations &&
         a.lapsedReservations == b.lapsedReservations &&
         a.reclaimedReservations == b.reclaimedReservations &&
         a.releasedReservations == b.releasedReservations && sameSlotRanges(a.freeRuns, b.freeRuns);
}

bool sameAudit(const AuditRecord& a, const AuditRecord& b) {
  return a.sequence == b.sequence && a.at == b.at && a.kind == b.kind &&
         sameFence(a.fence, b.fence) && a.outcome == b.outcome && a.reservation == b.reservation &&
         a.reservationGeneration == b.reservationGeneration && a.domain == b.domain &&
         sameSlotRange(a.slots, b.slots) && sameFrequencyRange(a.frequency, b.frequency) &&
         a.detail == b.detail;
}

// ---------------------------------------------------------------------------
// Round trip and truncation drivers
// ---------------------------------------------------------------------------

template <class T, class Same>
void expectRoundTrip(const T& value, Same same, const std::string& what) {
  Encoder out;
  encode(out, value);
  WF_EXPECT(!out.overflowed(), what + ": encoding must not overflow the encoder");
  const std::vector<std::uint8_t> bytes = out.buffer();
  WF_EXPECT(!bytes.empty(), what + ": an encoded value is never zero bytes");

  T decoded{};
  Decoder in(bytes);
  WF_EXPECT(decode(in, decoded), what + ": decoding its own encoding must succeed");
  WF_EXPECT(in.ok(), what + ": a successful decode leaves the decoder usable");
  WF_EXPECT(in.exhausted(), what + ": decoding consumes exactly the encoded bytes");
  WF_EXPECT(same(value, decoded), what + ": the decoded value must equal the original");

  Encoder again;
  encode(again, decoded);
  WF_EXPECT(again.buffer() == bytes, what + ": re-encoding the decoded value is byte-identical");
}

template <class T, class Same>
void expectTruncationRejected(const T& value, Same same, const std::string& what) {
  Encoder out;
  encode(out, value);
  const std::vector<std::uint8_t> bytes = out.buffer();
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    const std::vector<std::uint8_t> prefix(
        bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
    T target = value;
    Decoder in(prefix);
    const bool accepted = decode(in, target);
    WF_EXPECT(!accepted, what + ": a " + std::to_string(length) + "-byte prefix must be refused");
    WF_EXPECT(!in.ok(), what + ": a refused decode leaves the decoder failed");
    WF_EXPECT(same(target, value), what + ": a refused decode leaves the output untouched");
  }
}

// ---------------------------------------------------------------------------
// Exact byte builders. Each builder writes the fields the decoder expects, in
// order, so one field can be driven out of range in isolation.
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> statusBytes(std::uint8_t codeTag, const std::string& message) {
  Encoder out;
  out.u8(codeTag);
  out.string(message);
  return out.take();
}

std::vector<std::uint8_t> gridBytes(std::uint8_t kindTag) {
  Encoder out;
  out.u64(1);
  out.u64(1);
  out.u8(kindTag);
  out.i64(0);
  out.i64(50'000);
  out.u32(96);
  out.u32(1);
  out.u32(1);
  out.string("g");
  return out.take();
}

std::vector<std::uint8_t> domainBytes(std::uint8_t classTag) {
  Encoder out;
  out.u64(1);
  out.u64(1);
  out.u8(classTag);
  for (int field = 0; field < 8; ++field) out.u64(0);
  out.boolean(true);
  out.string("d");
  return out.take();
}

std::vector<std::uint8_t> capabilityBytes(std::uint8_t supportTag) {
  Encoder out;
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u8(supportTag);
  out.u32(0);
  out.u32(96);
  out.i64(191'300'000);
  out.i64(196'100'000);
  out.boolean(true);
  out.boolean(false);
  out.boolean(false);
  out.u64(0);
  out.string("");
  out.boolean(true);
  out.u64(0x5EED);
  out.string("fixture");
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.i64(1'800'000'000'000'000'000ll);
  out.string("cap");
  return out.take();
}

std::vector<std::uint8_t> requestBytes(std::uint8_t contiguityTag, std::uint8_t continuityTag,
                                       std::uint32_t slots) {
  Encoder out;
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u32(1);
  out.u64(1);
  out.u32(1);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u32(slots);
  out.u8(contiguityTag);
  out.u8(continuityTag);
  out.u32(0);
  out.i64(0);
  out.i64(1'000'000'000);
  out.u32(0);
  out.i64(1);
  out.i64(0);
  out.u64(1);
  out.u64(1);
  out.u32(0);
  out.u32(0);
  out.u32(0);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  return out.take();
}

std::vector<std::uint8_t> candidateBytes(std::uint8_t eligibilityTag, std::uint32_t ordinal) {
  Encoder out;
  out.u32(ordinal);
  out.u64(1);
  out.u32(0);
  out.u32(1);
  out.i64(191'300'000);
  out.i64(191'350'000);
  out.u8(eligibilityTag);
  out.u32(0);
  out.u32(0);
  out.boolean(false);
  out.string("c");
  return out.take();
}

void writeSelectedCandidate(Encoder& out) {
  out.u32(0);
  out.u64(1);
  out.u32(0);
  out.u32(1);
  out.i64(191'300'000);
  out.i64(191'350'000);
  out.u8(0);
  out.u32(0);
  out.u32(0);
  out.boolean(false);
  out.string("");
}

void writeExplanationBody(Encoder& out, std::uint8_t outcomeTag) {
  out.u8(outcomeTag);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u32(0);
  out.u32(0);
  out.u32(0);
  out.u32(0);
  writeSelectedCandidate(out);
  out.u32(0);
  out.u32(0);
  out.u32(0);
}

std::vector<std::uint8_t> explanationBytes(std::uint8_t outcomeTag) {
  Encoder out;
  writeExplanationBody(out, outcomeTag);
  return out.take();
}

std::vector<std::uint8_t> decisionBytes(std::uint8_t outcomeTag) {
  Encoder out;
  out.u8(0);
  out.string("");
  out.u8(outcomeTag);
  out.u64(1);
  out.u64(1);
  writeExplanationBody(out, 0);
  return out.take();
}

std::vector<std::uint8_t> reservationBytes(std::uint8_t stateTag) {
  Encoder out;
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u8(stateTag);
  out.u32(0);
  out.u32(0);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.u32(0);
  out.u32(1);
  out.i64(191'300'000);
  out.i64(191'350'000);
  out.u32(0);
  out.boolean(false);
  out.boolean(false);
  out.boolean(false);
  out.i64(0);
  out.u64(0);
  out.u64(1);
  out.i64(0);
  out.i64(1);
  out.u32(0);
  out.u32(0);
  for (int field = 0; field < 6; ++field) out.i64(0);
  out.u64(0);
  out.u64(1);
  out.u64(1);
  out.u64(1);
  out.boolean(false);
  out.string("");
  return out.take();
}

std::vector<std::uint8_t> auditBytes(std::uint8_t kindTag, std::uint8_t outcomeTag) {
  Encoder out;
  out.u64(1);
  out.i64(1);
  out.u8(kindTag);
  out.u64(1);
  out.u64(1);
  out.u8(outcomeTag);
  out.u64(0);
  out.u64(0);
  out.u64(0);
  out.u32(0);
  out.u32(0);
  out.i64(0);
  out.i64(0);
  out.string("");
  return out.take();
}

// ---------------------------------------------------------------------------
// Sample values that satisfy every bound the decoders enforce.
// ---------------------------------------------------------------------------

ControllerFence sampleFence() {
  ControllerFence fence;
  fence.epoch = ControllerEpoch(2);
  fence.incarnation = ControllerIncarnation(3);
  return fence;
}

SpectrumRequest sampleRequest() {
  SpectrumRequest request;
  request.requestId = AllocationRequestId(7);
  request.requestGeneration = AllocationRequestGeneration(3);
  request.owner = OwnerId(11);
  request.ownerGeneration = OwnerGeneration(5);
  request.domains = {SpectrumDomainId(1), SpectrumDomainId(2)};
  request.domainGenerations = {SpectrumDomainGeneration(1), SpectrumDomainGeneration(1)};
  request.grid = ChannelGridId(1);
  request.gridGeneration = GridGeneration(1);
  request.slots = 2;
  request.contiguity = ContiguityRequirement::Required;
  request.continuity = ContinuityRequirement::Required;
  request.frequencyWindows = {FrequencyRange{191'300'000, 196'100'000}};
  request.guardBandMhz = 12'500;
  request.leaseDuration = Duration::seconds(600);
  request.maxRenewals = 4;
  request.requestedAt = Instant::fromSeconds(1'800'000'000);
  request.notBefore = Instant::fromSeconds(1'800'000'100);
  request.policyGeneration = PolicyGeneration(2);
  request.priorityGeneration = PriorityGeneration(9);
  request.constraints.excludedSlots = {SlotRange{90, 6}};
  request.constraints.excludedFrequencies = {FrequencyRange{195'000'000, 195'100'000}};
  request.constraints.mustNotConflictWith = {ReservationId(4)};
  request.eligibilityAuthority.generation = EligibilityAuthorityGeneration(1);
  request.eligibilityAuthority.fence = sampleFence();
  request.reservationAuthority.generation = ReservationAuthorityGeneration(1);
  request.reservationAuthority.fence = sampleFence();
  return request;
}

SpectrumReservation sampleReservation() {
  SpectrumReservation reservation;
  reservation.id = ReservationId(9);
  reservation.generation = ReservationGeneration(2);
  reservation.requestId = AllocationRequestId(7);
  reservation.requestGeneration = AllocationRequestGeneration(3);
  reservation.owner = OwnerId(11);
  reservation.ownerGeneration = OwnerGeneration(5);
  reservation.state = ReservationState::Active;
  reservation.domains = {SpectrumDomainId(1), SpectrumDomainId(2)};
  reservation.domainGenerations = {SpectrumDomainGeneration(1), SpectrumDomainGeneration(1)};
  reservation.anchorDomain = SpectrumDomainId(1);
  reservation.grid = ChannelGridId(1);
  reservation.gridGeneration = GridGeneration(1);
  reservation.slots = SlotRange{10, 2};
  reservation.frequency = FrequencyRange{191'800'000, 191'900'000};
  reservation.perDomainSlots = {SlotRange{10, 2}, SlotRange{10, 2}};
  reservation.crossGrid = false;
  reservation.contiguityRequired = true;
  reservation.continuityRequired = true;
  reservation.guardBandMhz = 12'500;
  reservation.exclusionDomain = ExclusionDomainId(3);
  reservation.lease.generation = LeaseGeneration(4);
  reservation.lease.grantedAt = Instant::fromSeconds(1'800'000'000);
  reservation.lease.expiresAt = Instant::fromSeconds(1'800'000'600);
  reservation.lease.renewalCount = 1;
  reservation.lease.maxRenewals = 4;
  reservation.createdAt = Instant::fromSeconds(1'800'000'000);
  reservation.updatedAt = Instant::fromSeconds(1'800'000'010);
  reservation.activatedAt = Instant::fromSeconds(1'800'000'020);
  reservation.deactivatedAt = Instant::fromSeconds(1'800'000'030);
  reservation.releasedAt = Instant::fromSeconds(1'800'000'040);
  reservation.reclaimedAt = Instant::fromSeconds(1'800'000'050);
  reservation.capabilityGeneration = CapabilityGeneration(6);
  reservation.commitFence = sampleFence();
  reservation.lastOperation = OperationGeneration(8);
  reservation.needsRevalidation = true;
  reservation.detail = "sample";
  return reservation;
}

// Upper bound the codec enforces on a candidate's conflict list. It is not a
// public constant, so it is restated here as the value the decoder documents.
constexpr std::uint32_t kMaxConflictEntries = 4096;

std::uint32_t referenceCrc32Value(const std::vector<std::uint8_t>& data) {
  std::uint32_t crc = 0xFFFF'FFFFu;
  for (const std::uint8_t byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) != 0u ? (0xEDB8'8320u ^ (crc >> 1)) : (crc >> 1);
    }
  }
  return crc ^ 0xFFFF'FFFFu;
}

}  // namespace

// ---------------------------------------------------------------------------
// CRC-32
// ---------------------------------------------------------------------------

WF_TEST(crc32_matches_published_vectors) {
  const auto crcOf = [](const std::string& text) {
    return crc32(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
  };
  WF_CHECK_EQ(crcOf(""), std::uint32_t(0x0000'0000u));
  WF_CHECK_EQ(crcOf("a"), std::uint32_t(0xE8B7'BE43u));
  WF_CHECK_EQ(crcOf("abc"), std::uint32_t(0x3524'41C2u));
  WF_CHECK_EQ(crcOf("123456789"), std::uint32_t(0xCBF4'3926u));
  WF_CHECK_EQ(crcOf("The quick brown fox jumps over the lazy dog"), std::uint32_t(0x414F'A339u));
}

WF_TEST(crc32_matches_an_independent_bitwise_reference) {
  std::uint64_t state = 0x0123'4567'89AB'CDEFull;
  const auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  for (std::size_t length = 0; length <= 512; length += 37) {
    std::vector<std::uint8_t> data(length);
    for (std::uint8_t& byte : data) byte = static_cast<std::uint8_t>(next() & 0xFFu);
    WF_EXPECT(crc32(data) == referenceCrc32Value(data),
              "length " + std::to_string(length) + " must agree with the bitwise reference");
  }
  const std::vector<std::uint8_t> zero{0x00u};
  WF_CHECK_EQ(crc32(zero), referenceCrc32Value(zero));
  const std::vector<std::uint8_t> ones{0xFFu, 0xFFu, 0xFFu, 0xFFu};
  WF_CHECK_EQ(crc32(ones), referenceCrc32Value(ones));
}

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

WF_TEST(primitive_round_trips) {
  expectRoundTrip(SlotRange{0, 0}, sameSlotRange, "SlotRange empty");
  expectRoundTrip(SlotRange{0xFFFF'FFFFu, 0}, sameSlotRange, "SlotRange empty at the index bound");
  expectRoundTrip(SlotRange{0, kMaxGridSlots}, sameSlotRange, "SlotRange at the count bound");
  expectRoundTrip(SlotRange{95, 1}, sameSlotRange, "SlotRange single slot");

  expectRoundTrip(FrequencyRange{0, 0}, sameFrequencyRange, "FrequencyRange empty");
  expectRoundTrip(FrequencyRange{1, kMaxFrequencyMhz}, sameFrequencyRange,
                  "FrequencyRange spanning the whole model");
  expectRoundTrip(FrequencyRange{191'300'000, 191'350'000}, sameFrequencyRange, "FrequencyRange");

  ControllerFence fence;
  fence.epoch = ControllerEpoch(4);
  fence.incarnation = ControllerIncarnation(0xFFFF'FFFF'FFFF'FFFEull);
  expectRoundTrip(fence, sameFence, "ControllerFence at the fence bound");

  AuthorityState authority;
  authority.eligibilityGeneration = EligibilityAuthorityGeneration(1);
  authority.reservationGeneration = ReservationAuthorityGeneration(2);
  authority.activationGeneration = ActivationAuthorityGeneration(3);
  authority.releaseGeneration = ReleaseAuthorityGeneration(4);
  authority.fence = fence;
  expectRoundTrip(authority, sameAuthorityState, "AuthorityState");

  expectRoundTrip(Status{}, sameStatus, "Status ok with an empty message");
  expectRoundTrip(Status::failure(StatusCode::Corruption, std::string(4096, 'x')), sameStatus,
                  "Status at the message bound");

  EligibilityAuthority eligibility;
  eligibility.generation = EligibilityAuthorityGeneration(7);
  eligibility.fence = fence;
  expectRoundTrip(eligibility, sameAuthorityToken<EligibilityAuthority>, "EligibilityAuthority");

  ReservationAuthority reservation;
  reservation.generation = ReservationAuthorityGeneration(7);
  reservation.fence = fence;
  expectRoundTrip(reservation, sameAuthorityToken<ReservationAuthority>, "ReservationAuthority");

  ActivationAuthority activation;
  activation.generation = ActivationAuthorityGeneration(7);
  activation.fence = fence;
  expectRoundTrip(activation, sameAuthorityToken<ActivationAuthority>, "ActivationAuthority");

  ReleaseAuthority release;
  release.generation = ReleaseAuthorityGeneration(7);
  release.fence = fence;
  expectRoundTrip(release, sameAuthorityToken<ReleaseAuthority>, "ReleaseAuthority");
}

WF_TEST(domain_round_trips) {
  expectRoundTrip(wf_test::fixedGrid(), sameGrid, "ChannelGrid fixed");
  expectRoundTrip(wf_test::flexGrid(), sameGrid, "ChannelGrid flex");

  Span span;
  span.id = SpanId(3);
  span.generation = SpanGeneration(2);
  span.label = std::string(256, 's');
  expectRoundTrip(span, sameSpan, "Span at the label bound");

  OpticalPort port;
  port.id = PortId(4);
  port.generation = PortGeneration(2);
  port.label = "port";
  expectRoundTrip(port, samePort, "OpticalPort");

  SpectrumDomain domain = wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1));
  domain.span = SpanId(3);
  domain.spanGeneration = SpanGeneration(2);
  domain.portA = PortId(4);
  domain.portAGeneration = PortGeneration(2);
  domain.portB = PortId(5);
  domain.portBGeneration = PortGeneration(2);
  expectRoundTrip(domain, sameDomain, "SpectrumDomain with both endpoints");

  ExclusionDomain exclusion;
  exclusion.id = ExclusionDomainId(2);
  exclusion.generation = ExclusionDomainGeneration(1);
  exclusion.label = "exclusion";
  exclusion.members = {SpectrumDomainId(1), SpectrumDomainId(2), SpectrumDomainId(3)};
  exclusion.guardBandMhz = kMaxGuardBandMhz;
  expectRoundTrip(exclusion, sameExclusionDomain, "ExclusionDomain at the guard bound");

  CapabilityEvidence evidence;
  evidence.present = true;
  evidence.digest = 0xDEAD'BEEFull;
  evidence.source = "evidence";
  expectRoundTrip(evidence, sameEvidence, "CapabilityEvidence");

  SpectrumCapability capability =
      wf_test::makeCapability(SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1),
                              GridGeneration(1), SpectrumSupport::Supported, 0, 96, sampleFence());
  capability.conversionSupported = true;
  capability.conversionEvidence.present = true;
  capability.conversionEvidence.digest = 0x1234'5678u;
  capability.conversionEvidence.source = "conversion";
  expectRoundTrip(capability, sameCapability, "SpectrumCapability with conversion evidence");
}

WF_TEST(request_and_decision_round_trips) {
  expectRoundTrip(sampleRequest(), sameRequest, "SpectrumRequest");

  SpectrumCandidate candidate;
  candidate.ordinal = kMaxCandidatesPerRequest;
  candidate.anchorDomain = SpectrumDomainId(1);
  candidate.slots = SlotRange{10, 2};
  candidate.frequency = FrequencyRange{191'800'000, 191'900'000};
  candidate.eligibility = CandidateEligibility::IneligibleConflict;
  candidate.conflicts = {ReservationId(3), ReservationId(4)};
  candidate.perDomainSlots = {SlotRange{10, 2}, SlotRange{20, 2}};
  candidate.crossGrid = true;
  candidate.detail = "blocked";
  expectRoundTrip(candidate, sameCandidate, "SpectrumCandidate");

  CandidateSet set;
  set.status = okStatus();
  set.candidates = {candidate};
  set.eligibleCount = 1;
  set.omitted = 0;
  set.complete = false;
  set.summary = "summary";
  expectRoundTrip(set, sameCandidateSet, "CandidateSet");

  DecisionExplanation explanation;
  explanation.outcome = AllocationOutcome::RefusedConflict;
  explanation.reservation = ReservationId(9);
  explanation.generation = ReservationGeneration(2);
  explanation.fence = sampleFence();
  explanation.candidatesEnumerated = 12;
  explanation.candidatesRejected = 11;
  explanation.candidatesOmitted = 3;
  explanation.candidateOrdinal = 4;
  explanation.selected = candidate;
  explanation.rejected = {candidate, candidate};
  explanation.conflicts = {ReservationId(3)};
  explanation.reasons = {"first reason", "second reason"};
  expectRoundTrip(explanation, sameExplanation, "DecisionExplanation");

  AllocationDecision decision;
  decision.status = Status::failure(StatusCode::Conflict, "conflict");
  decision.outcome = AllocationOutcome::RefusedConflict;
  decision.reservation = ReservationId(9);
  decision.generation = ReservationGeneration(2);
  decision.explanation = explanation;
  expectRoundTrip(decision, sameDecision, "AllocationDecision");

  ReclaimReport reclaim;
  reclaim.status = okStatus();
  reclaim.evaluatedAt = Instant::fromSeconds(1'800'000'000);
  reclaim.fence = sampleFence();
  reclaim.scanned = 5;
  reclaim.expired = 2;
  reclaim.reclaimed = {ReservationId(1), ReservationId(2)};
  reclaim.lapsed = {ReservationId(3)};
  expectRoundTrip(reclaim, sameReclaimReport, "ReclaimReport");

  RecoveryReport recovery;
  recovery.status = Status::failure(StatusCode::Corruption, "corrupt");
  recovery.recovered = true;
  recovery.source = "state.wvl";
  recovery.fence = sampleFence();
  recovery.runtimeGeneration = RuntimeGeneration(3);
  recovery.recoveryGeneration = RecoveryGeneration(2);
  recovery.recordsRead = 100;
  recovery.recordsAccepted = 99;
  recovery.recordsRejected = 1;
  recovery.gridsRestored = 1;
  recovery.domainsRestored = 2;
  recovery.exclusionDomainsRestored = 3;
  recovery.capabilitiesRestored = 4;
  recovery.reservationsRestored = 5;
  recovery.auditsRestored = 6;
  recovery.demotedFromActive = 7;
  recovery.diagnostics = {"first", "second"};
  expectRoundTrip(recovery, sameRecoveryReport, "RecoveryReport");
}

WF_TEST(reservation_and_audit_round_trips) {
  Lease lease;
  lease.generation = LeaseGeneration(1);
  lease.grantedAt = Instant::fromSeconds(1'800'000'000);
  lease.expiresAt = Instant::fromSeconds(1'800'000'600);
  lease.renewalCount = 0;
  lease.maxRenewals = 0;
  expectRoundTrip(lease, sameLease, "Lease");

  expectRoundTrip(sampleReservation(), sameReservation, "SpectrumReservation");

  SpectrumUsage usage;
  usage.domain = SpectrumDomainId(1);
  usage.domainGeneration = SpectrumDomainGeneration(1);
  usage.grid = ChannelGridId(1);
  usage.gridGeneration = GridGeneration(1);
  usage.totalSlots = 96;
  usage.allocatableSlots = 96;
  usage.liveSlots = 4;
  usage.activeSlots = 2;
  usage.reservedSlots = 2;
  usage.freeSlots = 92;
  usage.lapsedSlots = 1;
  usage.liveReservations = 2;
  usage.activeReservations = 1;
  usage.lapsedReservations = 1;
  usage.reclaimedReservations = 3;
  usage.releasedReservations = 4;
  usage.freeRuns = {SlotRange{0, 4}, SlotRange{10, 86}};
  expectRoundTrip(usage, sameUsage, "SpectrumUsage");

  AuditRecord record;
  record.sequence = AuditSequence(17);
  record.at = Instant::fromSeconds(1'800'000'000);
  record.kind = AuditKind::AllocationCommitted;
  record.fence = sampleFence();
  record.outcome = AllocationOutcome::Allocated;
  record.reservation = ReservationId(5);
  record.reservationGeneration = ReservationGeneration(1);
  record.domain = SpectrumDomainId(1);
  record.slots = SlotRange{10, 2};
  record.frequency = FrequencyRange{191'800'000, 191'900'000};
  record.detail = "committed";
  expectRoundTrip(record, sameAudit, "AuditRecord");
}

// ---------------------------------------------------------------------------
// Exact byte layout
// ---------------------------------------------------------------------------

WF_TEST(encoded_layout_is_little_endian_and_exact) {
  Encoder range;
  encode(range, SlotRange{0x0102'0304u, 0x0506'0708u});
  const std::vector<std::uint8_t> expectedRange = {0x04, 0x03, 0x02, 0x01,
                                                   0x08, 0x07, 0x06, 0x05};
  WF_CHECK(range.buffer() == expectedRange);
  WF_CHECK_EQ(range.size(), std::size_t(8));

  Encoder frequency;
  encode(frequency, FrequencyRange{-1, -2});
  const std::vector<std::uint8_t> expectedFrequency = {
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  WF_CHECK(frequency.buffer() == expectedFrequency);

  Encoder fenceOut;
  ControllerFence value;
  value.epoch = ControllerEpoch(0x0102'0304'0506'0708ull);
  value.incarnation = ControllerIncarnation(0x1112'1314'1516'1718ull);
  encode(fenceOut, value);
  WF_CHECK_EQ(fenceOut.size(), std::size_t(16));
  WF_CHECK_EQ(fenceOut.buffer()[0], std::uint8_t(0x08));
  WF_CHECK_EQ(fenceOut.buffer()[7], std::uint8_t(0x01));
  WF_CHECK_EQ(fenceOut.buffer()[8], std::uint8_t(0x18));
  WF_CHECK_EQ(fenceOut.buffer()[15], std::uint8_t(0x11));

  Encoder primitives;
  primitives.u8(0xABu);
  primitives.u16(0x1234u);
  primitives.u32(0xDEAD'BEEFu);
  primitives.u64(0x0102'0304'0506'0708ull);
  primitives.boolean(true);
  primitives.boolean(false);
  primitives.string("hi");
  primitives.bytes(std::span<const std::uint8_t>());
  const std::vector<std::uint8_t> expected = {
      0xABu, 0x34u, 0x12u, 0xEFu, 0xBEu, 0xADu, 0xDEu, 0x08u, 0x07u,
      0x06u, 0x05u, 0x04u, 0x03u, 0x02u, 0x01u, 0x01u, 0x00u, 0x02u,
      0x00u, 0x00u, 0x00u, 0x68u, 0x69u, 0x00u, 0x00u, 0x00u, 0x00u};
  WF_CHECK(primitives.buffer() == expected);
  WF_CHECK_EQ(primitives.size(), expected.size());
}

WF_TEST(decoding_leaves_trailing_bytes_unconsumed) {
  Encoder out;
  encode(out, SlotRange{4, 2});
  std::vector<std::uint8_t> bytes = out.take();
  bytes.push_back(0xEEu);
  bytes.push_back(0xFFu);

  SlotRange decoded{};
  Decoder in(bytes);
  WF_CHECK(decode(in, decoded));
  WF_CHECK(sameSlotRange(decoded, SlotRange{4, 2}));
  WF_CHECK_EQ(in.offset(), std::size_t(8));
  WF_CHECK_EQ(in.remaining(), std::size_t(2));
  WF_CHECK(!in.exhausted());
  WF_CHECK(in.ok());
}

// ---------------------------------------------------------------------------
// Sticky decoder failure
// ---------------------------------------------------------------------------

WF_TEST(decoder_failure_is_sticky_and_preserves_the_output) {
  const std::vector<std::uint8_t> bytes = {0x01u, 0x02u, 0x03u};
  Decoder in(bytes);

  std::uint16_t first = 0xFFFFu;
  WF_CHECK(in.u16(first));
  WF_CHECK_EQ(first, std::uint16_t(0x0201u));
  WF_CHECK(in.ok());
  WF_CHECK_EQ(in.remaining(), std::size_t(1));

  std::uint32_t truncated = 0xDEAD'BEEFu;
  WF_CHECK(!in.u32(truncated));
  WF_CHECK_EQ(truncated, std::uint32_t(0xDEAD'BEEFu));
  WF_CHECK(!in.ok());

  std::uint8_t byte = 0x5Au;
  WF_CHECK(!in.u8(byte));
  WF_CHECK_EQ(byte, std::uint8_t(0x5Au));
  std::uint16_t shortValue = 0x1234u;
  WF_CHECK(!in.u16(shortValue));
  WF_CHECK_EQ(shortValue, std::uint16_t(0x1234u));
  std::uint64_t wide = 0x0102'0304'0506'0708ull;
  WF_CHECK(!in.u64(wide));
  WF_CHECK_EQ(wide, std::uint64_t(0x0102'0304'0506'0708ull));
  std::int64_t signedValue = -7;
  WF_CHECK(!in.i64(signedValue));
  WF_CHECK_EQ(signedValue, std::int64_t(-7));
  bool flag = true;
  WF_CHECK(!in.boolean(flag));
  WF_CHECK(flag);
  std::string text = "untouched";
  WF_CHECK(!in.string(text));
  WF_CHECK(text == "untouched");
  std::vector<std::uint8_t> blob = {1u, 2u};
  WF_CHECK(!in.bytes(blob, 16));
  WF_CHECK_EQ(blob.size(), std::size_t(2));
  WF_CHECK(!in.ok());
}

WF_TEST(explicit_fail_makes_every_later_read_fail) {
  const std::vector<std::uint8_t> bytes(14, 0x00u);
  Decoder in(bytes);
  in.fail();
  WF_CHECK(!in.ok());
  std::uint64_t value = 0;
  WF_CHECK(!in.u64(value));
  WF_CHECK_EQ(value, std::uint64_t(0));
  std::string text;
  WF_CHECK(!in.string(text));
  WF_CHECK_EQ(in.remaining(), std::size_t(14));
  WF_CHECK_EQ(in.offset(), std::size_t(0));
}

WF_TEST(composite_decode_does_not_write_a_partial_value) {
  const SpectrumReservation original = sampleReservation();
  Encoder out;
  encode(out, original);
  const std::vector<std::uint8_t> bytes = out.buffer();
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    SpectrumReservation target = original;
    target.detail = "sentinel";
    target.id = ReservationId(999);
    const std::vector<std::uint8_t> prefix(
        bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
    Decoder in(prefix);
    WF_EXPECT(!decode(in, target),
              "a " + std::to_string(length) + "-byte SpectrumReservation prefix must be refused");
    WF_EXPECT(target.id == ReservationId(999) && target.detail == "sentinel",
              "a refused SpectrumReservation decode must leave the output untouched");
  }

  const SpectrumRequest request = sampleRequest();
  Encoder requestOut;
  encode(requestOut, request);
  const std::vector<std::uint8_t> requestBytesValue = requestOut.buffer();
  for (std::size_t length = 0; length < requestBytesValue.size(); ++length) {
    SpectrumRequest target = request;
    target.requestId = AllocationRequestId(4242);
    const std::vector<std::uint8_t> prefix(
        requestBytesValue.begin(),
        requestBytesValue.begin() + static_cast<std::ptrdiff_t>(length));
    Decoder in(prefix);
    WF_EXPECT(!decode(in, target),
              "a " + std::to_string(length) + "-byte SpectrumRequest prefix must be refused");
    WF_EXPECT(target.requestId == AllocationRequestId(4242) && target.owner == request.owner,
              "a refused SpectrumRequest decode must leave the output untouched");
  }
}

WF_TEST(string_and_bytes_bounds_are_enforced_before_allocation) {
  Encoder declared;
  declared.u32(0x0001'0001u);
  declared.string("short");
  Decoder tooLong(declared.buffer());
  std::string text = "untouched";
  WF_CHECK(!tooLong.string(text));
  WF_CHECK(text == "untouched");
  WF_CHECK(!tooLong.ok());

  Encoder oversized;
  oversized.u32(kMaxEncodedStringBytes + 1u);
  Decoder over(oversized.buffer());
  std::string other = "untouched";
  WF_CHECK(!over.string(other));
  WF_CHECK(other == "untouched");
  WF_CHECK(!over.ok());

  Encoder exact;
  const std::string payload(kMaxEncodedStringBytes, 'z');
  exact.string(payload);
  WF_CHECK(!exact.overflowed());
  std::string decoded;
  Decoder exactIn(exact.buffer());
  WF_CHECK(exactIn.string(decoded));
  WF_CHECK_EQ(decoded.size(), payload.size());
  WF_CHECK(decoded == payload);

  Encoder blobOut;
  blobOut.u32(9);
  blobOut.u8(1);
  Decoder blobIn(blobOut.buffer());
  std::vector<std::uint8_t> blob = {7u, 8u};
  WF_CHECK(!blobIn.bytes(blob, 4));
  WF_CHECK_EQ(blob.size(), std::size_t(2));
  WF_CHECK_EQ(blob[0], std::uint8_t(7u));
  WF_CHECK(!blobIn.ok());
}

WF_TEST(boolean_rejects_every_non_boolean_tag) {
  for (std::uint8_t raw = 2; raw < 8; ++raw) {
    Encoder out;
    out.u8(raw);
    Decoder in(out.buffer());
    bool value = true;
    WF_EXPECT(!in.boolean(value), "boolean tag " + std::to_string(raw) + " must be refused");
    WF_EXPECT(value, "a refused boolean read leaves the output untouched");
    WF_EXPECT(!in.ok(), "a refused boolean read leaves the decoder failed");
  }
  for (std::uint8_t raw = 0; raw < 2; ++raw) {
    Encoder out;
    out.u8(raw);
    Decoder in(out.buffer());
    bool value = raw != 0;
    WF_EXPECT(in.boolean(value), "boolean tag " + std::to_string(raw) + " must be accepted");
    WF_EXPECT(value == (raw == 1), "boolean tag maps to the matching value");
  }
}

// ---------------------------------------------------------------------------
// Unknown enum tags
// ---------------------------------------------------------------------------

WF_TEST(unknown_enum_tags_are_rejected_at_the_exact_bound) {
  Status status = Status::failure(StatusCode::Unavailable, "sentinel");
  const Status statusSentinel = status;
  {
    const std::vector<std::uint8_t> bytes = statusBytes(21, "m");
    Decoder in(bytes);
    WF_CHECK(!decode(in, status));
    WF_CHECK(!in.ok());
    WF_CHECK(sameStatus(status, statusSentinel));
  }

  ChannelGrid grid = wf_test::flexGrid();
  const ChannelGrid gridSentinel = grid;
  {
    const std::vector<std::uint8_t> bytes = gridBytes(3);
    Decoder in(bytes);
    WF_CHECK(!decode(in, grid));
    WF_CHECK(!in.ok());
    WF_CHECK(sameGrid(grid, gridSentinel));
  }

  SpectrumDomain domain = wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1));
  const SpectrumDomain domainSentinel = domain;
  {
    const std::vector<std::uint8_t> bytes = domainBytes(6);
    Decoder in(bytes);
    WF_CHECK(!decode(in, domain));
    WF_CHECK(!in.ok());
    WF_CHECK(sameDomain(domain, domainSentinel));
  }

  SpectrumCapability capability =
      wf_test::makeCapability(SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1),
                              GridGeneration(1), SpectrumSupport::Supported, 0, 96, sampleFence());
  const SpectrumCapability capabilitySentinel = capability;
  {
    const std::vector<std::uint8_t> bytes = capabilityBytes(3);
    Decoder in(bytes);
    WF_CHECK(!decode(in, capability));
    WF_CHECK(!in.ok());
    WF_CHECK(sameCapability(capability, capabilitySentinel));
  }

  SpectrumRequest request = sampleRequest();
  const SpectrumRequest requestSentinel = request;
  {
    const std::vector<std::uint8_t> bytes = requestBytes(3, 0, 1);
    Decoder in(bytes);
    WF_CHECK(!decode(in, request));
    WF_CHECK(!in.ok());
    WF_CHECK(sameRequest(request, requestSentinel));
  }
  {
    const std::vector<std::uint8_t> bytes = requestBytes(0, 3, 1);
    Decoder in(bytes);
    WF_CHECK(!decode(in, request));
    WF_CHECK(!in.ok());
    WF_CHECK(sameRequest(request, requestSentinel));
  }

  SpectrumCandidate candidate;
  const SpectrumCandidate candidateSentinel = candidate;
  {
    const std::vector<std::uint8_t> bytes = candidateBytes(15, 0);
    Decoder in(bytes);
    WF_CHECK(!decode(in, candidate));
    WF_CHECK(!in.ok());
    WF_CHECK(sameCandidate(candidate, candidateSentinel));
  }

  DecisionExplanation explanation;
  const DecisionExplanation explanationSentinel = explanation;
  {
    const std::vector<std::uint8_t> bytes = explanationBytes(23);
    Decoder in(bytes);
    WF_CHECK(!decode(in, explanation));
    WF_CHECK(!in.ok());
    WF_CHECK(sameExplanation(explanation, explanationSentinel));
  }

  AllocationDecision decision;
  const AllocationDecision decisionSentinel = decision;
  {
    const std::vector<std::uint8_t> bytes = decisionBytes(23);
    Decoder in(bytes);
    WF_CHECK(!decode(in, decision));
    WF_CHECK(!in.ok());
    WF_CHECK(sameDecision(decision, decisionSentinel));
  }

  SpectrumReservation reservation = sampleReservation();
  const SpectrumReservation reservationSentinel = reservation;
  {
    const std::vector<std::uint8_t> bytes = reservationBytes(18);
    Decoder in(bytes);
    WF_CHECK(!decode(in, reservation));
    WF_CHECK(!in.ok());
    WF_CHECK(sameReservation(reservation, reservationSentinel));
  }

  AuditRecord record;
  const AuditRecord recordSentinel = record;
  {
    const std::vector<std::uint8_t> bytes = auditBytes(25, 0);
    Decoder in(bytes);
    WF_CHECK(!decode(in, record));
    WF_CHECK(!in.ok());
    WF_CHECK(sameAudit(record, recordSentinel));
  }
  {
    const std::vector<std::uint8_t> bytes = auditBytes(0, 23);
    Decoder in(bytes);
    WF_CHECK(!decode(in, record));
    WF_CHECK(!in.ok());
    WF_CHECK(sameAudit(record, recordSentinel));
  }
}

WF_TEST(every_defined_enum_tag_round_trips) {
  for (std::uint8_t tag = 0; tag <= 20; ++tag) {
    const std::vector<std::uint8_t> bytes = statusBytes(tag, "message");
    Decoder in(bytes);
    Status status;
    WF_EXPECT(decode(in, status) && status.code == static_cast<StatusCode>(tag),
              "StatusCode tag " + std::to_string(tag) + " must decode");
  }
  for (std::uint8_t tag = 0; tag <= 2; ++tag) {
    const std::vector<std::uint8_t> bytes = gridBytes(tag);
    Decoder in(bytes);
    ChannelGrid grid;
    WF_EXPECT(decode(in, grid) && grid.kind == static_cast<GridKind>(tag),
              "GridKind tag " + std::to_string(tag) + " must decode");
  }
  for (std::uint8_t tag = 0; tag <= 5; ++tag) {
    const std::vector<std::uint8_t> bytes = domainBytes(tag);
    Decoder in(bytes);
    SpectrumDomain domain;
    WF_EXPECT(decode(in, domain) && domain.klass == static_cast<ResourceClass>(tag),
              "ResourceClass tag " + std::to_string(tag) + " must decode");
  }
  for (std::uint8_t tag = 0; tag <= 2; ++tag) {
    const std::vector<std::uint8_t> bytes = capabilityBytes(tag);
    Decoder in(bytes);
    SpectrumCapability capability;
    WF_EXPECT(decode(in, capability) && capability.support == static_cast<SpectrumSupport>(tag),
              "SpectrumSupport tag " + std::to_string(tag) + " must decode");
  }
  for (std::uint8_t tag = 0; tag <= 14; ++tag) {
    const std::vector<std::uint8_t> bytes = candidateBytes(tag, 0);
    Decoder in(bytes);
    SpectrumCandidate candidate;
    WF_EXPECT(decode(in, candidate) &&
                  candidate.eligibility == static_cast<CandidateEligibility>(tag),
              "CandidateEligibility tag " + std::to_string(tag) + " must decode");
  }
  for (std::uint8_t tag = 0; tag <= 22; ++tag) {
    const std::vector<std::uint8_t> bytes = explanationBytes(tag);
    Decoder in(bytes);
    DecisionExplanation explanation;
    WF_EXPECT(decode(in, explanation) && explanation.outcome == static_cast<AllocationOutcome>(tag),
              "AllocationOutcome tag " + std::to_string(tag) + " must decode in an explanation");
    const std::vector<std::uint8_t> decisionValue = decisionBytes(tag);
    Decoder decisionIn(decisionValue);
    AllocationDecision decision;
    WF_EXPECT(decode(decisionIn, decision) &&
                  decision.outcome == static_cast<AllocationOutcome>(tag),
              "AllocationOutcome tag " + std::to_string(tag) + " must decode in a decision");
  }
  for (std::uint8_t tag = 0; tag <= 17; ++tag) {
    const std::vector<std::uint8_t> bytes = reservationBytes(tag);
    Decoder in(bytes);
    SpectrumReservation reservation;
    WF_EXPECT(decode(in, reservation) && reservation.state == static_cast<ReservationState>(tag),
              "ReservationState tag " + std::to_string(tag) + " must decode");
  }
  for (std::uint8_t tag = 0; tag <= 24; ++tag) {
    const std::vector<std::uint8_t> bytes = auditBytes(tag, 0);
    Decoder in(bytes);
    AuditRecord record;
    WF_EXPECT(decode(in, record) && record.kind == static_cast<AuditKind>(tag),
              "AuditKind tag " + std::to_string(tag) + " must decode");
  }
  for (std::uint8_t tag = 0; tag <= 2; ++tag) {
    const std::vector<std::uint8_t> bytes = requestBytes(tag, tag, 1);
    Decoder in(bytes);
    SpectrumRequest request;
    WF_EXPECT(decode(in, request) &&
                  request.contiguity == static_cast<ContiguityRequirement>(tag) &&
                  request.continuity == static_cast<ContinuityRequirement>(tag),
              "requirement tag " + std::to_string(tag) + " must decode");
  }
}

// ---------------------------------------------------------------------------
// Range bounds
// ---------------------------------------------------------------------------

WF_TEST(slot_range_bounds_are_enforced) {
  const auto accepted = [](std::uint32_t first, std::uint32_t count) {
    Encoder out;
    out.u32(first);
    out.u32(count);
    Decoder in(out.buffer());
    SlotRange range{0xAAAA'AAAAu, 0xBBBB'BBBBu};
    if (!decode(in, range)) return false;
    return range.first == first && range.count == count;
  };

  WF_CHECK(accepted(0, 0));
  WF_CHECK(accepted(0xFFFF'FFFFu, 0));
  WF_CHECK(accepted(0, kMaxGridSlots));
  WF_CHECK(accepted(0xFFFF'FFFFu - kMaxGridSlots, kMaxGridSlots));
  WF_CHECK(!accepted(0, kMaxGridSlots + 1u));
  WF_CHECK(!accepted(0xFFFF'F000u, 0x1000u));
  WF_CHECK(!accepted(0xFFFF'FFFFu, 2u));
  WF_CHECK(!accepted(0xFFFF'FFFFu, 1u));

  Encoder out;
  out.u32(0xFFFF'FFFFu);
  out.u32(1u);
  Decoder in(out.buffer());
  SlotRange range{7, 9};
  WF_CHECK(!decode(in, range));
  WF_CHECK(!in.ok());
  WF_CHECK(sameSlotRange(range, SlotRange{7, 9}));
}

WF_TEST(frequency_range_bounds_are_enforced) {
  const auto refused = [](std::int64_t low, std::int64_t high, const std::string& what) {
    Encoder out;
    out.i64(low);
    out.i64(high);
    Decoder in(out.buffer());
    FrequencyRange range{5, 6};
    const bool ok = decode(in, range);
    WF_EXPECT(!ok, what + " must be refused");
    WF_EXPECT(!in.ok(), what + " leaves the decoder failed");
    WF_EXPECT(sameFrequencyRange(range, FrequencyRange{5, 6}),
              what + " leaves the output untouched");
  };
  const auto accepted = [](std::int64_t low, std::int64_t high, const std::string& what) {
    Encoder out;
    out.i64(low);
    out.i64(high);
    Decoder in(out.buffer());
    FrequencyRange range;
    const bool ok = decode(in, range);
    WF_EXPECT(ok, what + " must be accepted");
    WF_EXPECT(range.lowMhz == low && range.highMhz == high, what + " must decode exactly");
  };

  refused(-1, 5, "a negative low edge");
  refused(5, 4, "a high edge below the low edge");
  refused(0, kMaxFrequencyMhz + 1, "a high edge above the model bound");
  accepted(0, 0, "an empty range at zero");
  accepted(0, kMaxFrequencyMhz, "the full model range");
  accepted(kMaxFrequencyMhz, kMaxFrequencyMhz, "an empty range at the bound");
}

WF_TEST(over_long_labels_details_and_messages_are_refused) {
  ChannelGrid grid = wf_test::flexGrid();
  grid.label = std::string(300, 'L');
  Encoder gridOut;
  encode(gridOut, grid);
  WF_CHECK(!gridOut.overflowed());
  Decoder gridIn(gridOut.buffer());
  ChannelGrid decodedGrid = wf_test::fixedGrid();
  WF_CHECK(!decode(gridIn, decodedGrid));
  WF_CHECK(!gridIn.ok());
  WF_CHECK(decodedGrid.id == ChannelGridId(1));
  WF_CHECK(decodedGrid.label == "fixed-1");

  Status status = Status::failure(StatusCode::IoError, std::string(5000, 'm'));
  Encoder statusOut;
  encode(statusOut, status);
  WF_CHECK(!statusOut.overflowed());
  Decoder statusIn(statusOut.buffer());
  Status decodedStatus = Status::failure(StatusCode::Ok, "sentinel");
  WF_CHECK(!decode(statusIn, decodedStatus));
  WF_CHECK(!statusIn.ok());
  WF_CHECK(decodedStatus.message == "sentinel");

  SpectrumReservation reservation = sampleReservation();
  reservation.detail = std::string(5000, 'd');
  Encoder reservationOut;
  encode(reservationOut, reservation);
  WF_CHECK(!reservationOut.overflowed());
  Decoder reservationIn(reservationOut.buffer());
  SpectrumReservation decodedReservation;
  WF_CHECK(!decode(reservationIn, decodedReservation));
  WF_CHECK(!reservationIn.ok());
  WF_CHECK(decodedReservation.detail.empty());
}

WF_TEST(over_long_collections_are_refused_before_allocation) {
  SpectrumRequest request = sampleRequest();
  Encoder out;
  out.u64(request.requestId.raw());
  out.u64(request.requestGeneration.raw());
  out.u64(request.owner.raw());
  out.u64(request.ownerGeneration.raw());
  out.u32(kMaxRequestDomains + 1u);
  for (std::uint32_t index = 0; index < kMaxRequestDomains + 1u; ++index) out.u64(index + 1u);
  out.u32(0);
  Decoder in(out.buffer());
  SpectrumRequest target;
  WF_CHECK(!decode(in, target));
  WF_CHECK(!in.ok());
  WF_CHECK(target.requestId.none());

  Encoder windowOut;
  windowOut.u64(1);
  windowOut.u64(1);
  windowOut.u64(1);
  windowOut.u64(1);
  windowOut.u32(1);
  windowOut.u64(1);
  windowOut.u32(1);
  windowOut.u64(1);
  windowOut.u64(1);
  windowOut.u64(1);
  windowOut.u32(1);
  windowOut.u8(0);
  windowOut.u8(0);
  windowOut.u32(kMaxFrequencyWindows + 1u);
  Decoder windowIn(windowOut.buffer());
  SpectrumRequest windowTarget;
  WF_CHECK(!decode(windowIn, windowTarget));
  WF_CHECK(!windowIn.ok());

  Encoder setOut;
  setOut.u8(0);
  setOut.string("");
  setOut.u32(kMaxCandidatesPerRequest + 1u);
  Decoder setIn(setOut.buffer());
  CandidateSet set;
  WF_CHECK(!decode(setIn, set));
  WF_CHECK(!setIn.ok());
  WF_CHECK(set.candidates.empty());

  Encoder conflictOut;
  conflictOut.u32(0);
  conflictOut.u64(1);
  conflictOut.u32(0);
  conflictOut.u32(1);
  conflictOut.i64(0);
  conflictOut.i64(0);
  conflictOut.u8(0);
  conflictOut.u32(kMaxConflictEntries + 1u);
  Decoder conflictIn(conflictOut.buffer());
  SpectrumCandidate candidate;
  WF_CHECK(!decode(conflictIn, candidate));
  WF_CHECK(!conflictIn.ok());
  WF_CHECK(candidate.conflicts.empty());
}

WF_TEST(semantically_inconsistent_records_are_refused) {
  SpectrumRequest expiredLease = sampleRequest();
  expiredLease.leaseDuration = Duration::zero();
  Encoder leaseOut;
  encode(leaseOut, expiredLease);
  Decoder leaseIn(leaseOut.buffer());
  SpectrumRequest leaseTarget;
  WF_CHECK(!decode(leaseIn, leaseTarget));

  SpectrumRequest negative = sampleRequest();
  negative.requestedAt = Instant::fromNanos(-1);
  Encoder negativeOut;
  encode(negativeOut, negative);
  Decoder negativeIn(negativeOut.buffer());
  SpectrumRequest negativeTarget;
  WF_CHECK(!decode(negativeIn, negativeTarget));
  WF_CHECK(!negativeIn.ok());

  SpectrumRequest defaulted;
  Encoder defaultOut;
  encode(defaultOut, defaulted);
  Decoder defaultIn(defaultOut.buffer());
  SpectrumRequest defaultTarget;
  WF_CHECK(!decode(defaultIn, defaultTarget));

  Encoder mismatch;
  mismatch.u64(1);
  mismatch.u64(1);
  mismatch.u64(1);
  mismatch.u64(1);
  mismatch.u32(2);
  mismatch.u64(1);
  mismatch.u64(2);
  mismatch.u32(1);
  mismatch.u64(1);
  mismatch.u64(1);
  mismatch.u64(1);
  mismatch.u32(1);
  mismatch.u8(0);
  mismatch.u8(0);
  mismatch.u32(0);
  mismatch.i64(0);
  mismatch.i64(1);
  mismatch.u32(0);
  mismatch.i64(1);
  mismatch.i64(0);
  mismatch.u64(1);
  mismatch.u64(1);
  mismatch.u32(0);
  mismatch.u32(0);
  mismatch.u32(0);
  mismatch.u64(1);
  mismatch.u64(1);
  mismatch.u64(1);
  mismatch.u64(1);
  mismatch.u64(1);
  mismatch.u64(1);
  Decoder mismatchIn(mismatch.buffer());
  SpectrumRequest mismatchTarget;
  WF_CHECK(!decode(mismatchIn, mismatchTarget));
  WF_CHECK(!mismatchIn.ok());

  Lease lease;
  lease.expiresAt = Instant::fromNanos(-1);
  Encoder leaseRecord;
  encode(leaseRecord, lease);
  Decoder leaseRecordIn(leaseRecord.buffer());
  Lease decodedLease;
  WF_CHECK(!decode(leaseRecordIn, decodedLease));

  Encoder candidates;
  candidates.u8(0);
  candidates.string("");
  candidates.u32(0);
  candidates.u32(1);
  candidates.u32(0);
  candidates.boolean(true);
  candidates.string("");
  Decoder candidatesIn(candidates.buffer());
  CandidateSet set;
  WF_CHECK(!decode(candidatesIn, set));

  SpectrumCandidate overOrdinal;
  overOrdinal.ordinal = kMaxCandidatesPerRequest + 1u;
  Encoder ordinalOut;
  encode(ordinalOut, overOrdinal);
  Decoder ordinalIn(ordinalOut.buffer());
  SpectrumCandidate ordinalTarget;
  WF_CHECK(!decode(ordinalIn, ordinalTarget));

  Encoder reclaim;
  reclaim.u8(0);
  reclaim.string("");
  reclaim.i64(-1);
  reclaim.u64(1);
  reclaim.u64(1);
  reclaim.u32(0);
  reclaim.u32(0);
  reclaim.u32(0);
  reclaim.u32(0);
  Decoder reclaimIn(reclaim.buffer());
  ReclaimReport report;
  WF_CHECK(!decode(reclaimIn, report));
}

// ---------------------------------------------------------------------------
// Truncation for every encoded type
// ---------------------------------------------------------------------------

WF_TEST(every_type_refuses_a_truncated_buffer) {
  expectTruncationRejected(SlotRange{4, 2}, sameSlotRange, "SlotRange");
  expectTruncationRejected(FrequencyRange{5, 9}, sameFrequencyRange, "FrequencyRange");
  ControllerFence fence;
  fence.epoch = ControllerEpoch(1);
  fence.incarnation = ControllerIncarnation(2);
  expectTruncationRejected(fence, sameFence, "ControllerFence");
  AuthorityState authority;
  authority.fence = fence;
  authority.eligibilityGeneration = EligibilityAuthorityGeneration(1);
  authority.reservationGeneration = ReservationAuthorityGeneration(1);
  authority.activationGeneration = ActivationAuthorityGeneration(1);
  authority.releaseGeneration = ReleaseAuthorityGeneration(1);
  expectTruncationRejected(authority, sameAuthorityState, "AuthorityState");
  expectTruncationRejected(Status::failure(StatusCode::Conflict, "conflict"), sameStatus, "Status");
  EligibilityAuthority eligibility;
  eligibility.generation = EligibilityAuthorityGeneration(1);
  eligibility.fence = fence;
  expectTruncationRejected(eligibility, sameAuthorityToken<EligibilityAuthority>,
                           "EligibilityAuthority");
  ReservationAuthority reservation;
  reservation.generation = ReservationAuthorityGeneration(1);
  reservation.fence = fence;
  expectTruncationRejected(reservation, sameAuthorityToken<ReservationAuthority>,
                           "ReservationAuthority");
  ActivationAuthority activation;
  activation.generation = ActivationAuthorityGeneration(1);
  activation.fence = fence;
  expectTruncationRejected(activation, sameAuthorityToken<ActivationAuthority>,
                           "ActivationAuthority");
  ReleaseAuthority release;
  release.generation = ReleaseAuthorityGeneration(1);
  release.fence = fence;
  expectTruncationRejected(release, sameAuthorityToken<ReleaseAuthority>, "ReleaseAuthority");

  expectTruncationRejected(wf_test::flexGrid(), sameGrid, "ChannelGrid");
  Span span;
  span.id = SpanId(1);
  span.generation = SpanGeneration(1);
  span.label = "span";
  expectTruncationRejected(span, sameSpan, "Span");
  OpticalPort port;
  port.id = PortId(1);
  port.generation = PortGeneration(1);
  port.label = "port";
  expectTruncationRejected(port, samePort, "OpticalPort");
  expectTruncationRejected(wf_test::makeDomain(SpectrumDomainId(1), ChannelGridId(1)), sameDomain,
                           "SpectrumDomain");
  ExclusionDomain exclusion;
  exclusion.id = ExclusionDomainId(1);
  exclusion.generation = ExclusionDomainGeneration(1);
  exclusion.members = {SpectrumDomainId(1)};
  expectTruncationRejected(exclusion, sameExclusionDomain, "ExclusionDomain");
  CapabilityEvidence evidence;
  evidence.present = true;
  evidence.digest = 9;
  evidence.source = "e";
  expectTruncationRejected(evidence, sameEvidence, "CapabilityEvidence");
  expectTruncationRejected(wf_test::makeCapability(
                               SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1),
                               GridGeneration(1), SpectrumSupport::Supported, 0, 96, fence),
                           sameCapability, "SpectrumCapability");

  expectTruncationRejected(sampleRequest(), sameRequest, "SpectrumRequest");
  SpectrumCandidate candidate;
  candidate.ordinal = 1;
  candidate.detail = "c";
  expectTruncationRejected(candidate, sameCandidate, "SpectrumCandidate");
  CandidateSet set;
  set.status = okStatus();
  set.candidates = {candidate};
  set.eligibleCount = 1;
  set.summary = "s";
  expectTruncationRejected(set, sameCandidateSet, "CandidateSet");
  DecisionExplanation explanation;
  explanation.reasons = {"r"};
  expectTruncationRejected(explanation, sameExplanation, "DecisionExplanation");
  AllocationDecision decision;
  decision.status = okStatus();
  decision.outcome = AllocationOutcome::Allocated;
  decision.explanation = explanation;
  expectTruncationRejected(decision, sameDecision, "AllocationDecision");
  ReclaimReport reclaim;
  reclaim.status = okStatus();
  reclaim.evaluatedAt = Instant::fromSeconds(1);
  reclaim.reclaimed = {ReservationId(1)};
  expectTruncationRejected(reclaim, sameReclaimReport, "ReclaimReport");
  RecoveryReport recovery;
  recovery.status = okStatus();
  recovery.source = "s";
  recovery.diagnostics = {"d"};
  expectTruncationRejected(recovery, sameRecoveryReport, "RecoveryReport");

  Lease lease;
  lease.expiresAt = Instant::fromSeconds(1);
  expectTruncationRejected(lease, sameLease, "Lease");
  expectTruncationRejected(sampleReservation(), sameReservation, "SpectrumReservation");
  SpectrumUsage usage;
  usage.domain = SpectrumDomainId(1);
  usage.freeRuns = {SlotRange{0, 1}};
  expectTruncationRejected(usage, sameUsage, "SpectrumUsage");
  AuditRecord record;
  record.sequence = AuditSequence(1);
  record.detail = "d";
  expectTruncationRejected(record, sameAudit, "AuditRecord");
}

// ---------------------------------------------------------------------------
// Encoder bounds
// ---------------------------------------------------------------------------

WF_TEST(encoder_refuses_an_over_long_string_without_writing_it) {
  Encoder out;
  out.u32(0x1122'3344u);
  out.string(std::string(kMaxEncodedStringBytes + 1u, 'x'));
  WF_CHECK(out.overflowed());
  WF_CHECK_EQ(out.size(), std::size_t(4));

  out.u8(0x55u);
  WF_CHECK_EQ(out.size(), std::size_t(4));
  out.u64(1);
  WF_CHECK_EQ(out.size(), std::size_t(4));
  out.string("short");
  WF_CHECK_EQ(out.size(), std::size_t(4));

  const std::vector<std::uint8_t>& bytes = out.buffer();
  WF_CHECK_EQ(bytes.size(), std::size_t(4));
  WF_CHECK_EQ(bytes[0], std::uint8_t(0x44u));
  WF_CHECK_EQ(bytes[3], std::uint8_t(0x11u));

  Encoder atBound;
  atBound.string(std::string(kMaxEncodedStringBytes, 'y'));
  WF_CHECK(!atBound.overflowed());
  WF_CHECK_EQ(atBound.size(), std::size_t(4) + std::size_t(kMaxEncodedStringBytes));
}

WF_TEST(encoder_refuses_an_over_long_byte_blob_without_writing_it) {
  const std::size_t oversized = std::size_t(16u) * 1024u * 1024u + 1u;
  const std::vector<std::uint8_t> blob(oversized, 0x5Au);
  Encoder out;
  out.u8(0x01u);
  out.bytes(blob);
  WF_CHECK(out.overflowed());
  WF_CHECK_EQ(out.size(), std::size_t(1));

  const std::vector<std::uint8_t> allowed(std::size_t(16u) * 1024u * 1024u, 0x5Au);
  Encoder exact;
  exact.bytes(allowed);
  WF_CHECK(!exact.overflowed());
  WF_CHECK_EQ(exact.size(), std::size_t(4) + std::size_t(16u) * 1024u * 1024u);
}

WF_TEST(encoder_never_exceeds_its_total_buffer_bound) {
  const std::size_t chunk = std::size_t(16u) * 1024u * 1024u;
  const std::vector<std::uint8_t> block(chunk, 0xA5u);
  Encoder out;
  std::size_t accepted = 0;
  while (!out.overflowed() && accepted < 8) {
    out.bytes(block);
    ++accepted;
  }
  WF_CHECK(out.overflowed());
  WF_CHECK_EQ(accepted, std::size_t(4));
  WF_CHECK(out.size() <= std::size_t(64u) * 1024u * 1024u);
  WF_CHECK(out.size() >= 3u * chunk);

  const std::size_t frozen = out.size();
  out.bytes(block);
  WF_CHECK_EQ(out.size(), frozen);
  out.u8(0xFFu);
  WF_CHECK_EQ(out.size(), frozen);
  WF_CHECK(out.overflowed());
}

WF_TEST_MAIN()
