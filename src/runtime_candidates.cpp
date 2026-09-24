#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "impl.hpp"

namespace wavelength_fabric {
namespace detail {
namespace {

// Upper bound on the number of distinct blocking intervals considered for one
// request. Exceeding it is reported as a limit refusal, never as "no capacity".
constexpr std::size_t kMaxBlockedIntervals = 1u << 18;

// Caps the number of diagnostic candidates produced for a refusal.
constexpr std::size_t kMaxDiagnosticCandidates = 8;

struct DomainContext {
  SpectrumDomainId id{};
  SpectrumDomainGeneration generation{};
  const ChannelGrid* grid{nullptr};
  const SpectrumCapability* capability{nullptr};
  bool conversionRequired{false};
  bool requiresContiguity{true};
};

struct Blocked {
  std::int64_t lowMhz{0};
  std::int64_t highMhz{0};
  ReservationId reservation{};
  ExclusionDomainId exclusion{};
};

struct Preparation {
  Status status;
  AllocationOutcome outcome{AllocationOutcome::Unknown};
  std::string reason;
  std::vector<DomainContext> contexts;
  bool crossGrid{false};
};

[[nodiscard]] SlotRange intersectRange(SlotRange a, SlotRange b) noexcept {
  const std::uint32_t first = a.first > b.first ? a.first : b.first;
  const std::uint32_t aEnd = a.end();
  const std::uint32_t bEnd = b.end();
  const std::uint32_t last = aEnd < bEnd ? aEnd : bEnd;
  if (last <= first) return SlotRange{};
  return SlotRange{first, last - first};
}

// Slot range fully inside a frequency range, in the given grid.
[[nodiscard]] bool innerSlotRange(const ChannelGrid& grid, FrequencyRange range, SlotRange& out) {
  if (range.empty() || grid.slotWidthMhz <= 0) return false;
  std::int64_t lo = 0;
  std::int64_t hi = 0;
  if (subOverflow(range.lowMhz, grid.anchorMhz, lo)) return false;
  if (subOverflow(range.highMhz, grid.anchorMhz, hi)) return false;
  std::int64_t first = ceilDiv(lo, grid.slotWidthMhz);
  std::int64_t last = floorDiv(hi, grid.slotWidthMhz);
  if (first < 0) first = 0;
  if (last < 0) last = 0;
  if (first > static_cast<std::int64_t>(grid.slotCount)) first = grid.slotCount;
  if (last > static_cast<std::int64_t>(grid.slotCount)) last = grid.slotCount;
  if (last <= first) return false;
  out.first = static_cast<std::uint32_t>(first);
  out.count = static_cast<std::uint32_t>(last - first);
  return true;
}

// Slot range that covers every slot touching a frequency range, in the given grid.
[[nodiscard]] bool outerSlotRange(const ChannelGrid& grid, FrequencyRange range, SlotRange& out) {
  if (range.empty() || grid.slotWidthMhz <= 0) return false;
  std::int64_t lo = 0;
  std::int64_t hi = 0;
  if (subOverflow(range.lowMhz, grid.anchorMhz, lo)) return false;
  if (subOverflow(range.highMhz, grid.anchorMhz, hi)) return false;
  std::int64_t first = floorDiv(lo, grid.slotWidthMhz);
  std::int64_t last = ceilDiv(hi, grid.slotWidthMhz);
  if (first < 0) first = 0;
  if (last < 0) last = 0;
  if (first > static_cast<std::int64_t>(grid.slotCount)) first = grid.slotCount;
  if (last > static_cast<std::int64_t>(grid.slotCount)) last = grid.slotCount;
  if (last <= first) return false;
  out.first = static_cast<std::uint32_t>(first);
  out.count = static_cast<std::uint32_t>(last - first);
  return true;
}

void mergeRanges(std::vector<SlotRange>& ranges) {
  if (ranges.size() < 2) return;
  std::sort(ranges.begin(), ranges.end(), [](const SlotRange& a, const SlotRange& b) {
    if (a.first != b.first) return a.first < b.first;
    return a.count < b.count;
  });
  std::vector<SlotRange> merged;
  merged.reserve(ranges.size());
  for (const SlotRange& range : ranges) {
    if (range.empty()) continue;
    if (!merged.empty() && range.first <= merged.back().end()) {
      const std::uint32_t end = merged.back().end() > range.end() ? merged.back().end() : range.end();
      merged.back().count = end - merged.back().first;
    } else {
      merged.push_back(range);
    }
  }
  ranges.swap(merged);
}

// base minus cut, appended to out in ascending order.
void subtractRange(std::vector<SlotRange>& out, SlotRange base, SlotRange cut) {
  if (base.empty()) return;
  if (cut.empty() || !base.overlaps(cut)) {
    out.push_back(base);
    return;
  }
  if (cut.first > base.first) {
    out.push_back(SlotRange{base.first, cut.first - base.first});
  }
  const std::uint32_t cutEnd = cut.end();
  if (cutEnd < base.end()) {
    out.push_back(SlotRange{cutEnd, base.end() - cutEnd});
  }
}

[[nodiscard]] bool containsFrequency(const std::vector<FrequencyRange>& windows,
                                     FrequencyRange value) {
  if (windows.empty()) return true;
  for (const FrequencyRange& window : windows) {
    if (window.contains(value)) return true;
  }
  return false;
}

[[nodiscard]] Preparation prepare(const RuntimeState& state, const SpectrumRequest& request) {
  Preparation prep;

  const auto gridIt = state.grids.find(request.grid);
  if (gridIt == state.grids.end()) {
    prep.status = fail(StatusCode::NotFound,
                       "grid " + typedToken("grid", request.grid) + " is not registered");
    prep.outcome = AllocationOutcome::RefusedInvalidRequest;
    prep.reason = prep.status.message;
    return prep;
  }
  if (gridIt->second.generation != request.gridGeneration) {
    prep.status = fail(StatusCode::StaleGeneration,
                       "grid " + typedToken("grid", request.grid) + " is registered at generation " +
                           std::to_string(gridIt->second.generation.raw()) +
                           " but the request names generation " +
                           std::to_string(request.gridGeneration.raw()));
    prep.outcome = AllocationOutcome::RefusedStaleGeneration;
    prep.reason = prep.status.message;
    return prep;
  }

  // A request whose channel width the grid cannot express is refused before any
  // candidate is considered: a fixed grid carries exactly one slot per channel,
  // and a flex grid is bounded by its per-channel slot limits.
  if (!channelWidthSupported(gridIt->second, request.slots)) {
    prep.status = fail(StatusCode::InvalidArgument,
                       "grid " + typedToken("grid", request.grid) + " does not support a channel of " +
                           std::to_string(request.slots) + " slot(s)");
    prep.outcome = AllocationOutcome::RefusedChannelWidth;
    prep.reason = prep.status.message;
    return prep;
  }

  prep.contexts.reserve(request.domains.size());
  for (std::size_t index = 0; index < request.domains.size(); ++index) {
    const SpectrumDomainId id = request.domains[index];
    const auto domainIt = state.domains.find(id);
    if (domainIt == state.domains.end()) {
      prep.status = fail(StatusCode::NotFound, "domain " + typedToken("domain", id) +
                                                   " is not registered");
      prep.outcome = AllocationOutcome::RefusedUnknownDomain;
      prep.reason = prep.status.message;
      return prep;
    }
    const SpectrumDomain& domain = domainIt->second;
    if (domain.generation != request.domainGenerations[index]) {
      prep.status = fail(StatusCode::StaleGeneration,
                         "domain " + typedToken("domain", id) + " is registered at generation " +
                             std::to_string(domain.generation.raw()) +
                             " but the request names generation " +
                             std::to_string(request.domainGenerations[index].raw()));
      prep.outcome = AllocationOutcome::RefusedStaleGeneration;
      prep.reason = prep.status.message;
      return prep;
    }
    const auto capabilityIt = state.capabilities.find(id);
    if (capabilityIt == state.capabilities.end()) {
      prep.status = fail(StatusCode::NotFound, "domain " + typedToken("domain", id) +
                                                   " has no registered spectrum capability");
      prep.outcome = AllocationOutcome::RefusedUnknownCapability;
      prep.reason = prep.status.message;
      return prep;
    }
    const SpectrumCapability& capability = capabilityIt->second;
    if (capability.domainGeneration != domain.generation) {
      prep.status = fail(StatusCode::StaleGeneration,
                         "capability for domain " + typedToken("domain", id) + " describes domain "
                         "generation " + std::to_string(capability.domainGeneration.raw()) +
                             " but the domain is at generation " +
                             std::to_string(domain.generation.raw()));
      prep.outcome = AllocationOutcome::RefusedStaleCapability;
      prep.reason = prep.status.message;
      return prep;
    }
    if (capability.support == SpectrumSupport::Unsupported) {
      prep.status = fail(StatusCode::Unsupported,
                         "domain " + typedToken("domain", id) +
                             " is published as UNSUPPORTED for spectrum allocation");
      prep.outcome = AllocationOutcome::RefusedUnsupported;
      prep.reason = prep.status.message;
      return prep;
    }
    if (capability.support == SpectrumSupport::Unknown) {
      prep.status = fail(StatusCode::Unknown,
                         "domain " + typedToken("domain", id) +
                             " has no authoritative spectrum capability (UNKNOWN)");
      prep.outcome = AllocationOutcome::RefusedUnknownCapability;
      prep.reason = prep.status.message;
      return prep;
    }
    const auto domainGridIt = state.grids.find(domain.grid);
    if (domainGridIt == state.grids.end()) {
      prep.status = fail(StatusCode::NotFound, "domain " + typedToken("domain", id) +
                                                   " references an unregistered grid");
      prep.outcome = AllocationOutcome::RefusedUnknownDomain;
      prep.reason = prep.status.message;
      return prep;
    }
    DomainContext context;
    context.id = id;
    context.generation = domain.generation;
    context.grid = &domainGridIt->second;
    context.capability = &capability;
    context.requiresContiguity = domain.requiresContiguity && capability.contiguityEnforced;
    context.conversionRequired = domainGridIt->second.id != request.grid;
    if (context.conversionRequired) prep.crossGrid = true;
    prep.contexts.push_back(context);
  }

  if (prep.crossGrid) {
    for (const DomainContext& context : prep.contexts) {
      if (context.capability->conversionSupported && context.capability->conversionEvidence.usable()) {
        continue;
      }
      prep.status = fail(StatusCode::NotPermitted,
                         "domain " + typedToken("domain", context.id) +
                             " is on grid " + typedToken("grid", context.grid->id) +
                             " but no conversion capability evidence was supplied; conversion is "
                             "never assumed");
      prep.outcome = AllocationOutcome::RefusedConversion;
      prep.reason = prep.status.message;
      return prep;
    }
  }

  for (const ReservationId id : request.constraints.mustNotConflictWith) {
    if (state.reservations.find(id) == state.reservations.end()) {
      prep.status = fail(StatusCode::NotFound,
                         "mustNotConflictWith references unknown reservation " +
                             typedToken("reservation", id));
      prep.outcome = AllocationOutcome::RefusedConstraint;
      prep.reason = prep.status.message;
      return prep;
    }
  }

  prep.status = okStatus();
  return prep;
}

void collectDomainBlocks(const RuntimeState& state, SpectrumDomainId domain, Instant now,
                         std::int64_t requestGuard, std::int64_t extraGuard,
                         ExclusionDomainId exclusion, std::map<ReservationId, Blocked>& blocks) {
  const auto it = state.liveByDomain.find(domain);
  if (it == state.liveByDomain.end()) return;
  for (const ReservationId id : it->second) {
    const auto reservationIt = state.reservations.find(id);
    if (reservationIt == state.reservations.end()) continue;
    const SpectrumReservation& reservation = reservationIt->second;
    if (!isLiveAt(reservation, now)) continue;
    std::int64_t guard = reservation.guardBandMhz > requestGuard ? reservation.guardBandMhz
                                                                : requestGuard;
    if (addOverflow(guard, extraGuard, guard)) guard = kMaxGuardBandMhz * 4;
    const FrequencyRange expanded = expandByGuard(reservation.frequency, guard);
    auto existing = blocks.find(id);
    if (existing == blocks.end()) {
      blocks.emplace(id, Blocked{expanded.lowMhz, expanded.highMhz, id, exclusion});
    } else {
      if (expanded.lowMhz < existing->second.lowMhz) existing->second.lowMhz = expanded.lowMhz;
      if (expanded.highMhz > existing->second.highMhz) existing->second.highMhz = expanded.highMhz;
      if (exclusion.valid()) existing->second.exclusion = exclusion;
    }
  }
}

// True when the candidate frequency range is free on one specific domain,
// considering that domain's own live reservations and its exclusion siblings.
[[nodiscard]] bool crossDomainFree(const RuntimeState& state, SpectrumDomainId domain,
                                   FrequencyRange candidate, std::int64_t requestGuard, Instant now,
                                   std::vector<ReservationId>& conflicts, bool& exclusionHit) {
  bool free = true;
  const auto scan = [&](SpectrumDomainId member, std::int64_t extraGuard,
                        ExclusionDomainId exclusion) {
    const auto it = state.liveByDomain.find(member);
    if (it == state.liveByDomain.end()) return;
    for (const ReservationId id : it->second) {
      const auto reservationIt = state.reservations.find(id);
      if (reservationIt == state.reservations.end()) continue;
      const SpectrumReservation& reservation = reservationIt->second;
      if (!isLiveAt(reservation, now)) continue;
      const std::int64_t guard =
          (reservation.guardBandMhz > requestGuard ? reservation.guardBandMhz : requestGuard) +
          extraGuard;
      if (frequencyConflict(candidate, guard, reservation.frequency, guard)) {
        free = false;
        if (std::find(conflicts.begin(), conflicts.end(), id) == conflicts.end()) {
          conflicts.push_back(id);
        }
        if (exclusion.valid()) exclusionHit = true;
      }
    }
  };

  scan(domain, 0, ExclusionDomainId{});
  const auto exclusions = state.domainExclusions.find(domain);
  if (exclusions != state.domainExclusions.end()) {
    for (const ExclusionDomainId exclusionId : exclusions->second) {
      const auto exclusionIt = state.exclusionDomains.find(exclusionId);
      if (exclusionIt == state.exclusionDomains.end()) continue;
      for (const SpectrumDomainId member : exclusionIt->second.members) {
        if (member == domain) continue;
        scan(member, exclusionIt->second.guardBandMhz, exclusionId);
      }
    }
  }
  std::sort(conflicts.begin(), conflicts.end());
  return free;
}

void classify(SpectrumCandidate& candidate, CandidateEligibility eligibility, std::string detail) {
  if (isEligible(candidate.eligibility)) {
    candidate.eligibility = eligibility;
    candidate.detail = std::move(detail);
  }
}

void evaluateCandidate(const RuntimeState& state, const SpectrumRequest& request,
                       const ChannelGrid& requestGrid,
                       const std::vector<DomainContext>& contexts, std::uint32_t start,
                       SpectrumCandidate& candidate) {
  candidate.slots = SlotRange{start, request.slots};
  if (!slotRangeInGrid(requestGrid, candidate.slots)) {
    classify(candidate, CandidateEligibility::IneligibleOutsideWindow,
             "slot range leaves the request grid");
    return;
  }
  if (!frequencyOfSlotRange(requestGrid, candidate.slots, candidate.frequency)) {
    classify(candidate, CandidateEligibility::IneligibleOutsideWindow,
             "slot range frequency is not representable");
    return;
  }
  if (!containsFrequency(request.frequencyWindows, candidate.frequency)) {
    classify(candidate, CandidateEligibility::IneligibleOutsideWindow,
             "candidate frequency range is outside every permitted frequency window");
    return;
  }
  for (const SlotRange& excluded : request.constraints.excludedSlots) {
    if (excluded.overlaps(candidate.slots)) {
      classify(candidate, CandidateEligibility::IneligibleExcluded,
               "candidate overlaps an excluded slot range");
      return;
    }
  }
  for (const FrequencyRange& excluded : request.constraints.excludedFrequencies) {
    if (excluded.overlaps(candidate.frequency)) {
      classify(candidate, CandidateEligibility::IneligibleExcluded,
               "candidate overlaps an excluded frequency range");
      return;
    }
  }

  candidate.perDomainSlots.assign(contexts.size(), SlotRange{});
  const Instant now = request.requestedAt;

  for (std::size_t index = 0; index < contexts.size(); ++index) {
    const DomainContext& context = contexts[index];
    const SpectrumCapability& capability = *context.capability;
    if (candidate.frequency.lowMhz < capability.minTunableMhz ||
        candidate.frequency.highMhz > capability.maxTunableMhz) {
      classify(candidate, CandidateEligibility::IneligibleOutsideTunable,
               "candidate is outside the tunable range of domain " + typedToken("domain", context.id));
      return;
    }
    if (request.slots > 1 && request.contiguity == ContiguityRequirement::NotRequired &&
        !context.requiresContiguity) {
      classify(candidate, CandidateEligibility::IneligibleContiguity,
               "domain " + typedToken("domain", context.id) +
                   " permits a fragmented channel and the request permits fragmentation, but this "
                   "runtime allocates a single contiguous slot range");
      return;
    }

    if (!context.conversionRequired) {
      candidate.perDomainSlots[index] = candidate.slots;
      continue;
    }

    candidate.crossGrid = true;
    SlotRange mapped;
    if (!innerSlotRange(*context.grid, candidate.frequency, mapped) ||
        !channelWidthSupported(*context.grid, mapped.count)) {
      classify(candidate, CandidateEligibility::IneligibleConversionRequired,
               "domain " + typedToken("domain", context.id) + " on grid " +
                   typedToken("grid", context.grid->id) + " cannot represent the candidate exactly");
      return;
    }
    FrequencyRange mappedFrequency;
    if (!frequencyOfSlotRange(*context.grid, mapped, mappedFrequency) ||
        !(mappedFrequency == candidate.frequency)) {
      classify(candidate, CandidateEligibility::IneligibleConversionRequired,
               "domain " + typedToken("domain", context.id) +
                   " cannot represent the candidate frequency window without a conversion gap");
      return;
    }
    candidate.perDomainSlots[index] = mapped;
  }

  // Every spanned domain is verified against current ownership, not only the
  // anchor domain. Diagnostic candidates are synthesised from blocked windows,
  // so an anchor-only check would let an overlapping reservation commit on a
  // non-anchor domain of the same grid.
  for (std::size_t index = 0; index < contexts.size(); ++index) {
    FrequencyRange domainFrequency = candidate.frequency;
    if (contexts[index].conversionRequired) {
      if (!frequencyOfSlotRange(*contexts[index].grid, candidate.perDomainSlots[index],
                                domainFrequency)) {
        classify(candidate, CandidateEligibility::IneligibleConversionRequired,
                 "domain " + typedToken("domain", contexts[index].id) +
                     " cannot express the mapped placement as a frequency range");
        return;
      }
    }
    std::vector<ReservationId> conflicts;
    bool localExclusion = false;
    if (!crossDomainFree(state, contexts[index].id, domainFrequency, request.guardBandMhz, now,
                         conflicts, localExclusion)) {
      candidate.conflicts = conflicts;
      classify(candidate, localExclusion ? CandidateEligibility::IneligibleExcluded
                                         : CandidateEligibility::IneligibleConflict,
               "candidate conflicts with a live reservation on domain " +
                   typedToken("domain", contexts[index].id));
      return;
    }
  }

  candidate.eligibility = CandidateEligibility::Eligible;
  candidate.detail = "eligible";
}

void noteReason(Evaluation& evaluation, CandidateEligibility eligibility) {
  switch (eligibility) {
    case CandidateEligibility::IneligibleStaleDomain:
      evaluation.refuse(AllocationOutcome::RefusedStaleGeneration,
                        "a spanned domain or its capability became stale");
      break;
    case CandidateEligibility::IneligibleStaleCapability:
      evaluation.refuse(AllocationOutcome::RefusedStaleCapability,
                        "a spanned domain capability generation is stale");
      break;
    case CandidateEligibility::IneligibleUnsupported:
      evaluation.refuse(AllocationOutcome::RefusedUnsupported,
                        "a spanned domain is published as UNSUPPORTED");
      break;
    case CandidateEligibility::IneligibleUnknownCapability:
      evaluation.refuse(AllocationOutcome::RefusedUnknownCapability,
                        "a spanned domain has an UNKNOWN capability");
      break;
    case CandidateEligibility::IneligibleConversionRequired:
      evaluation.refuse(AllocationOutcome::RefusedConversion,
                        "a spanned domain cannot represent the candidate without conversion");
      break;
    case CandidateEligibility::IneligibleChannelWidth:
      evaluation.refuse(AllocationOutcome::RefusedChannelWidth,
                        "the requested channel width is not representable");
      break;
    case CandidateEligibility::IneligibleExcluded:
      evaluation.refuse(AllocationOutcome::RefusedExclusion,
                        "an exclusion domain forbids the candidate");
      break;
    case CandidateEligibility::IneligibleContiguity:
      evaluation.refuse(AllocationOutcome::RefusedContiguity,
                        "the candidate cannot satisfy the stated contiguity requirement");
      break;
    case CandidateEligibility::IneligibleGuardBand:
    case CandidateEligibility::IneligibleOutsideWindow:
    case CandidateEligibility::IneligibleOutsideTunable:
    case CandidateEligibility::IneligibleNoGrid:
    case CandidateEligibility::IneligibleUnknown:
      evaluation.refuse(AllocationOutcome::RefusedConstraint,
                        "the candidate does not satisfy a stated constraint");
      break;
    case CandidateEligibility::IneligibleConflict:
      evaluation.refuse(AllocationOutcome::RefusedConflict,
                        "the candidate is owned by a live reservation");
      break;
    case CandidateEligibility::Eligible:
      break;
  }
}

[[nodiscard]] AllocationOutcome dominantOutcome(const SpectrumCandidate& candidate) {
  switch (candidate.eligibility) {
    case CandidateEligibility::IneligibleStaleDomain:
      return AllocationOutcome::RefusedStaleGeneration;
    case CandidateEligibility::IneligibleStaleCapability:
      return AllocationOutcome::RefusedStaleCapability;
    case CandidateEligibility::IneligibleUnsupported:
      return AllocationOutcome::RefusedUnsupported;
    case CandidateEligibility::IneligibleUnknownCapability:
      return AllocationOutcome::RefusedUnknownCapability;
    case CandidateEligibility::IneligibleConversionRequired:
      return AllocationOutcome::RefusedConversion;
    case CandidateEligibility::IneligibleChannelWidth:
      return AllocationOutcome::RefusedChannelWidth;
    case CandidateEligibility::IneligibleExcluded:
      return candidate.conflicts.empty() ? AllocationOutcome::RefusedConstraint
                                         : AllocationOutcome::RefusedExclusion;
    case CandidateEligibility::IneligibleContiguity:
      return AllocationOutcome::RefusedContiguity;
    case CandidateEligibility::IneligibleGuardBand:
    case CandidateEligibility::IneligibleOutsideWindow:
    case CandidateEligibility::IneligibleOutsideTunable:
    case CandidateEligibility::IneligibleNoGrid:
    case CandidateEligibility::IneligibleUnknown:
      return AllocationOutcome::RefusedConstraint;
    case CandidateEligibility::IneligibleConflict:
      return AllocationOutcome::RefusedConflict;
    case CandidateEligibility::Eligible:
      return AllocationOutcome::Allocated;
  }
  return AllocationOutcome::RefusedNoCapacity;
}

[[nodiscard]] int outcomeRank(AllocationOutcome outcome) {
  switch (outcome) {
    case AllocationOutcome::RefusedStaleGeneration:
      return 100;
    case AllocationOutcome::RefusedStaleCapability:
      return 95;
    case AllocationOutcome::RefusedUnsupported:
      return 90;
    case AllocationOutcome::RefusedUnknownCapability:
      return 85;
    case AllocationOutcome::RefusedConversion:
      return 80;
    case AllocationOutcome::RefusedChannelWidth:
      return 75;
    case AllocationOutcome::RefusedExclusion:
      return 70;
    case AllocationOutcome::RefusedConstraint:
      return 65;
    case AllocationOutcome::RefusedContiguity:
      return 60;
    case AllocationOutcome::RefusedConflict:
      return 55;
    default:
      return 10;
  }
}

}  // namespace

bool sameRequestShape(const SpectrumReservation& reservation, const SpectrumRequest& request) {
  if (reservation.domains != request.domains) return false;
  if (reservation.domainGenerations != request.domainGenerations) return false;
  if (reservation.grid != request.grid) return false;
  if (reservation.gridGeneration != request.gridGeneration) return false;
  if (reservation.owner != request.owner) return false;
  if (reservation.ownerGeneration != request.ownerGeneration) return false;
  if (reservation.slots.count != request.slots) return false;
  if (reservation.guardBandMhz != request.guardBandMhz) return false;
  if (reservation.lease.maxRenewals != request.maxRenewals) return false;
  if (reservation.contiguityRequired != (request.contiguity == ContiguityRequirement::Required)) {
    return false;
  }
  if (reservation.continuityRequired != (request.continuity == ContinuityRequirement::Required)) {
    return false;
  }
  return true;
}

SpectrumCandidate candidateFromReservation(const SpectrumReservation& reservation) {
  SpectrumCandidate candidate;
  candidate.ordinal = 0;
  candidate.anchorDomain = reservation.anchorDomain;
  candidate.slots = reservation.slots;
  candidate.frequency = reservation.frequency;
  candidate.perDomainSlots = reservation.perDomainSlots;
  candidate.crossGrid = reservation.crossGrid;
  candidate.eligibility = CandidateEligibility::Eligible;
  candidate.detail = "already committed as " + typedToken("reservation", reservation.id);
  return candidate;
}

CandidateSet enumerateWithOutcome(const RuntimeState& state, const SpectrumRequest& request,
                                 AllocationOutcome& outcome) {
  outcome = AllocationOutcome::Unknown;
  CandidateSet result;
  const Status shape = validateRequestShape(request);
  if (!shape.ok()) {
    outcome = AllocationOutcome::RefusedInvalidRequest;
    result.status = shape;
    result.complete = false;
    result.summary = "request rejected: " + shape.message;
    return result;
  }

  const Preparation prep = prepare(state, request);
  if (!prep.status.ok()) {
    outcome = prep.outcome;
    result.status = prep.status;
    result.complete = false;
    result.summary = prep.reason;
    return result;
  }

  const ChannelGrid& requestGrid = state.grids.at(request.grid);
  const Instant now = request.requestedAt;

  // ---- search window inside the request grid ------------------------------
  std::vector<SlotRange> ranges;
  for (const DomainContext& context : prep.contexts) {
    if (context.conversionRequired) continue;
    SlotRange window = allocatableWindow(*context.capability);
    if (!slotRangeInGrid(*context.grid, window)) {
      outcome = AllocationOutcome::RefusedUnsupported;
      result.status = fail(StatusCode::Unsupported,
                           "domain " + typedToken("domain", context.id) +
                               " has no allocatable window inside its grid");
      result.complete = false;
      result.summary = result.status.message;
      return result;
    }
    const FrequencyRange tunable{context.capability->minTunableMhz,
                                 context.capability->maxTunableMhz};
    SlotRange tunableSlots;
    if (!innerSlotRange(requestGrid, tunable, tunableSlots)) {
      outcome = AllocationOutcome::RefusedUnsupported;
      result.status = fail(StatusCode::Unsupported,
                           "domain " + typedToken("domain", context.id) +
                               " has no tunable slot inside the request grid");
      result.complete = false;
      result.summary = result.status.message;
      return result;
    }
    window = intersectRange(window, tunableSlots);
    if (window.empty()) {
      outcome = AllocationOutcome::RefusedUnsupported;
      result.status = fail(StatusCode::Unsupported,
                           "domain " + typedToken("domain", context.id) +
                               " has no slot that is both allocatable and tunable");
      result.complete = false;
      result.summary = result.status.message;
      return result;
    }
    ranges.push_back(window);
  }

  if (!request.frequencyWindows.empty()) {
    std::vector<SlotRange> windowRanges;
    for (const FrequencyRange& window : request.frequencyWindows) {
      SlotRange mapped;
      if (innerSlotRange(requestGrid, window, mapped)) windowRanges.push_back(mapped);
    }
    std::vector<SlotRange> intersected;
    for (const SlotRange& base : ranges) {
      for (const SlotRange& window : windowRanges) {
        const SlotRange hit = intersectRange(base, window);
        if (!hit.empty()) intersected.push_back(hit);
      }
    }
    ranges.swap(intersected);
  }
  mergeRanges(ranges);

  // ---- blocking intervals -------------------------------------------------
  std::map<ReservationId, Blocked> blocks;
  for (const DomainContext& context : prep.contexts) {
    collectDomainBlocks(state, context.id, now, request.guardBandMhz, 0, ExclusionDomainId{}, blocks);
    const auto exclusions = state.domainExclusions.find(context.id);
    if (exclusions == state.domainExclusions.end()) continue;
    for (const ExclusionDomainId exclusionId : exclusions->second) {
      const auto exclusionIt = state.exclusionDomains.find(exclusionId);
      if (exclusionIt == state.exclusionDomains.end()) continue;
      for (const SpectrumDomainId member : exclusionIt->second.members) {
        if (member == context.id) continue;
        collectDomainBlocks(state, member, now, request.guardBandMhz,
                            exclusionIt->second.guardBandMhz, exclusionId, blocks);
      }
    }
  }
  for (const ReservationId id : request.constraints.mustNotConflictWith) {
    const auto reservationIt = state.reservations.find(id);
    if (reservationIt == state.reservations.end()) continue;
    const SpectrumReservation& reservation = reservationIt->second;
    if (!isLiveAt(reservation, now)) continue;
    const std::int64_t guard = reservation.guardBandMhz > request.guardBandMhz
                                   ? reservation.guardBandMhz
                                   : request.guardBandMhz;
    const FrequencyRange expanded = expandByGuard(reservation.frequency, guard);
    auto existing = blocks.find(id);
    if (existing == blocks.end()) {
      blocks.emplace(id, Blocked{expanded.lowMhz, expanded.highMhz, id, ExclusionDomainId{}});
    } else {
      if (expanded.lowMhz < existing->second.lowMhz) existing->second.lowMhz = expanded.lowMhz;
      if (expanded.highMhz > existing->second.highMhz) existing->second.highMhz = expanded.highMhz;
    }
  }
  if (blocks.size() > kMaxBlockedIntervals) {
    outcome = AllocationOutcome::RefusedLimitExceeded;
    result.status = fail(StatusCode::LimitExceeded,
                         "request evaluation would examine " + std::to_string(blocks.size()) +
                             " competing reservations, above the supported bound of " +
                             std::to_string(kMaxBlockedIntervals));
    result.complete = false;
    result.summary = result.status.message;
    return result;
  }

  std::vector<SlotRange> blockedSlots;
  blockedSlots.reserve(blocks.size());
  for (const auto& entry : blocks) {
    SlotRange mapped;
    if (outerSlotRange(requestGrid, FrequencyRange{entry.second.lowMhz, entry.second.highMhz},
                       mapped)) {
      blockedSlots.push_back(mapped);
    }
  }
  mergeRanges(blockedSlots);

  // ---- free runs ----------------------------------------------------------
  std::vector<SlotRange> freeRuns;
  std::vector<SlotRange> blockedWindows;
  for (const SlotRange& base : ranges) {
    std::vector<SlotRange> pieces{base};
    for (const SlotRange& cut : blockedSlots) {
      if (pieces.empty()) break;
      std::vector<SlotRange> next;
      for (const SlotRange& piece : pieces) {
        if (!piece.overlaps(cut)) {
          next.push_back(piece);
        } else {
          subtractRange(next, piece, cut);
          const SlotRange hit = intersectRange(piece, cut);
          if (!hit.empty()) blockedWindows.push_back(hit);
        }
      }
      pieces.swap(next);
    }
    for (const SlotRange& piece : pieces) freeRuns.push_back(piece);
  }
  mergeRanges(freeRuns);
  mergeRanges(blockedWindows);

  std::uint64_t totalStarts = 0;
  for (const SlotRange& run : freeRuns) {
    if (run.count >= request.slots) {
      totalStarts += static_cast<std::uint64_t>(run.count - request.slots) + 1;
    }
  }

  const std::size_t limit = state.config.maxCandidatesPerRequest;
  std::uint64_t emitted = 0;
  bool truncated = false;
  for (const SlotRange& run : freeRuns) {
    if (run.count < request.slots) continue;
    const std::uint32_t lastStart = run.end() - request.slots;
    for (std::uint32_t start = run.first; start <= lastStart; ++start) {
      if (result.candidates.size() >= limit) {
        truncated = true;
        break;
      }
      SpectrumCandidate candidate;
      candidate.ordinal = result.candidates.size();
      candidate.anchorDomain = prep.contexts[0].id;
      evaluateCandidate(state, request, requestGrid, prep.contexts, start, candidate);
      if (isEligible(candidate.eligibility)) {
        ++result.eligibleCount;
      } else {
        ++emitted;
      }
      result.candidates.push_back(std::move(candidate));
    }
    if (truncated) break;
  }
  (void)emitted;

  if (truncated) {
    result.complete = false;
    result.omitted = static_cast<std::size_t>(totalStarts - result.candidates.size());
  }

  // ---- diagnostics when nothing is usable ---------------------------------
  if (result.eligibleCount == 0 && !blockedWindows.empty()) {
    std::size_t produced = 0;
    for (const SlotRange& blockedWindow : blockedWindows) {
      if (produced >= kMaxDiagnosticCandidates) break;
      if (blockedWindow.count < request.slots) continue;
      SpectrumCandidate candidate;
      candidate.ordinal = result.candidates.size();
      candidate.anchorDomain = prep.contexts[0].id;
      evaluateCandidate(state, request, requestGrid, prep.contexts, blockedWindow.first, candidate);
      if (isEligible(candidate.eligibility)) ++result.eligibleCount;
      result.candidates.push_back(std::move(candidate));
      ++produced;
    }
  }

  result.status = okStatus();
  result.summary = "evaluated " + std::to_string(result.candidates.size()) + " candidate(s), " +
                   std::to_string(result.eligibleCount) + " eligible";
  if (result.eligibleCount > 0) outcome = AllocationOutcome::Allocated;
  return result;
}

CandidateSet enumerate(const RuntimeState& state, const SpectrumRequest& request) {
  AllocationOutcome ignored = AllocationOutcome::Unknown;
  return enumerateWithOutcome(state, request, ignored);
}

Evaluation evaluate(const RuntimeState& state, const SpectrumRequest& request) {
  Evaluation evaluation;

  const Status shape = validateRequestShape(request);
  if (!shape.ok()) {
    evaluation.refuse(AllocationOutcome::RefusedInvalidRequest, shape.message);
    return evaluation;
  }

  const Status eligibility =
      checkEligibilityAuthority(state.authority, request.eligibilityAuthority);
  if (!eligibility.ok()) {
    evaluation.refuse(outcomeForStatus(eligibility.code), eligibility.message);
    return evaluation;
  }
  const Status reservation =
      checkReservationAuthority(state.authority, request.reservationAuthority);
  if (!reservation.ok()) {
    evaluation.refuse(outcomeForStatus(reservation.code), reservation.message);
    return evaluation;
  }

  // Replaying a request identity that is already committed is idempotent: it
  // never creates a second reservation and never mutates ownership. A newer
  // request generation supersedes the previous reservation at commit time.
  const auto existingRequest = state.requestIndex.find(request.requestId);
  if (existingRequest != state.requestIndex.end()) {
    const auto existingIt = state.reservations.find(existingRequest->second);
    if (existingIt != state.reservations.end()) {
      const SpectrumReservation& existing = existingIt->second;
      if (request.requestGeneration < existing.requestGeneration) {
        evaluation.refuse(AllocationOutcome::RefusedStaleGeneration,
                          "request " + typedToken("request", request.requestId) +
                              " is committed at generation " +
                              std::to_string(existing.requestGeneration.raw()) +
                              "; generation " +
                              std::to_string(request.requestGeneration.raw()) + " is stale");
        return evaluation;
      }
      if (request.requestGeneration == existing.requestGeneration) {
        if (sameRequestShape(existing, request)) {
          evaluation.replay = true;
          evaluation.replayValue = existing;
          evaluation.eligible = true;
          evaluation.outcome = AllocationOutcome::Allocated;
          evaluation.selected = candidateFromReservation(existing);
          evaluation.candidatesEnumerated = evaluation.replayValue.perDomainSlots.size();
          evaluation.refuse(AllocationOutcome::Allocated,
                            "idempotent replay: request " +
                                typedToken("request", request.requestId) +
                                " is already committed as " +
                                typedToken("reservation", existing.id) + " at generation " +
                                std::to_string(existing.requestGeneration.raw()));
          return evaluation;
        }
        evaluation.refuse(AllocationOutcome::RefusedDuplicate,
                          "request " + typedToken("request", request.requestId) +
                              " is already committed at generation " +
                              std::to_string(existing.requestGeneration.raw()) +
                              " with a different shape; a matching replay is required");
        return evaluation;
      }
    }
  }

  AllocationOutcome preparedOutcome = AllocationOutcome::Unknown;
  const CandidateSet candidates = enumerateWithOutcome(state, request, preparedOutcome);
  evaluation.candidatesEnumerated = candidates.candidates.size();
  evaluation.candidatesOmitted = candidates.omitted;

  if (!candidates.status.ok()) {
    evaluation.refuse(preparedOutcome != AllocationOutcome::Unknown
                          ? preparedOutcome
                          : outcomeForStatus(candidates.status.code),
                      candidates.summary);
    if (!evaluation.reasons.empty()) {
      for (std::size_t index = 0; index < candidates.candidates.size(); ++index) {
        if (evaluation.rejected.size() >= state.config.maxRejectedExplanations) break;
        evaluation.rejected.push_back(candidates.candidates[index]);
      }
    }
    return evaluation;
  }

  for (const SpectrumCandidate& candidate : candidates.candidates) {
    if (isEligible(candidate.eligibility)) {
      evaluation.selected = candidate;
      evaluation.candidateOrdinal = candidate.ordinal;
      evaluation.eligible = true;
      evaluation.outcome = AllocationOutcome::Allocated;
      evaluation.refuse(AllocationOutcome::Allocated,
                        "allocated " + std::to_string(candidate.slots.count) +
                            " slot(s) starting at " + std::to_string(candidate.slots.first) +
                            " on grid " + typedToken("grid", request.grid));
      return evaluation;
    }
    ++evaluation.candidatesRejected;
    evaluation.noteConflicts(candidate.conflicts);
    if (evaluation.rejected.size() < state.config.maxRejectedExplanations) {
      evaluation.rejected.push_back(candidate);
    }
  }

  AllocationOutcome outcome = AllocationOutcome::RefusedNoCapacity;
  int bestRank = -1;
  for (const SpectrumCandidate& candidate : candidates.candidates) {
    if (isEligible(candidate.eligibility)) continue;
    const AllocationOutcome candidateOutcome = dominantOutcome(candidate);
    const int rank = outcomeRank(candidateOutcome);
    if (rank > bestRank) {
      bestRank = rank;
      outcome = candidateOutcome;
    }
  }
  for (const SpectrumCandidate& candidate : candidates.candidates) {
    if (isEligible(candidate.eligibility)) continue;
    if (dominantOutcome(candidate) != outcome) continue;
    noteReason(evaluation, candidate.eligibility);
    evaluation.refuse(outcome, candidate.detail);
  }
  if (evaluation.reasons.empty()) {
    evaluation.refuse(outcome, "no candidate satisfies the request");
  } else {
    evaluation.outcome = outcome;
  }
  return evaluation;
}

AllocationOutcome outcomeForStatus(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok:
      return AllocationOutcome::Allocated;
    case StatusCode::InvalidArgument:
      return AllocationOutcome::RefusedInvalidRequest;
    case StatusCode::NotFound:
      return AllocationOutcome::RefusedUnknownDomain;
    case StatusCode::Duplicate:
      return AllocationOutcome::RefusedDuplicate;
    case StatusCode::LimitExceeded:
      return AllocationOutcome::RefusedLimitExceeded;
    case StatusCode::Unsupported:
      return AllocationOutcome::RefusedUnsupported;
    case StatusCode::Unknown:
      return AllocationOutcome::RefusedUnknownCapability;
    case StatusCode::StaleGeneration:
      return AllocationOutcome::RefusedStaleGeneration;
    case StatusCode::StaleIncarnation:
      return AllocationOutcome::RefusedStaleIncarnation;
    case StatusCode::StaleEpoch:
      return AllocationOutcome::RefusedStaleEpoch;
    case StatusCode::StaleAuthority:
      return AllocationOutcome::RefusedStaleAuthority;
    case StatusCode::IllegalTransition:
      return AllocationOutcome::RefusedNotPermitted;
    case StatusCode::Conflict:
      return AllocationOutcome::RefusedConflict;
    case StatusCode::Excluded:
      return AllocationOutcome::RefusedExclusion;
    case StatusCode::PersistenceFailure:
    case StatusCode::Corruption:
    case StatusCode::IoError:
      return AllocationOutcome::Unknown;
    case StatusCode::NotPermitted:
      return AllocationOutcome::RefusedConversion;
    case StatusCode::Overflow:
      return AllocationOutcome::RefusedLimitExceeded;
    case StatusCode::Refused:
      return AllocationOutcome::RefusedConstraint;
    case StatusCode::Unavailable:
      return AllocationOutcome::RefusedNoCapacity;
  }
  return AllocationOutcome::Unknown;
}

}  // namespace detail
}  // namespace wavelength_fabric
