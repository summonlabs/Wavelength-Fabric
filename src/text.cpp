#include "wavelength_fabric/text.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// Stable single-line renderings for every value the runtime exposes as text.
//
// Each function here is pure: it reads its argument, builds exactly one result
// string and returns it. Rendering never consults the clock, never reads or
// writes shared state and never mutates its argument, so an audit record, a CLI
// line and a test diagnostic built from the same value are byte-identical.
//
// Conventions:
//   * one line of printable ASCII, fields separated by a single space;
//   * caller-supplied text is sanitised and bounded before it is echoed;
//   * derived quantities use the checked helpers from quantity.hpp and are
//     rendered as '?' when they are not representable;
//   * typed states and outcomes are rendered through their toToken overload,
//     so a refusal is never spelled as success.

namespace wavelength_fabric {
namespace {

constexpr std::int64_t kNanosPerMicrosecond = 1'000ll;
constexpr std::int64_t kNanosPerMillisecond = 1'000'000ll;
constexpr std::int64_t kNanosPerSecond = 1'000'000'000ll;

// Most bytes of caller-supplied text echoed into one rendering.
constexpr std::size_t kMaxTextBytes = 128;
// Most identities or ranges echoed into one rendering.
constexpr std::size_t kMaxListedIdentities = 8;
// Most spanned domains echoed by describeRequest. A request that passed
// validation never exceeds kMaxRequestDomains, so a valid request is always
// rendered in full.
constexpr std::size_t kMaxListedDomains = 64;

// Writes the exclusive end of a slot range. The end is derived rather than read
// from the struct because a wrapped end index names a different range.
[[nodiscard]] bool slotRangeEnd(const SlotRange& range, std::int64_t& outEnd) noexcept {
  if (addOverflow(static_cast<std::int64_t>(range.first),
                  static_cast<std::int64_t>(range.count), outEnd)) {
    return false;
  }
  return outEnd <= 0xFFFF'FFFFll;
}

void appendUnsigned(std::string& out, std::uint64_t value) {
  out.append(std::to_string(value));
}

void appendSigned(std::string& out, std::int64_t value) {
  out.append(std::to_string(value));
}

// "[first, end)", with '?' for a range whose end is not representable.
void appendSlotBounds(std::string& out, const SlotRange& range) {
  out.push_back('[');
  appendUnsigned(out, range.first);
  out.push_back(',');
  std::int64_t end = 0;
  if (slotRangeEnd(range, end)) {
    appendSigned(out, end);
  } else {
    out.push_back('?');
  }
  out.push_back(')');
}

void appendSlotRangeDetail(std::string& out, const SlotRange& range) {
  appendSlotBounds(out, range);
  out += " count=";
  appendUnsigned(out, range.count);
}

void appendFrequencyBounds(std::string& out, const FrequencyRange& range) {
  out.push_back('[');
  appendSigned(out, range.lowMhz);
  out.push_back(',');
  appendSigned(out, range.highMhz);
  out.push_back(')');
}

void appendFrequencyDetail(std::string& out, const FrequencyRange& range) {
  appendFrequencyBounds(out, range);
  out += " width=";
  std::int64_t width = 0;
  if (subOverflow(range.highMhz, range.lowMhz, width)) {
    out.push_back('?');
  } else {
    appendSigned(out, width);
  }
}

// Echoes caller text as printable ASCII on one line. Bytes outside 0x20..0x7E
// become '.', and the result is truncated on a byte bound so a hostile label
// cannot inflate an audit record without limit.
void appendText(std::string& out, std::string_view text) {
  if (text.empty()) {
    out.push_back('-');
    return;
  }
  const std::size_t limit = text.size() < kMaxTextBytes ? text.size() : kMaxTextBytes;
  for (std::size_t index = 0; index < limit; ++index) {
    const char raw = text[index];
    const unsigned char byte = static_cast<unsigned char>(raw);
    out.push_back(byte >= 0x20u && byte <= 0x7Eu ? raw : '.');
  }
  if (text.size() > limit) out += "...";
}

void appendIdentityList(std::string& out, const std::vector<ReservationId>& ids) {
  out.push_back('[');
  const std::size_t listed =
      ids.size() < kMaxListedIdentities ? ids.size() : kMaxListedIdentities;
  for (std::size_t index = 0; index < listed; ++index) {
    if (index != 0) out.push_back(',');
    appendUnsigned(out, ids[index].raw());
  }
  if (ids.size() > listed) {
    out += ",+";
    appendUnsigned(out, ids.size() - listed);
    out += " more";
  }
  out.push_back(']');
}

void appendSlotRangeList(std::string& out, const std::vector<SlotRange>& ranges) {
  out.push_back('[');
  const std::size_t listed =
      ranges.size() < kMaxListedIdentities ? ranges.size() : kMaxListedIdentities;
  for (std::size_t index = 0; index < listed; ++index) {
    if (index != 0) out.push_back(',');
    appendSlotRangeDetail(out, ranges[index]);
  }
  if (ranges.size() > listed) {
    out += ",+";
    appendUnsigned(out, ranges.size() - listed);
    out += " more";
  }
  out.push_back(']');
}

// Checked addition for externally derived sizes.
[[nodiscard]] bool addSize(std::size_t left, std::size_t right, std::size_t& out) noexcept {
  const std::size_t maximum = std::numeric_limits<std::size_t>::max();
  if (left > maximum - right) return false;
  out = left + right;
  return true;
}

// Renders caller text that carries a digest-bearing evidence claim.
void appendEvidenceDigest(std::string& out, const CapabilityEvidence& evidence) {
  if (evidence.usable()) {
    appendUnsigned(out, evidence.digest);
  } else {
    out.push_back('-');
  }
}

}  // namespace

std::string renderSlotRange(const SlotRange& range) {
  std::string out = "slots=";
  appendSlotRangeDetail(out, range);
  return out;
}

std::string renderFrequencyRange(const FrequencyRange& range) {
  std::string out = "mhz=";
  appendFrequencyDetail(out, range);
  return out;
}

std::string renderDuration(Duration duration) {
  const std::int64_t nanos = duration.nanos();
  std::string out = std::to_string(nanos);
  out += "ns";
  const std::uint64_t magnitude = nanos < 0
                                      ? std::uint64_t{0} - static_cast<std::uint64_t>(nanos)
                                      : static_cast<std::uint64_t>(nanos);
  if (magnitude == 0) return out;
  if (magnitude % static_cast<std::uint64_t>(kNanosPerSecond) == 0) {
    out += " (";
    appendSigned(out, nanos / kNanosPerSecond);
    out += "s)";
  } else if (magnitude % static_cast<std::uint64_t>(kNanosPerMillisecond) == 0) {
    out += " (";
    appendSigned(out, nanos / kNanosPerMillisecond);
    out += "ms)";
  } else if (magnitude % static_cast<std::uint64_t>(kNanosPerMicrosecond) == 0) {
    out += " (";
    appendSigned(out, nanos / kNanosPerMicrosecond);
    out += "us)";
  }
  return out;
}

std::string renderInstant(Instant instant) {
  const std::int64_t nanos = instant.nanos();
  std::string out = std::to_string(nanos);
  out += "ns";

  const std::chrono::seconds seconds(floorDiv(nanos, kNanosPerSecond));
  const std::int64_t elapsed = seconds.count();
  if (elapsed < static_cast<std::int64_t>(std::numeric_limits<std::time_t>::min()) ||
      elapsed > static_cast<std::int64_t>(std::numeric_limits<std::time_t>::max())) {
    return out;
  }

  const std::time_t stamp = static_cast<std::time_t>(elapsed);
  std::tm broken{};
#if defined(_WIN32)
  if (gmtime_s(&broken, &stamp) != 0) return out;
#else
  if (gmtime_r(&stamp, &broken) == nullptr) return out;
#endif
  char buffer[40] = {};
  const std::size_t written =
      std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &broken);
  if (written == 0) return out;
  out += " (";
  out.append(buffer, written);
  out.push_back(')');
  return out;
}

