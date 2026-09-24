#include "wavelength_fabric/audit.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "wavelength_fabric/authority.hpp"
#include "wavelength_fabric/candidate.hpp"
#include "wavelength_fabric/capability.hpp"
#include "wavelength_fabric/decision.hpp"
#include "wavelength_fabric/error.hpp"
#include "wavelength_fabric/grid.hpp"
#include "wavelength_fabric/identity.hpp"
#include "wavelength_fabric/lifecycle.hpp"
#include "wavelength_fabric/quantity.hpp"
#include "wavelength_fabric/request.hpp"
#include "wavelength_fabric/resource.hpp"
#include "wavelength_fabric/time.hpp"

// Tokens, structural validation and the lifecycle state machine of the
// Wavelength Fabric data model.
//
// Everything in this translation unit is pure: no allocation is sized from a
// value that was not bounded first, no container is iterated in an unordered
// order, and every refusal names the field that failed so a caller can act on it
// without guessing.

namespace wavelength_fabric {
namespace {

// Bounds that are local to the model.
constexpr std::size_t kMaxLabelBytes = 256;
constexpr std::size_t kMaxDetailBytes = 4096;

// Ten years in nanoseconds. The lease ceiling is a structural limit on what a
// single request may reserve, not a policy statement.
constexpr std::int64_t kMaxLeaseNanos = 3650ll * 24ll * 60ll * 60ll * 1'000'000'000ll;

// Number of ReservationState enumerators, including None.
constexpr std::size_t kReservationStateCount = 18;

template <class Enum>
struct TokenEntry {
  Enum value;
  std::string_view token;
};

// Exact-match token lookup. Each table is the single source of truth for both
// directions, so a token can never exist in one direction only, and an
// unrecognised input always yields the documented fallback.
template <class Enum, std::size_t N>
[[nodiscard]] constexpr std::string_view tokenOf(Enum value, const TokenEntry<Enum> (&table)[N],
                                                 std::string_view fallback) noexcept {
  for (std::size_t i = 0; i < N; ++i) {
    if (table[i].value == value) return table[i].token;
  }
  return fallback;
}

template <class Enum, std::size_t N>
[[nodiscard]] constexpr Enum valueOf(std::string_view token, const TokenEntry<Enum> (&table)[N],
                                     Enum fallback) noexcept {
  for (std::size_t i = 0; i < N; ++i) {
    if (table[i].token == token) return table[i].value;
  }
  return fallback;
}

// Reservation state tokens live here because the compile-time explanation table
// below needs them; the public accessors read the same array.
constexpr TokenEntry<ReservationState> kReservationStateTokens[] = {
    {ReservationState::None, "none"},
    {ReservationState::Requested, "requested"},
    {ReservationState::Evaluated, "evaluated"},
    {ReservationState::Committing, "committing"},
    {ReservationState::Reserved, "reserved"},
    {ReservationState::Activating, "activating"},
    {ReservationState::Active, "active"},
    {ReservationState::Deactivating, "deactivating"},
    {ReservationState::Renewing, "renewing"},
    {ReservationState::Releasing, "releasing"},
    {ReservationState::Reclaiming, "reclaiming"},
    {ReservationState::Recovering, "recovering"},
    {ReservationState::Released, "released"},
    {ReservationState::Expired, "expired"},
    {ReservationState::Reclaimed, "reclaimed"},
    {ReservationState::Superseded, "superseded"},
    {ReservationState::Refused, "refused"},
    {ReservationState::Retired, "retired"},
};

// The single description of the legal transitions. isLegalTransition delegates
// here and the explanation table below is derived from it, so the state machine
// and its explanations can never disagree.
[[nodiscard]] constexpr bool transitionAllowed(ReservationState from,
                                               ReservationState to) noexcept {
  switch (from) {
    case ReservationState::None:
      return to == ReservationState::Requested;
    case ReservationState::Requested:
      return to == ReservationState::Evaluated || to == ReservationState::Refused;
    case ReservationState::Evaluated:
      return to == ReservationState::Committing || to == ReservationState::Refused;
    case ReservationState::Committing:
      return to == ReservationState::Reserved || to == ReservationState::Refused;
    case ReservationState::Reserved:
      return to == ReservationState::Activating || to == ReservationState::Renewing ||
             to == ReservationState::Releasing || to == ReservationState::Reclaiming ||
             to == ReservationState::Expired || to == ReservationState::Superseded;
    case ReservationState::Activating:
      return to == ReservationState::Active || to == ReservationState::Reserved ||
             to == ReservationState::Reclaiming;
    case ReservationState::Active:
      return to == ReservationState::Deactivating || to == ReservationState::Renewing ||
             to == ReservationState::Releasing || to == ReservationState::Reclaiming ||
             to == ReservationState::Expired || to == ReservationState::Superseded;
    case ReservationState::Deactivating:
      return to == ReservationState::Reserved || to == ReservationState::Active ||
             to == ReservationState::Releasing || to == ReservationState::Reclaiming;
    case ReservationState::Renewing:
      return to == ReservationState::Reserved || to == ReservationState::Active ||
             to == ReservationState::Expired || to == ReservationState::Reclaiming;
    case ReservationState::Releasing:
      return to == ReservationState::Released;
    case ReservationState::Reclaiming:
      return to == ReservationState::Reclaimed;
    case ReservationState::Expired:
      return to == ReservationState::Reclaiming || to == ReservationState::Reclaimed;
    case ReservationState::Recovering:
      return to == ReservationState::Reserved || to == ReservationState::Expired ||
             to == ReservationState::Reclaiming;
    case ReservationState::Released:
    case ReservationState::Reclaimed:
    case ReservationState::Superseded:
    case ReservationState::Refused:
    case ReservationState::Retired:
      return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Static transition explanations
// ---------------------------------------------------------------------------
//
// illegalTransitionReason returns a string_view, so every (from, to) pair needs
// text with static storage. The text is therefore composed once at compile time
// into one immutable buffer; legal pairs keep an empty span, which is exactly
// what the caller receives for them.

struct TransitionReasonSpan {
  std::uint32_t offset{0};
  std::uint32_t length{0};
};

constexpr std::size_t kTransitionReasonEntryBytes = 64;

struct TransitionReasonTable {
  std::array<char, kReservationStateCount * kReservationStateCount * kTransitionReasonEntryBytes>
      text{};
  std::array<TransitionReasonSpan, kReservationStateCount * kReservationStateCount> spans{};
  bool overflowed{false};
};

[[nodiscard]] constexpr bool appendTransitionText(TransitionReasonTable& table, std::size_t limit,
                                                  std::size_t& used,
                                                  std::string_view piece) noexcept {
  for (const char c : piece) {
    if (used >= limit) {
      table.overflowed = true;
      return false;
    }
    table.text[used] = c;
    ++used;
  }
  return true;
}

[[nodiscard]] constexpr TransitionReasonTable buildTransitionReasonTable() noexcept {
  TransitionReasonTable table{};
  std::size_t used = 0;
  for (std::size_t from = 0; from < kReservationStateCount; ++from) {
    for (std::size_t to = 0; to < kReservationStateCount; ++to) {
      const ReservationState fromState = static_cast<ReservationState>(from);
      const ReservationState toState = static_cast<ReservationState>(to);
      if (transitionAllowed(fromState, toState)) continue;
      const std::size_t start = used;
      const std::size_t limit = start + kTransitionReasonEntryBytes;
      const bool complete =
          appendTransitionText(table, limit, used,
                               tokenOf(fromState, kReservationStateTokens, "none")) &&
          appendTransitionText(table, limit, used, " -> ") &&
          appendTransitionText(table, limit, used,
                               tokenOf(toState, kReservationStateTokens, "none")) &&
          appendTransitionText(table, limit, used, " is not a legal transition");
      if (!complete) continue;
      table.spans[from * kReservationStateCount + to] =
          TransitionReasonSpan{static_cast<std::uint32_t>(start),
                               static_cast<std::uint32_t>(used - start)};
    }
  }
  return table;
}

constexpr TransitionReasonTable kTransitionReasons = buildTransitionReasonTable();

static_assert(!kTransitionReasons.overflowed,
              "the static transition reason buffer must hold the longest explanation");

// ---------------------------------------------------------------------------
// Authority helpers
// ---------------------------------------------------------------------------

[[nodiscard]] Status checkFenceFields(std::string_view subject, const ControllerFence& current,
                                      const ControllerFence& candidate) {
  if (candidate.epoch != current.epoch)
    return fail(StatusCode::StaleEpoch,
                std::string(subject) + " epoch " + std::to_string(candidate.epoch.raw()) +
                    " does not match the current epoch " + std::to_string(current.epoch.raw()));
  if (candidate.incarnation != current.incarnation)
    return fail(StatusCode::StaleIncarnation,
                std::string(subject) + " incarnation " +
                    std::to_string(candidate.incarnation.raw()) +
                    " does not match the current incarnation " +
                    std::to_string(current.incarnation.raw()));
  return Status::success();
}

[[nodiscard]] Status checkAuthorityGeneration(std::string_view domain,
                                              std::uint64_t tokenGeneration,
                                              std::uint64_t currentGeneration) {
  if (tokenGeneration == currentGeneration) return Status::success();
  return fail(StatusCode::StaleGeneration,
              std::string(domain) + " authority generation " + std::to_string(tokenGeneration) +
                  " does not match the current generation " + std::to_string(currentGeneration));
}

}  // namespace

// ---------------------------------------------------------------------------
// Resource classes
// ---------------------------------------------------------------------------

namespace {

constexpr TokenEntry<ResourceClass> kResourceClassTokens[] = {
    {ResourceClass::Unknown, "unknown"},
    {ResourceClass::FiberSpan, "fiber-span"},
    {ResourceClass::MediaChannel, "media-channel"},
    {ResourceClass::OpticalPort, "optical-port"},
    {ResourceClass::AmplifierBand, "amplifier-band"},
    {ResourceClass::AbstractDomain, "abstract-domain"},
};

}  // namespace

std::string_view toToken(ResourceClass klass) noexcept {
  return tokenOf(klass, kResourceClassTokens, "unknown");
}

ResourceClass resourceClassFromToken(std::string_view token) noexcept {
  return valueOf(token, kResourceClassTokens, ResourceClass::Unknown);
}

Status validateSpan(const Span& span) {
  if (!span.id.valid()) return fail(StatusCode::InvalidArgument, "span id must be non-zero");
  if (!span.generation.valid())
    return fail(StatusCode::InvalidArgument, "span generation must be non-zero");
  if (span.label.size() > kMaxLabelBytes)
    return fail(StatusCode::InvalidArgument,
                "span label of " + std::to_string(span.label.size()) +
                    " bytes exceeds the maximum of " + std::to_string(kMaxLabelBytes) + " bytes");
  return Status::success();
}

Status validatePort(const OpticalPort& port) {
  if (!port.id.valid()) return fail(StatusCode::InvalidArgument, "port id must be non-zero");
  if (!port.generation.valid())
    return fail(StatusCode::InvalidArgument, "port generation must be non-zero");
  if (port.label.size() > kMaxLabelBytes)
    return fail(StatusCode::InvalidArgument,
                "port label of " + std::to_string(port.label.size()) +
                    " bytes exceeds the maximum of " + std::to_string(kMaxLabelBytes) + " bytes");
  return Status::success();
}

Status validateDomain(const SpectrumDomain& domain) {
  if (!domain.id.valid()) return fail(StatusCode::InvalidArgument, "domain id must be non-zero");
  if (!domain.generation.valid())
    return fail(StatusCode::InvalidArgument, "domain generation must be non-zero");
  if (domain.klass == ResourceClass::Unknown)
    return fail(StatusCode::InvalidArgument, "domain resource class must be known");
  if (!domain.grid.valid())
    return fail(StatusCode::InvalidArgument, "domain grid id must be non-zero");
  if (!domain.gridGeneration.valid())
    return fail(StatusCode::InvalidArgument, "domain grid generation must be non-zero");
  if (domain.label.size() > kMaxLabelBytes)
    return fail(StatusCode::InvalidArgument,
                "domain label of " + std::to_string(domain.label.size()) +
                    " bytes exceeds the maximum of " + std::to_string(kMaxLabelBytes) + " bytes");
  if (domain.span.valid() && !domain.spanGeneration.valid())
    return fail(StatusCode::InvalidArgument,
                "domain references span " + std::to_string(domain.span.raw()) +
                    " without a span generation");
  if (domain.portA.valid() && !domain.portAGeneration.valid())
    return fail(StatusCode::InvalidArgument,
                "domain references port " + std::to_string(domain.portA.raw()) +
                    " as endpoint A without a port generation");
  if (domain.portB.valid() && !domain.portBGeneration.valid())
    return fail(StatusCode::InvalidArgument,
                "domain references port " + std::to_string(domain.portB.raw()) +
                    " as endpoint B without a port generation");
  if (domain.portA.valid() && domain.portB.valid() && domain.portA == domain.portB)
    return fail(StatusCode::InvalidArgument,
                "domain endpoints must differ, but both are port " +
                    std::to_string(domain.portA.raw()));
  return Status::success();
}

Status validateExclusionDomain(const ExclusionDomain& domain) {
  if (!domain.id.valid())
    return fail(StatusCode::InvalidArgument, "exclusion domain id must be non-zero");
  if (!domain.generation.valid())
    return fail(StatusCode::InvalidArgument, "exclusion domain generation must be non-zero");
  if (domain.members.empty())
    return fail(StatusCode::InvalidArgument,
                "exclusion domain must name at least one member domain");
  if (domain.members.size() > kMaxExclusionDomainMembers)
    return fail(StatusCode::InvalidArgument,
                "exclusion domain member count " + std::to_string(domain.members.size()) +
                    " exceeds the maximum of " + std::to_string(kMaxExclusionDomainMembers));
  for (std::size_t i = 1; i < domain.members.size(); ++i) {
    if (!(domain.members[i - 1] < domain.members[i]))
      return fail(StatusCode::InvalidArgument,
                  "exclusion domain members must be strictly ascending, but member " +
                      std::to_string(i) + " is not greater than member " + std::to_string(i - 1));
  }
  for (std::size_t i = 0; i < domain.members.size(); ++i) {
    if (!domain.members[i].valid())
      return fail(StatusCode::InvalidArgument,
                  "exclusion domain member " + std::to_string(i) + " must be a non-zero domain id");
  }
  if (domain.guardBandMhz < 0 || domain.guardBandMhz > kMaxGuardBandMhz)
    return fail(StatusCode::InvalidArgument,
                "exclusion domain guard band " + std::to_string(domain.guardBandMhz) +
                    " MHz is outside [0, " + std::to_string(kMaxGuardBandMhz) + "] MHz");
  if (domain.label.size() > kMaxLabelBytes)
    return fail(StatusCode::InvalidArgument,
                "exclusion domain label of " + std::to_string(domain.label.size()) +
                    " bytes exceeds the maximum of " + std::to_string(kMaxLabelBytes) + " bytes");
  return Status::success();
}

bool exclusionDomainContains(const ExclusionDomain& domain, SpectrumDomainId member) noexcept {
  // Members are validated as strictly ascending on registration, so the ordered
  // search below is exact and independent of the order they were supplied in.
  std::size_t low = 0;
  std::size_t high = domain.members.size();
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    const SpectrumDomainId probe = domain.members[mid];
    if (probe == member) return true;
    if (probe < member) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Capability
// ---------------------------------------------------------------------------

namespace {

constexpr TokenEntry<SpectrumSupport> kSpectrumSupportTokens[] = {
    {SpectrumSupport::Unsupported, "unsupported"},
    {SpectrumSupport::Supported, "supported"},
    {SpectrumSupport::Unknown, "unknown"},
};

}  // namespace

std::string_view toToken(SpectrumSupport support) noexcept {
  return tokenOf(support, kSpectrumSupportTokens, "unknown");
}

SpectrumSupport supportFromToken(std::string_view token) noexcept {
  return valueOf(token, kSpectrumSupportTokens, SpectrumSupport::Unknown);
}

Status validateCapabilityShape(const SpectrumCapability& capability) {
  if (!capability.domain.valid())
    return fail(StatusCode::InvalidArgument, "capability domain id must be non-zero");
  if (!capability.domainGeneration.valid())
    return fail(StatusCode::InvalidArgument, "capability domain generation must be non-zero");
  if (!capability.grid.valid())
    return fail(StatusCode::InvalidArgument, "capability grid id must be non-zero");
  if (!capability.gridGeneration.valid())
    return fail(StatusCode::InvalidArgument, "capability grid generation must be non-zero");
  if (!capability.publisher.valid())
    return fail(StatusCode::InvalidArgument, "capability publisher must be non-zero");
  if (!capability.fence.valid())
    return fail(StatusCode::InvalidArgument,
                "capability fence must name both an epoch and an incarnation");
  if (!capability.generation.valid())
    return fail(StatusCode::InvalidArgument, "capability generation must be non-zero");
  if (capability.publishedAt.nanos() <= 0)
    return fail(StatusCode::InvalidArgument, "capability publication instant must be positive");
  if (capability.detail.size() > kMaxDetailBytes)
    return fail(StatusCode::InvalidArgument,
                "capability detail of " + std::to_string(capability.detail.size()) +
                    " bytes exceeds the maximum of " + std::to_string(kMaxDetailBytes) + " bytes");
  if (capability.support == SpectrumSupport::Supported) {
    if (!capability.presenceEvidence.usable())
      return fail(StatusCode::InvalidArgument, "supported capability requires explicit evidence");
    if (capability.allocatableSlots == 0 || capability.allocatableSlots > kMaxGridSlots)
      return fail(StatusCode::InvalidArgument,
                  "supported capability allocatable slot count " +
                      std::to_string(capability.allocatableSlots) + " is outside [1, " +
                      std::to_string(kMaxGridSlots) + "]");
    // The window is deliberately not range-checked against a grid here: the
    // runtime checks it against the registered grid. Overflow is still refused.
    std::int64_t windowEnd = 0;
    if (addOverflow(static_cast<std::int64_t>(capability.firstAllocatableSlot),
                    static_cast<std::int64_t>(capability.allocatableSlots), windowEnd))
      return fail(StatusCode::InvalidArgument,
                  "supported capability allocatable window overflows the slot index space");
    if (capability.minTunableMhz < kMinFrequencyMhz)
      return fail(StatusCode::InvalidArgument,
                  "supported capability minimum tunable frequency " +
                      std::to_string(capability.minTunableMhz) + " MHz is below the minimum of " +
                      std::to_string(kMinFrequencyMhz) + " MHz");
    if (capability.maxTunableMhz <= capability.minTunableMhz)
      return fail(StatusCode::InvalidArgument,
                  "supported capability tunable range [" +
                      std::to_string(capability.minTunableMhz) + ", " +
                      std::to_string(capability.maxTunableMhz) + "] MHz must not be empty");
    if (capability.maxTunableMhz > kMaxFrequencyMhz)
      return fail(StatusCode::InvalidArgument,
                  "supported capability maximum tunable frequency " +
                      std::to_string(capability.maxTunableMhz) + " MHz exceeds the maximum of " +
                      std::to_string(kMaxFrequencyMhz) + " MHz");
  } else {
    if (capability.allocatableSlots != 0 || capability.firstAllocatableSlot != 0)
      return fail(StatusCode::InvalidArgument,
                  "an unsupported or unknown capability must not declare allocatable slots");
  }
  if (capability.conversionSupported && !capability.conversionEvidence.usable())
    return fail(StatusCode::InvalidArgument,
                "conversion capability requires explicit evidence; it is never assumed");
  return Status::success();
}

SlotRange allocatableWindow(const SpectrumCapability& capability) noexcept {
  SlotRange window;
  window.first = capability.firstAllocatableSlot;
  window.count = capability.allocatableSlots;
  return window;
}

// ---------------------------------------------------------------------------
// Request
// ---------------------------------------------------------------------------

namespace {

constexpr TokenEntry<ContinuityRequirement> kContinuityTokens[] = {
    {ContinuityRequirement::Unspecified, "unspecified"},
    {ContinuityRequirement::Required, "required"},
    {ContinuityRequirement::NotRequired, "not-required"},
};

constexpr TokenEntry<ContiguityRequirement> kContiguityTokens[] = {
    {ContiguityRequirement::Unspecified, "unspecified"},
    {ContiguityRequirement::Required, "required"},
    {ContiguityRequirement::NotRequired, "not-required"},
};

}  // namespace

std::string_view toToken(ContinuityRequirement requirement) noexcept {
  return tokenOf(requirement, kContinuityTokens, "unspecified");
}

std::string_view toToken(ContiguityRequirement requirement) noexcept {
  return tokenOf(requirement, kContiguityTokens, "unspecified");
}

Status validateRequestShape(const SpectrumRequest& request) {
  if (!request.requestId.valid())
    return fail(StatusCode::InvalidArgument, "request id must be non-zero");
  if (!request.requestGeneration.valid())
    return fail(StatusCode::InvalidArgument, "request generation must be non-zero");
  if (!request.owner.valid())
    return fail(StatusCode::InvalidArgument, "request owner must be non-zero");
  if (!request.ownerGeneration.valid())
    return fail(StatusCode::InvalidArgument, "request owner generation must be non-zero");
  if (request.domains.empty())
    return fail(StatusCode::InvalidArgument, "request must name at least one spectrum domain");
  if (request.domains.size() > kMaxRequestDomains)
    return fail(StatusCode::InvalidArgument,
                "request domain count " + std::to_string(request.domains.size()) +
                    " exceeds the maximum of " + std::to_string(kMaxRequestDomains));
  if (request.domainGenerations.size() != request.domains.size())
    return fail(StatusCode::InvalidArgument,
                "request carries " + std::to_string(request.domains.size()) + " domains but " +
                    std::to_string(request.domainGenerations.size()) + " domain generations");
  for (std::size_t i = 0; i < request.domains.size(); ++i) {
    if (!request.domains[i].valid())
      return fail(StatusCode::InvalidArgument,
                  "request domain " + std::to_string(i) + " must be a non-zero domain id");
    if (!request.domainGenerations[i].valid())
      return fail(StatusCode::InvalidArgument, "request domain " + std::to_string(i) +
                                                   " must carry a non-zero domain generation");
  }
  for (std::size_t i = 1; i < request.domains.size(); ++i) {
    if (!(request.domains[i - 1] < request.domains[i]))
      return fail(StatusCode::InvalidArgument,
                  "request domains must be strictly ascending and unique, but domain " +
                      std::to_string(i) + " is not greater than domain " + std::to_string(i - 1));
  }
  if (!request.grid.valid())
    return fail(StatusCode::InvalidArgument, "request grid id must be non-zero");
  if (!request.gridGeneration.valid())
    return fail(StatusCode::InvalidArgument, "request grid generation must be non-zero");
  if (request.slots == 0)
    return fail(StatusCode::InvalidArgument, "request channel width must be at least one slot");
  if (request.slots > kMaxSlotsPerChannel)
    return fail(StatusCode::InvalidArgument,
                "request channel width of " + std::to_string(request.slots) +
                    " slots exceeds the maximum of " + std::to_string(kMaxSlotsPerChannel) +
                    " slots");
  if (request.frequencyWindows.size() > kMaxFrequencyWindows)
    return fail(StatusCode::InvalidArgument,
                "request frequency window count " +
                    std::to_string(request.frequencyWindows.size()) + " exceeds the maximum of " +
                    std::to_string(kMaxFrequencyWindows));
  for (std::size_t i = 0; i < request.frequencyWindows.size(); ++i) {
    const FrequencyRange& window = request.frequencyWindows[i];
    if (window.empty() || window.lowMhz < kMinFrequencyMhz ||
        window.highMhz > kMaxFrequencyMhz)
      return fail(StatusCode::InvalidArgument,
                  "request frequency window " + std::to_string(i) + " [" +
                      std::to_string(window.lowMhz) + ", " + std::to_string(window.highMhz) +
                      "] MHz is empty or outside the supported frequency range");
  }
  if (request.constraints.excludedSlots.size() > kMaxExcludedRanges)
    return fail(StatusCode::InvalidArgument,
                "request excluded slot range count " +
                    std::to_string(request.constraints.excludedSlots.size()) +
                    " exceeds the maximum of " + std::to_string(kMaxExcludedRanges));
  for (std::size_t i = 0; i < request.constraints.excludedSlots.size(); ++i) {
    const SlotRange& range = request.constraints.excludedSlots[i];
    if (range.count == 0)
      return fail(StatusCode::InvalidArgument,
                  "request excluded slot range " + std::to_string(i) + " must not be empty");
    std::int64_t end = 0;
    if (addOverflow(static_cast<std::int64_t>(range.first),
                    static_cast<std::int64_t>(range.count), end))
      return fail(StatusCode::InvalidArgument, "request excluded slot range " +
                                                   std::to_string(i) +
                                                   " overflows the slot index space");
  }
  if (request.constraints.excludedFrequencies.size() > kMaxExcludedRanges)
    return fail(StatusCode::InvalidArgument,
                "request excluded frequency range count " +
                    std::to_string(request.constraints.excludedFrequencies.size()) +
                    " exceeds the maximum of " + std::to_string(kMaxExcludedRanges));
  for (std::size_t i = 0; i < request.constraints.excludedFrequencies.size(); ++i) {
    const FrequencyRange& range = request.constraints.excludedFrequencies[i];
    if (range.empty())
      return fail(StatusCode::InvalidArgument,
                  "request excluded frequency range " + std::to_string(i) + " [" +
                      std::to_string(range.lowMhz) + ", " + std::to_string(range.highMhz) +
                      "] MHz must not be empty");
  }
  if (request.constraints.mustNotConflictWith.size() > kMaxExcludedRanges)
    return fail(StatusCode::InvalidArgument,
                "request conflict reservation count " +
                    std::to_string(request.constraints.mustNotConflictWith.size()) +
                    " exceeds the maximum of " + std::to_string(kMaxExcludedRanges));
  for (std::size_t i = 0; i < request.constraints.mustNotConflictWith.size(); ++i) {
    if (!request.constraints.mustNotConflictWith[i].valid())
      return fail(StatusCode::InvalidArgument,
                  "request conflict reservation " + std::to_string(i) +
                      " must be a non-zero reservation id");
  }
  if (request.guardBandMhz < 0 || request.guardBandMhz > kMaxGuardBandMhz)
    return fail(StatusCode::InvalidArgument,
                "request guard band " + std::to_string(request.guardBandMhz) +
                    " MHz is outside [0, " + std::to_string(kMaxGuardBandMhz) + "] MHz");
  if (request.leaseDuration.nanos() <= 0)
    return fail(StatusCode::InvalidArgument, "request lease duration must be positive");
  if (request.leaseDuration.nanos() > kMaxLeaseNanos)
    return fail(StatusCode::InvalidArgument,
                "request lease duration of " + std::to_string(request.leaseDuration.nanos()) +
                    " ns exceeds the maximum of " + std::to_string(kMaxLeaseNanos) + " ns");
  if (request.requestedAt.nanos() <= 0)
    return fail(StatusCode::InvalidArgument, "request requestedAt instant must be positive");
  if (request.notBefore.nanos() < 0)
    return fail(StatusCode::InvalidArgument, "request notBefore instant must not be negative");
  std::int64_t leaseEnd = 0;
  if (addOverflow(request.requestedAt.nanos(), request.leaseDuration.nanos(), leaseEnd))
    return fail(StatusCode::InvalidArgument,
                "request lease end instant overflows the representable time range");
  if (!request.eligibilityAuthority.valid())
    return fail(StatusCode::InvalidArgument, "request eligibility authority token is required");
  if (!request.reservationAuthority.valid())
    return fail(StatusCode::InvalidArgument, "request reservation authority token is required");
  if (request.domains.size() > 1 && request.continuity == ContinuityRequirement::Unspecified)
    return fail(StatusCode::InvalidArgument,
                "continuity must be stated explicitly when a request spans multiple optical "
                "resources");
  if (request.slots > 1 && request.contiguity == ContiguityRequirement::Unspecified)
    return fail(StatusCode::InvalidArgument,
                "contiguity must be stated explicitly when a channel occupies more than one slot");
  return Status::success();
}

// ---------------------------------------------------------------------------
// Reservation lifecycle
// ---------------------------------------------------------------------------

std::string_view toToken(ReservationState state) noexcept {
  return tokenOf(state, kReservationStateTokens, "none");
}

ReservationState reservationStateFromToken(std::string_view token) noexcept {
  return valueOf(token, kReservationStateTokens, ReservationState::None);
}

bool isTerminalState(ReservationState state) noexcept {
  switch (state) {
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

bool isLiveState(ReservationState state) noexcept {
  switch (state) {
    case ReservationState::Reserved:
    case ReservationState::Activating:
    case ReservationState::Active:
    case ReservationState::Deactivating:
    case ReservationState::Renewing:
    case ReservationState::Releasing:
    case ReservationState::Reclaiming:
      return true;
    default:
      return false;
  }
}

bool isTransientState(ReservationState state) noexcept {
  switch (state) {
    case ReservationState::Requested:
    case ReservationState::Evaluated:
    case ReservationState::Committing:
    case ReservationState::Activating:
    case ReservationState::Deactivating:
    case ReservationState::Renewing:
    case ReservationState::Releasing:
    case ReservationState::Reclaiming:
    case ReservationState::Recovering:
      return true;
    default:
      return false;
  }
}

bool isLegalTransition(ReservationState from, ReservationState to) noexcept {
  return transitionAllowed(from, to);
}

std::string_view illegalTransitionReason(ReservationState from, ReservationState to) noexcept {
  const std::size_t fromIndex = static_cast<std::size_t>(from);
  const std::size_t toIndex = static_cast<std::size_t>(to);
  if (fromIndex >= kReservationStateCount || toIndex >= kReservationStateCount)
    return "a reservation state outside the defined states has no legal transitions";
  const std::size_t index = fromIndex * kReservationStateCount + toIndex;
  const TransitionReasonSpan span = kTransitionReasons.spans[index];
  if (span.length == 0) return {};
  return std::string_view(kTransitionReasons.text.data() + span.offset, span.length);
}

// ---------------------------------------------------------------------------
// Status codes
// ---------------------------------------------------------------------------

namespace {

constexpr TokenEntry<StatusCode> kStatusCodeTokens[] = {
    {StatusCode::Ok, "ok"},
    {StatusCode::InvalidArgument, "invalid-argument"},
    {StatusCode::NotFound, "not-found"},
    {StatusCode::Duplicate, "duplicate"},
    {StatusCode::LimitExceeded, "limit-exceeded"},
    {StatusCode::Unsupported, "unsupported"},
    {StatusCode::Unknown, "unknown"},
    {StatusCode::StaleGeneration, "stale-generation"},
    {StatusCode::StaleIncarnation, "stale-incarnation"},
    {StatusCode::StaleEpoch, "stale-epoch"},
    {StatusCode::StaleAuthority, "stale-authority"},
    {StatusCode::IllegalTransition, "illegal-transition"},
    {StatusCode::Conflict, "conflict"},
    {StatusCode::Excluded, "excluded"},
    {StatusCode::PersistenceFailure, "persistence-failure"},
    {StatusCode::Corruption, "corruption"},
    {StatusCode::IoError, "io-error"},
    {StatusCode::NotPermitted, "not-permitted"},
    {StatusCode::Overflow, "overflow"},
    {StatusCode::Refused, "refused"},
    {StatusCode::Unavailable, "unavailable"},
};

}  // namespace

std::string_view toToken(StatusCode code) noexcept {
  return tokenOf(code, kStatusCodeTokens, "unknown");
}

StatusCode statusCodeFromToken(std::string_view token) noexcept {
  return valueOf(token, kStatusCodeTokens, StatusCode::Unknown);
}

bool isStale(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::StaleGeneration:
    case StatusCode::StaleIncarnation:
    case StatusCode::StaleEpoch:
    case StatusCode::StaleAuthority:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

Instant systemNow() noexcept {
  const std::chrono::nanoseconds sinceEpoch =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch());
  return Instant::fromNanos(static_cast<std::int64_t>(sinceEpoch.count()));
}

// ---------------------------------------------------------------------------
// Authority checks
// ---------------------------------------------------------------------------

Status checkEligibilityAuthority(const AuthorityState& state, const EligibilityAuthority& token) {
  if (!token.valid())
    return fail(StatusCode::InvalidArgument, "eligibility authority token is required");
  const Status fence = checkFenceFields("eligibility authority", state.fence, token.fence);
  if (!fence.ok()) return fence;
  return checkAuthorityGeneration("eligibility", token.generation.raw(),
                                  state.eligibilityGeneration.raw());
}

Status checkReservationAuthority(const AuthorityState& state, const ReservationAuthority& token) {
  if (!token.valid())
    return fail(StatusCode::InvalidArgument, "reservation authority token is required");
  const Status fence = checkFenceFields("reservation authority", state.fence, token.fence);
  if (!fence.ok()) return fence;
  return checkAuthorityGeneration("reservation", token.generation.raw(),
                                  state.reservationGeneration.raw());
}

Status checkActivationAuthority(const AuthorityState& state, const ActivationAuthority& token) {
  if (!token.valid())
    return fail(StatusCode::InvalidArgument, "activation authority token is required");
  const Status fence = checkFenceFields("activation authority", state.fence, token.fence);
  if (!fence.ok()) return fence;
  return checkAuthorityGeneration("activation", token.generation.raw(),
                                  state.activationGeneration.raw());
}

Status checkReleaseAuthority(const AuthorityState& state, const ReleaseAuthority& token) {
  if (!token.valid())
    return fail(StatusCode::InvalidArgument, "release authority token is required");
  const Status fence = checkFenceFields("release authority", state.fence, token.fence);
  if (!fence.ok()) return fence;
  return checkAuthorityGeneration("release", token.generation.raw(),
                                  state.releaseGeneration.raw());
}

Status checkFence(const AuthorityState& state, const ControllerFence& fence) {
  if (!fence.valid())
    return fail(StatusCode::InvalidArgument, "controller fence is required");
  return checkFenceFields("controller fence", state.fence, fence);
}

// ---------------------------------------------------------------------------
// Candidate eligibility
// ---------------------------------------------------------------------------

namespace {

constexpr TokenEntry<CandidateEligibility> kCandidateEligibilityTokens[] = {
    {CandidateEligibility::Eligible, "eligible"},
    {CandidateEligibility::IneligibleUnsupported, "ineligible-unsupported"},
    {CandidateEligibility::IneligibleUnknownCapability, "ineligible-unknown-capability"},
    {CandidateEligibility::IneligibleStaleCapability, "ineligible-stale-capability"},
    {CandidateEligibility::IneligibleStaleDomain, "ineligible-stale-domain"},
    {CandidateEligibility::IneligibleOutsideWindow, "ineligible-outside-window"},
    {CandidateEligibility::IneligibleOutsideTunable, "ineligible-outside-tunable"},
    {CandidateEligibility::IneligibleExcluded, "ineligible-excluded"},
    {CandidateEligibility::IneligibleConflict, "ineligible-conflict"},
    {CandidateEligibility::IneligibleContiguity, "ineligible-contiguity"},
    {CandidateEligibility::IneligibleConversionRequired, "ineligible-conversion-required"},
    {CandidateEligibility::IneligibleChannelWidth, "ineligible-channel-width"},
    {CandidateEligibility::IneligibleGuardBand, "ineligible-guard-band"},
    {CandidateEligibility::IneligibleNoGrid, "ineligible-no-grid"},
    {CandidateEligibility::IneligibleUnknown, "ineligible-unknown"},
};

}  // namespace

std::string_view toToken(CandidateEligibility eligibility) noexcept {
  return tokenOf(eligibility, kCandidateEligibilityTokens, "ineligible-unknown");
}

CandidateEligibility candidateEligibilityFromToken(std::string_view token) noexcept {
  return valueOf(token, kCandidateEligibilityTokens, CandidateEligibility::IneligibleUnknown);
}

bool isEligible(CandidateEligibility eligibility) noexcept {
  return eligibility == CandidateEligibility::Eligible;
}

// ---------------------------------------------------------------------------
// Allocation outcomes
// ---------------------------------------------------------------------------

namespace {

constexpr TokenEntry<AllocationOutcome> kAllocationOutcomeTokens[] = {
    {AllocationOutcome::Allocated, "allocated"},
    {AllocationOutcome::RefusedUnsupported, "refused-unsupported"},
    {AllocationOutcome::RefusedUnknownCapability, "refused-unknown-capability"},
    {AllocationOutcome::RefusedNoCapacity, "refused-no-capacity"},
    {AllocationOutcome::RefusedConflict, "refused-conflict"},
    {AllocationOutcome::RefusedContiguity, "refused-contiguity"},
    {AllocationOutcome::RefusedContinuity, "refused-continuity"},
    {AllocationOutcome::RefusedConversion, "refused-conversion"},
    {AllocationOutcome::RefusedConstraint, "refused-constraint"},
    {AllocationOutcome::RefusedExclusion, "refused-exclusion"},
    {AllocationOutcome::RefusedStaleGeneration, "refused-stale-generation"},
    {AllocationOutcome::RefusedStaleIncarnation, "refused-stale-incarnation"},
    {AllocationOutcome::RefusedStaleEpoch, "refused-stale-epoch"},
    {AllocationOutcome::RefusedStaleAuthority, "refused-stale-authority"},
    {AllocationOutcome::RefusedStaleCapability, "refused-stale-capability"},
    {AllocationOutcome::RefusedDuplicate, "refused-duplicate"},
    {AllocationOutcome::RefusedInvalidRequest, "refused-invalid-request"},
    {AllocationOutcome::RefusedLeaseExpired, "refused-lease-expired"},
    {AllocationOutcome::RefusedLimitExceeded, "refused-limit-exceeded"},
    {AllocationOutcome::RefusedUnknownDomain, "refused-unknown-domain"},
    {AllocationOutcome::RefusedNotPermitted, "refused-not-permitted"},
    {AllocationOutcome::RefusedChannelWidth, "refused-channel-width"},
    {AllocationOutcome::Unknown, "unknown"},
};

}  // namespace

std::string_view toToken(AllocationOutcome outcome) noexcept {
  return tokenOf(outcome, kAllocationOutcomeTokens, "unknown");
}

AllocationOutcome allocationOutcomeFromToken(std::string_view token) noexcept {
  return valueOf(token, kAllocationOutcomeTokens, AllocationOutcome::Unknown);
}

bool isRefusal(AllocationOutcome outcome) noexcept {
  return outcome != AllocationOutcome::Allocated && outcome != AllocationOutcome::Unknown;
}

// ---------------------------------------------------------------------------
// Audit kinds
// ---------------------------------------------------------------------------

namespace {

constexpr TokenEntry<AuditKind> kAuditKindTokens[] = {
    {AuditKind::None, "none"},
    {AuditKind::RuntimeOpened, "runtime-opened"},
    {AuditKind::RecoveryPerformed, "recovery-performed"},
    {AuditKind::GridRegistered, "grid-registered"},
    {AuditKind::SpanRegistered, "span-registered"},
    {AuditKind::PortRegistered, "port-registered"},
    {AuditKind::DomainRegistered, "domain-registered"},
    {AuditKind::ExclusionDomainRegistered, "exclusion-domain-registered"},
    {AuditKind::CapabilityPublished, "capability-published"},
    {AuditKind::RequestEvaluated, "request-evaluated"},
    {AuditKind::AllocationCommitted, "allocation-committed"},
    {AuditKind::AllocationRefused, "allocation-refused"},
    {AuditKind::ReservationRenewed, "reservation-renewed"},
    {AuditKind::ReservationActivated, "reservation-activated"},
    {AuditKind::ReservationDeactivated, "reservation-deactivated"},
    {AuditKind::ReservationReleased, "reservation-released"},
    {AuditKind::ReservationExpired, "reservation-expired"},
    {AuditKind::ReservationReclaimed, "reservation-reclaimed"},
    {AuditKind::ReservationSuperseded, "reservation-superseded"},
    {AuditKind::AuthorityAdvanced, "authority-advanced"},
    {AuditKind::PersistenceFlushed, "persistence-flushed"},
    {AuditKind::PersistenceFailed, "persistence-failed"},
    {AuditKind::CorruptionDetected, "corruption-detected"},
    {AuditKind::ReplayRejected, "replay-rejected"},
    {AuditKind::StateReset, "state-reset"},
};

}  // namespace

std::string_view toToken(AuditKind kind) noexcept {
  return tokenOf(kind, kAuditKindTokens, "none");
}

AuditKind auditKindFromToken(std::string_view token) noexcept {
  return valueOf(token, kAuditKindTokens, AuditKind::None);
}

}  // namespace wavelength_fabric