std::string renderFence(const ControllerFence& fence) {
  std::string out = "fence(epoch=";
  appendUnsigned(out, fence.epoch.raw());
  out += ",incarnation=";
  appendUnsigned(out, fence.incarnation.raw());
  out += ",valid=";
  out += (fence.valid() ? "true" : "false");
  out.push_back(')');
  return out;
}

std::string describeGrid(const ChannelGrid& grid) {
  std::string out = "grid id=";
  appendUnsigned(out, grid.id.raw());
  out += " generation=";
  appendUnsigned(out, grid.generation.raw());
  out += " kind=";
  out.append(toToken(grid.kind));
  out += " anchorMhz=";
  appendSigned(out, grid.anchorMhz);
  out += " slotWidthMhz=";
  appendSigned(out, grid.slotWidthMhz);
  out += " slotCount=";
  appendUnsigned(out, grid.slotCount);
  out += " endMhz=";
  std::int64_t span = 0;
  std::int64_t endMhz = 0;
  const std::int64_t slotCount = static_cast<std::int64_t>(grid.slotCount);
  if (mulOverflow(slotCount, grid.slotWidthMhz, span) ||
      addOverflow(grid.anchorMhz, span, endMhz)) {
    out.push_back('?');
  } else {
    appendSigned(out, endMhz);
  }
  out += " minSlotsPerChannel=";
  appendUnsigned(out, grid.minSlotsPerChannel);
  out += " maxSlotsPerChannel=";
  appendUnsigned(out, grid.maxSlotsPerChannel);
  out += " label=";
  appendText(out, grid.label);
  return out;
}

std::string describeSlotRange(const ChannelGrid& grid, SlotRange range) {
  std::string out = "slotRange grid=";
  appendUnsigned(out, grid.id.raw());
  out += " generation=";
  appendUnsigned(out, grid.generation.raw());
  out += " slotCount=";
  appendUnsigned(out, grid.slotCount);
  out += " slots=";
  appendSlotRangeDetail(out, range);
  FrequencyRange frequency{};
  if (frequencyOfSlotRange(grid, range, frequency)) {
    out += " mhz=";
    appendFrequencyDetail(out, frequency);
  } else {
    out += " mhz=unrepresentable";
  }
  return out;
}

std::string describeCapability(const SpectrumCapability& capability) {
  std::string out = "capability domain=";
  appendUnsigned(out, capability.domain.raw());
  out += " domainGeneration=";
  appendUnsigned(out, capability.domainGeneration.raw());
  out += " grid=";
  appendUnsigned(out, capability.grid.raw());
  out += " gridGeneration=";
  appendUnsigned(out, capability.gridGeneration.raw());
  out += " support=";
  out.append(toToken(capability.support));
  out += " window=";
  appendSlotRangeDetail(out, allocatableWindow(capability));
  out += " tunable=";
  appendFrequencyDetail(out, FrequencyRange{capability.minTunableMhz, capability.maxTunableMhz});
  out += " contiguityEnforced=";
  out += (capability.contiguityEnforced ? "true" : "false");
  out += " conversionSupported=";
  out += (capability.conversionSupported ? "true" : "false");
  out += " presenceEvidence=";
  appendEvidenceDigest(out, capability.presenceEvidence);
  out += " conversionEvidence=";
  appendEvidenceDigest(out, capability.conversionEvidence);
  out += " capabilityGeneration=";
  appendUnsigned(out, capability.generation.raw());
  out += " publisher=";
  appendUnsigned(out, capability.publisher.raw());
  out.push_back(' ');
  out += renderFence(capability.fence);
  out += " publishedAt=";
  out += renderInstant(capability.publishedAt);
  out += " detail=";
  appendText(out, capability.detail);
  return out;
}

std::string describeRequest(const SpectrumRequest& request) {
  std::string out = "request id=";
  appendUnsigned(out, request.requestId.raw());
  out += " generation=";
  appendUnsigned(out, request.requestGeneration.raw());
  out += " owner=";
  appendUnsigned(out, request.owner.raw());
  out += " ownerGeneration=";
  appendUnsigned(out, request.ownerGeneration.raw());

  out += " domains=[";
  const std::size_t domainCount = request.domains.size();
  const std::size_t listedDomains =
      domainCount < kMaxListedDomains ? domainCount : kMaxListedDomains;
  for (std::size_t index = 0; index < listedDomains; ++index) {
    if (index != 0) out.push_back(',');
    appendUnsigned(out, request.domains[index].raw());
    out.push_back('@');
    if (index < request.domainGenerations.size()) {
      appendUnsigned(out, request.domainGenerations[index].raw());
    } else {
      out.push_back('?');
    }
  }
  if (domainCount > listedDomains) {
    out += ",+";
    appendUnsigned(out, domainCount - listedDomains);
    out += " more";
  }
  out.push_back(']');

  out += " grid=";
  appendUnsigned(out, request.grid.raw());
  out += " gridGeneration=";
  appendUnsigned(out, request.gridGeneration.raw());
  out += " slots=";
  appendUnsigned(out, request.slots);
  out += " contiguity=";
  out.append(toToken(request.contiguity));
  out += " continuity=";
  out.append(toToken(request.continuity));
  out += " guardBandMhz=";
  appendSigned(out, request.guardBandMhz);
  out += " lease=";
  out += renderDuration(request.leaseDuration);
  out += " maxRenewals=";
  appendUnsigned(out, request.maxRenewals);
  out += " frequencyWindows=";
  appendUnsigned(out, request.frequencyWindows.size());

  std::size_t exclusionConstraints = 0;
  const bool exclusionSumOk =
      addSize(request.constraints.excludedSlots.size(),
              request.constraints.excludedFrequencies.size(), exclusionConstraints) &&
      addSize(exclusionConstraints, request.constraints.mustNotConflictWith.size(),
              exclusionConstraints);
  out += " exclusionConstraints=";
  if (exclusionSumOk) {
    appendUnsigned(out, exclusionConstraints);
  } else {
    out.push_back('?');
  }
  out += " excludedSlots=";
  appendUnsigned(out, request.constraints.excludedSlots.size());
  out += " excludedFrequencies=";
  appendUnsigned(out, request.constraints.excludedFrequencies.size());
  out += " mustNotConflictWith=";
  appendUnsigned(out, request.constraints.mustNotConflictWith.size());
  return out;
}

std::string describeRuntime(const RuntimeStats& stats) {
  std::string out = "runtime allocationsCommitted=";
  appendUnsigned(out, stats.allocationsCommitted);
  out += " allocationsRefused=";
  appendUnsigned(out, stats.allocationsRefused);
  out += " candidatesEvaluated=";
  appendUnsigned(out, stats.candidatesEvaluated);
  out += " enumerationTruncations=";
  appendUnsigned(out, stats.enumerationTruncations);
  out += " renewals=";
  appendUnsigned(out, stats.renewals);
  out += " releases=";
  appendUnsigned(out, stats.releases);
  out += " activations=";
  appendUnsigned(out, stats.activations);
  out += " deactivations=";
  appendUnsigned(out, stats.deactivations);
  out += " expirationSweeps=";
  appendUnsigned(out, stats.expirationSweeps);
  out += " reclamations=";
  appendUnsigned(out, stats.reclamations);
  out += " persistenceWrites=";
  appendUnsigned(out, stats.persistenceWrites);
  out += " persistenceFailures=";
  appendUnsigned(out, stats.persistenceFailures);
  out += " replayRejections=";
  appendUnsigned(out, stats.replayRejections);
  out += " corruptionDetections=";
  appendUnsigned(out, stats.corruptionDetections);
  return out;
}

std::string SpectrumCandidate::describe() const {
  std::string out = "candidate ordinal=";
  appendUnsigned(out, ordinal);
  out += " domain=";
  appendUnsigned(out, anchorDomain.raw());
  out += " eligibility=";
  out.append(toToken(eligibility));
  out += " slots=";
  appendSlotRangeDetail(out, slots);
  out += " mhz=";
  appendFrequencyDetail(out, frequency);
  out += " crossGrid=";
  out += (crossGrid ? "true" : "false");
  out += " conflicts=";
  appendIdentityList(out, conflicts);
  out += " perDomainSlots=";
  appendSlotRangeList(out, perDomainSlots);
  out += " detail=";
  appendText(out, detail);
  return out;
}

std::string DecisionExplanation::summary() const {
  std::string out = "outcome=";
  out.append(toToken(outcome));
  out += " reservation=";
  appendUnsigned(out, reservation.raw());
  out += " generation=";
  appendUnsigned(out, generation.raw());
  out.push_back(' ');
  out += renderFence(fence);
  out += " candidatesEnumerated=";
  appendUnsigned(out, candidatesEnumerated);
  out += " candidatesRejected=";
  appendUnsigned(out, candidatesRejected);
  out += " candidatesOmitted=";
  appendUnsigned(out, candidatesOmitted);
  out += " candidateOrdinal=";
  appendUnsigned(out, candidateOrdinal);
  out += " rejectedExplained=";
  appendUnsigned(out, rejected.size());
  out += " conflicts=";
  appendIdentityList(out, conflicts);
  out += " reasons=";
  appendUnsigned(out, reasons.size());
  out += " firstReason=";
  if (reasons.empty()) {
    out.push_back('-');
  } else {
    appendText(out, reasons.front());
  }
  out += " selectedOrdinal=";
  if (outcome == AllocationOutcome::Allocated) {
    appendUnsigned(out, selected.ordinal);
  } else {
    out.push_back('-');
  }
  return out;
}

std::string AllocationDecision::describe() const {
  std::string out = "decision allocated=";
  out += (allocated() ? "true" : "false");
  out += " status=";
  out.append(toToken(status.code));
  out += " statusMessage=";
  appendText(out, status.message);
  out += " outcome=";
  out.append(toToken(outcome));
  out += " reservation=";
  appendUnsigned(out, reservation.raw());
  out += " generation=";
  appendUnsigned(out, generation.raw());
  out.push_back(' ');
  out += explanation.summary();
  return out;
}

std::string ReclaimReport::describe() const {
  std::string out = "reclaim status=";
  out.append(toToken(status.code));
  out += " statusMessage=";
  appendText(out, status.message);
  out += " evaluatedAt=";
  out += renderInstant(evaluatedAt);
  out.push_back(' ');
  out += renderFence(fence);
  out += " scanned=";
  appendUnsigned(out, scanned);
  out += " expired=";
  appendUnsigned(out, expired);
  out += " reclaimed=";
  appendUnsigned(out, reclaimed.size());
  out.push_back(':');
  appendIdentityList(out, reclaimed);
  out += " lapsed=";
  appendUnsigned(out, lapsed.size());
  out.push_back(':');
  appendIdentityList(out, lapsed);
  return out;
}

std::string RecoveryReport::describe() const {
  std::string out = "recovery status=";
  out.append(toToken(status.code));
  out += " statusMessage=";
  appendText(out, status.message);
  out += " recovered=";
  out += (recovered ? "true" : "false");
  out += " source=";
  appendText(out, source);
  out.push_back(' ');
  out += renderFence(fence);
  out += " runtimeGeneration=";
  appendUnsigned(out, runtimeGeneration.raw());
  out += " recoveryGeneration=";
  appendUnsigned(out, recoveryGeneration.raw());
  out += " recordsRead=";
  appendUnsigned(out, recordsRead);
  out += " recordsAccepted=";
  appendUnsigned(out, recordsAccepted);
  out += " recordsRejected=";
  appendUnsigned(out, recordsRejected);
  out += " gridsRestored=";
  appendUnsigned(out, gridsRestored);
  out += " domainsRestored=";
  appendUnsigned(out, domainsRestored);
  out += " exclusionDomainsRestored=";
  appendUnsigned(out, exclusionDomainsRestored);
  out += " capabilitiesRestored=";
  appendUnsigned(out, capabilitiesRestored);
  out += " reservationsRestored=";
  appendUnsigned(out, reservationsRestored);
  out += " auditsRestored=";
  appendUnsigned(out, auditsRestored);
  out += " demotedFromActive=";
  appendUnsigned(out, demotedFromActive);
  out += " diagnostics=";
  appendUnsigned(out, diagnostics.size());
  out += " firstDiagnostic=";
  if (diagnostics.empty()) {
    out.push_back('-');
  } else {
    appendText(out, diagnostics.front());
  }
  return out;
}

std::string AuditRecord::describe() const {
  std::string out = "audit sequence=";
  appendUnsigned(out, sequence.raw());
  out += " at=";
  out += renderInstant(at);
  out += " kind=";
  out.append(toToken(kind));
  out.push_back(' ');
  out += renderFence(fence);
  out += " outcome=";
  out.append(toToken(outcome));
  out += " reservation=";
  appendUnsigned(out, reservation.raw());
  out += " reservationGeneration=";
  appendUnsigned(out, reservationGeneration.raw());
  out += " domain=";
  appendUnsigned(out, domain.raw());
  out += " slots=";
  appendSlotRangeDetail(out, slots);
  out += " mhz=";
  appendFrequencyDetail(out, frequency);
  out += " detail=";
  appendText(out, detail);
  return out;
}

}  // namespace wavelength_fabric
