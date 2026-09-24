// Independent consumer proof for an installed Wavelength Fabric package.
//
// This translation unit is compiled by consumer/CMakeLists.txt against the
// installed WavelengthFabric package (find_package(WavelengthFabric 1.0
// REQUIRED CONFIG)). It includes installed public headers only and never
// references the repository source tree. The process exits 0 only when every
// assertion below held.

#include <wavelength_fabric/decision.hpp>
#include <wavelength_fabric/protocol.hpp>
#include <wavelength_fabric/runtime.hpp>
#include <wavelength_fabric/text.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using namespace wavelength_fabric;

// ---------------------------------------------------------------------------
// Assertion harness
// ---------------------------------------------------------------------------

class Proof {
 public:
  void section(const std::string& title) { std::cout << "\n== " << title << " ==\n"; }

  bool expect(bool condition, const std::string& what) {
    ++checks_;
    if (condition) {
      std::cout << "  ok    " << what << '\n';
      return true;
    }
    ++failures_;
    std::cout << "  FAIL  " << what << '\n';
    return false;
  }

  [[nodiscard]] int checks() const noexcept { return checks_; }
  [[nodiscard]] int failures() const noexcept { return failures_; }

 private:
  int checks_{0};
  int failures_{0};
};

// ---------------------------------------------------------------------------
// Typed tokens of the public enums
// ---------------------------------------------------------------------------

[[nodiscard]] std::string token(StatusCode code) { return std::string(toToken(code)); }

[[nodiscard]] std::string token(GridKind kind) { return std::string(toToken(kind)); }

[[nodiscard]] std::string token(SpectrumSupport support) { return std::string(toToken(support)); }

[[nodiscard]] std::string token(CandidateEligibility eligibility) {
  return std::string(toToken(eligibility));
}

[[nodiscard]] std::string token(AllocationOutcome outcome) {
  return std::string(toToken(outcome));
}

[[nodiscard]] std::string token(ReservationState state) { return std::string(toToken(state)); }

[[nodiscard]] std::string token(AuditKind kind) { return std::string(toToken(kind)); }

[[nodiscard]] std::string describeStatus(const Status& status) {
  std::string out = token(status.code);
  if (!status.message.empty()) {
    out += " (";
    out += status.message;
    out += ')';
  }
  return out;
}

void expectStatus(Proof& proof, const Status& status, const std::string& what) {
  proof.expect(status.ok(), what + " [status=" + describeStatus(status) + "]");
}

// ---------------------------------------------------------------------------
// Fixtures built from the public model types
//
// An abstract 96-slot 50 GHz fixed grid and an abstract 384-slot 12.5 GHz flex
// grid, both anchored at 191.3 THz. These are data-model values; nothing here
// asserts physical optical behaviour.
// ---------------------------------------------------------------------------

constexpr std::int64_t kAnchorMhz = 191'300'000;
constexpr std::int64_t kFixedSlotWidthMhz = 50'000;
constexpr std::uint32_t kFixedSlotCount = 96;
constexpr std::int64_t kFlexSlotWidthMhz = 12'500;
constexpr std::uint32_t kFlexSlotCount = 384;
constexpr std::uint32_t kFlexMaxSlotsPerChannel = 32;
constexpr std::int64_t kGridEndMhz =
    kAnchorMhz + kFixedSlotWidthMhz * static_cast<std::int64_t>(kFixedSlotCount);

[[nodiscard]] ChannelGrid makeFixedGrid() {
  ChannelGrid grid;
  grid.id = ChannelGridId(1u);
  grid.generation = GridGeneration(1u);
  grid.kind = GridKind::Fixed;
  grid.anchorMhz = kAnchorMhz;
  grid.slotWidthMhz = kFixedSlotWidthMhz;
  grid.slotCount = kFixedSlotCount;
  grid.minSlotsPerChannel = 1u;
  grid.maxSlotsPerChannel = 1u;
  grid.label = "consumer-fixed";
  return grid;
}

[[nodiscard]] ChannelGrid makeFlexGrid() {
  ChannelGrid grid;
  grid.id = ChannelGridId(2u);
  grid.generation = GridGeneration(1u);
  grid.kind = GridKind::Flex;
  grid.anchorMhz = kAnchorMhz;
  grid.slotWidthMhz = kFlexSlotWidthMhz;
  grid.slotCount = kFlexSlotCount;
  grid.minSlotsPerChannel = 1u;
  grid.maxSlotsPerChannel = kFlexMaxSlotsPerChannel;
  grid.label = "consumer-flex";
  return grid;
}

[[nodiscard]] SpectrumDomain makeDomain(SpectrumDomainId id, ChannelGridId grid,
                                        ResourceClass klass) {
  SpectrumDomain domain;
  domain.id = id;
  domain.generation = SpectrumDomainGeneration(1u);
  domain.klass = klass;
  domain.grid = grid;
  domain.gridGeneration = GridGeneration(1u);
  domain.requiresContiguity = true;
  domain.label = "consumer-domain-" + std::to_string(id.raw());
  return domain;
}

[[nodiscard]] SpectrumCapability makeCapability(const SpectrumDomain& domain, std::uint32_t slots,
                                                const ControllerFence& fence, Instant publishedAt) {
  SpectrumCapability capability;
  capability.domain = domain.id;
  capability.domainGeneration = domain.generation;
  capability.grid = domain.grid;
  capability.gridGeneration = domain.gridGeneration;
  capability.support = SpectrumSupport::Supported;
  capability.firstAllocatableSlot = 0u;
  capability.allocatableSlots = slots;
  capability.minTunableMhz = kAnchorMhz;
  capability.maxTunableMhz = kGridEndMhz;
  capability.contiguityEnforced = true;
  capability.conversionSupported = false;
  capability.presenceEvidence.present = true;
  capability.presenceEvidence.digest = 0x5EED0001ull;
  capability.presenceEvidence.source = "wavelength-fabric-consumer";
  capability.generation = CapabilityGeneration(1u);
  capability.publisher = ControllerId(1u);
  capability.fence = fence;
  capability.publishedAt = publishedAt;
  capability.detail = "capability published by the installed-package consumer";
  return capability;
}

struct Authorities {
  EligibilityAuthority eligibility;
  ReservationAuthority reservation;
  ActivationAuthority activation;
  ReleaseAuthority release;
};

// A freshly constructed runtime recognises authority generation 1 in all four
// separated authority domains, and every token must carry the live fence.
[[nodiscard]] Authorities authoritiesOf(const SpectrumRuntime& runtime) {
  const ControllerFence fence = runtime.fence();
  Authorities authorities;
  authorities.eligibility.generation = EligibilityAuthorityGeneration(1u);
  authorities.eligibility.fence = fence;
  authorities.reservation.generation = ReservationAuthorityGeneration(1u);
  authorities.reservation.fence = fence;
  authorities.activation.generation = ActivationAuthorityGeneration(1u);
  authorities.activation.fence = fence;
  authorities.release.generation = ReleaseAuthorityGeneration(1u);
  authorities.release.fence = fence;
  return authorities;
}

[[nodiscard]] SpectrumRequest makeRequest(std::uint64_t id, const SpectrumDomain& domain,
                                          ChannelGridId grid, std::uint32_t slots,
                                          Instant requestedAt, Duration lease,
                                          const Authorities& authorities, std::uint32_t maxRenewals,
                                          const std::vector<FrequencyRange>& windows) {
  SpectrumRequest request;
  request.requestId = AllocationRequestId(id);
  request.requestGeneration = AllocationRequestGeneration(1u);
  request.owner = OwnerId(7u);
  request.ownerGeneration = OwnerGeneration(1u);
  request.domains = {domain.id};
  request.domainGenerations = {domain.generation};
  request.grid = grid;
  request.gridGeneration = GridGeneration(1u);
  request.slots = slots;
  request.contiguity =
      slots > 1u ? ContiguityRequirement::Required : ContiguityRequirement::Unspecified;
  request.continuity = ContinuityRequirement::Unspecified;
  request.frequencyWindows = windows;
  request.guardBandMhz = 0;
  request.leaseDuration = lease;
  request.maxRenewals = maxRenewals;
  request.requestedAt = requestedAt;
  request.notBefore = Instant{};
  request.policyGeneration = PolicyGeneration(1u);
  request.priorityGeneration = PriorityGeneration(1u);
  request.eligibilityAuthority = authorities.eligibility;
  request.reservationAuthority = authorities.reservation;
  return request;
}

// Frequency span of the first fixed-grid slot: [191'300'000, 191'350'000) MHz.
[[nodiscard]] FrequencyRange firstFixedSlotFrequency() {
  return FrequencyRange{kAnchorMhz, kAnchorMhz + kFixedSlotWidthMhz};
}

// Unique scratch state file under the operating system temporary directory.
[[nodiscard]] std::filesystem::path temporaryStatePath() {
  std::error_code error;
  const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
  const std::filesystem::path base = error ? std::filesystem::path(".") : directory;
  std::random_device device;
  const std::uint64_t entropy =
      (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
  return base / ("wavelength_fabric_consumer_" + std::to_string(entropy) + ".state");
}

// ---------------------------------------------------------------------------
// Core proof: registration, enumeration, allocation, refusal, lifecycle,
// reclamation and durable save/recover.
// ---------------------------------------------------------------------------

void proveCore(Proof& proof, const std::string& statePath) {
  proof.section("core: registration, enumeration, allocation, refusal, lifecycle, reclaim");

  const Instant requestedAt = Instant::fromSeconds(1'800'000'000);
  const Instant lifecycleAt = requestedAt + Duration::seconds(1);
  const Duration lease = Duration::minutes(30);

  RuntimeConfig config;
  config.statePath = statePath;
  config.controller = ControllerId(1u);
  config.durableCommits = false;
  config.fsyncState = true;

  SpectrumRuntime runtime(config);
  proof.expect(runtime.fence().valid(),
               "runtime opened at epoch " + std::to_string(runtime.epoch().raw()) +
                   " incarnation " + std::to_string(runtime.incarnation().raw()));

  const ChannelGrid fixed = makeFixedGrid();
  const ChannelGrid flex = makeFlexGrid();
  expectStatus(proof, runtime.registerGrid(fixed), "registerGrid(" + describeGrid(fixed) + ")");
  expectStatus(proof, runtime.registerGrid(flex),
               "registerGrid(flex, " + std::to_string(flex.slotCount) + " slots, " +
                   std::to_string(flex.slotWidthMhz) + " MHz)");

  const SpectrumDomain first = makeDomain(SpectrumDomainId(1u), fixed.id, ResourceClass::FiberSpan);
  const SpectrumDomain sibling =
      makeDomain(SpectrumDomainId(2u), fixed.id, ResourceClass::FiberSpan);
  const SpectrumDomain flexDomain =
      makeDomain(SpectrumDomainId(3u), flex.id, ResourceClass::MediaChannel);
  expectStatus(proof, runtime.registerDomain(first), "registerDomain(domain:1 on grid:1)");
  expectStatus(proof, runtime.registerDomain(sibling), "registerDomain(domain:2 on grid:1)");
  expectStatus(proof, runtime.registerDomain(flexDomain), "registerDomain(domain:3 on grid:2)");

  ExclusionDomain exclusionDomain;
  exclusionDomain.id = ExclusionDomainId(1u);
  exclusionDomain.generation = ExclusionDomainGeneration(1u);
  exclusionDomain.members = {SpectrumDomainId(1u), SpectrumDomainId(2u)};
  exclusionDomain.guardBandMhz = 0;
  exclusionDomain.label = "consumer-exclusion";
  expectStatus(proof, runtime.registerExclusionDomain(exclusionDomain),
               "registerExclusionDomain(members={domain:1, domain:2})");

  expectStatus(proof,
               runtime.publishCapability(
                   makeCapability(first, fixed.slotCount, runtime.fence(), requestedAt)),
               "publishCapability(domain:1, " + token(SpectrumSupport::Supported) + ")");
  expectStatus(proof,
               runtime.publishCapability(
                   makeCapability(sibling, fixed.slotCount, runtime.fence(), requestedAt)),
               "publishCapability(domain:2, " + token(SpectrumSupport::Supported) + ")");
  expectStatus(proof,
               runtime.publishCapability(
                   makeCapability(flexDomain, flex.slotCount, runtime.fence(), requestedAt)),
               "publishCapability(domain:3, " + token(SpectrumSupport::Supported) + ")");

  proof.expect(runtime.grids().size() == 2u && runtime.domains().size() == 3u &&
                   runtime.exclusionDomains().size() == 1u,
               "registries report 2 grid(s), 3 domain(s) and 1 exclusion domain");
  proof.expect(runtime.grid(fixed.id).has_value() && runtime.domain(flexDomain.id).has_value() &&
                   runtime.capability(flexDomain.id).has_value(),
               "grid, domain and capability queries answer for the registered identities");

  // ---- enumeration --------------------------------------------------------
  const Authorities authorities = authoritiesOf(runtime);
  const SpectrumRequest request =
      makeRequest(1u, first, fixed.id, 1u, requestedAt, lease, authorities, 4u, {});
  const CandidateSet candidates = runtime.enumerateCandidates(request);
  expectStatus(proof, candidates.status, "enumerateCandidates(request:1)");
  proof.expect(candidates.eligibleCount >= 1u,
               "enumeration found " + std::to_string(candidates.eligibleCount) +
                   " eligible candidate(s) out of " + std::to_string(candidates.candidates.size()));
  const SpectrumCandidate* best = nullptr;
  for (const SpectrumCandidate& candidate : candidates.candidates) {
    if (isEligible(candidate.eligibility)) {
      best = &candidate;
      break;
    }
  }
  if (proof.expect(best != nullptr, "the ordered candidate list contains an eligible candidate")) {
    proof.expect(best->slots.first == 0u && best->slots.count == 1u,
                 "the first eligible candidate is slot 0 with eligibility=" +
                     token(best->eligibility));
  }
  proof.expect(runtime.reservations().empty(), "enumeration committed no reservation");

  // ---- allocation ---------------------------------------------------------
  const AllocationDecision decision = runtime.allocate(request);
  if (!proof.expect(decision.allocated(), "allocate(request:1) committed [outcome=" +
                                               token(decision.outcome) + "]")) {
    return;
  }
  std::cout << "  decision: " << decision.describe() << '\n';

  const ReservationId committed = decision.reservation;
  const auto stored = runtime.reservation(committed);
  if (!proof.expect(stored.has_value(),
                    "reservation " + typedToken("reservation", committed) + " is stored")) {
    return;
  }
  proof.expect(stored->state == ReservationState::Reserved,
               "the committed state is " + token(stored->state));
  proof.expect(stored->slots.first == 0u && stored->slots.count == 1u,
               "the committed slot range is [0, 1)");
  proof.expect(stored->frequency.lowMhz == kAnchorMhz &&
                   stored->frequency.highMhz == kAnchorMhz + kFixedSlotWidthMhz,
               "the committed frequency is [" + std::to_string(stored->frequency.lowMhz) + ", " +
                   std::to_string(stored->frequency.highMhz) + ") MHz");
  proof.expect(stored->lease.expiresAt == requestedAt + lease,
               "the lease expires at requestedAt + leaseDuration");
  proof.expect(stored->domains.size() == 1u && stored->domains.front() == first.id,
               "the reservation owns spectrum on domain:1 only");

  // ---- a conflicting request on the same domain ---------------------------
  const SpectrumRequest conflictingRequest =
      makeRequest(2u, first, fixed.id, 1u, requestedAt, lease, authorities, 4u,
                  {firstFixedSlotFrequency()});
  const AllocationDecision conflicting = runtime.allocate(conflictingRequest);
  proof.expect(!conflicting.allocated(),
               "a conflicting request on domain:1 is refused [outcome=" +
                   token(conflicting.outcome) + "]");
  proof.expect(conflicting.outcome == AllocationOutcome::RefusedConflict &&
                   conflicting.status.code == StatusCode::Conflict,
               "the refusal is typed as " + token(conflicting.outcome) + " / " +
                   token(conflicting.status.code));
  proof.expect(std::find(conflicting.explanation.conflicts.begin(),
                         conflicting.explanation.conflicts.end(),
                         committed) != conflicting.explanation.conflicts.end(),
               "the refusal names " + typedToken("reservation", committed) +
                   " as the blocking owner");
  const auto afterConflict = runtime.reservation(committed);
  proof.expect(afterConflict.has_value() &&
                   afterConflict->state == ReservationState::Reserved,
               "the refused request left the owner untouched");

  // ---- a request on the exclusion sibling ---------------------------------
  const SpectrumRequest siblingRequest =
      makeRequest(3u, sibling, fixed.id, 1u, requestedAt, lease, authorities, 4u,
                  {firstFixedSlotFrequency()});
  const AllocationDecision siblingDecision = runtime.allocate(siblingRequest);
  proof.expect(!siblingDecision.allocated(),
               "a request on the exclusion sibling domain:2 is refused [outcome=" +
                   token(siblingDecision.outcome) + "]");
  proof.expect(siblingDecision.outcome == AllocationOutcome::RefusedExclusion &&
                   siblingDecision.status.code == StatusCode::Excluded,
               "the refusal is typed as " + token(siblingDecision.outcome) + " / " +
                   token(siblingDecision.status.code));

  // ---- lifecycle ----------------------------------------------------------
  expectStatus(proof,
               runtime.activate(committed, decision.generation, authorities.activation,
                                lifecycleAt),
               "activate(reservation, generation 1)");
  const auto active = runtime.reservation(committed);
  proof.expect(active.has_value() && active->state == ReservationState::Active,
               "the reservation is now " +
                   (active.has_value() ? token(active->state) : std::string("missing")));

  expectStatus(proof,
               runtime.renew(committed, decision.generation, Duration::minutes(10),
                             authorities.reservation, lifecycleAt, 4u),
               "renew(reservation, generation 1, +10min)");
  const auto renewed = runtime.reservation(committed);
  if (!proof.expect(renewed.has_value(), "the renewed reservation is stored")) {
    return;
  }
  proof.expect(renewed->generation.raw() == decision.generation.raw() + 1u,
               "renewal advanced the reservation generation to " +
                   std::to_string(renewed->generation.raw()));
  proof.expect(renewed->state == ReservationState::Active,
               "renewal preserved the state " + token(renewed->state));
  proof.expect(renewed->lease.renewalCount == 1u, "renewalCount is 1");
  proof.expect(renewed->lease.expiresAt == requestedAt + lease + Duration::minutes(10),
               "the lease now expires 40 minutes after the request instant");
  const ReservationGeneration renewedGeneration = renewed->generation;

  expectStatus(proof, runtime.deactivate(committed, renewedGeneration, authorities.activation,
                                         lifecycleAt),
               "deactivate(reservation, generation 2)");
  const auto deactivated = runtime.reservation(committed);
  proof.expect(deactivated.has_value() && deactivated->state == ReservationState::Reserved,
               "deactivation returned the reservation to " +
                   (deactivated.has_value() ? token(deactivated->state) : std::string("missing")));

  expectStatus(proof,
               runtime.release(committed, renewedGeneration, authorities.release, lifecycleAt),
               "release(reservation, generation 2)");
  const auto released = runtime.reservation(committed);
  proof.expect(released.has_value() && released->state == ReservationState::Released,
               "release moved the reservation to " +
                   (released.has_value() ? token(released->state) : std::string("missing")));

  // ---- the released spectrum is allocatable again -------------------------
  const SpectrumRequest reallocation =
      makeRequest(4u, first, fixed.id, 1u, requestedAt, lease, authorities, 0u, {});
  const AllocationDecision reallocated = runtime.allocate(reallocation);
  proof.expect(reallocated.allocated() && reallocated.explanation.selected.slots.first == 0u,
               "a fresh request commits at slot 0 again [outcome=" +
                   token(reallocated.outcome) + "]");

  const auto usage = runtime.usage(first.id, lifecycleAt);
  if (proof.expect(usage.has_value(), "usage(domain:1) is available")) {
    proof.expect(usage->liveSlots == 1u && usage->freeSlots == kFixedSlotCount - 1u &&
                     usage->reservedSlots == 1u,
                 "usage reports liveSlots=" + std::to_string(usage->liveSlots) +
                     " freeSlots=" + std::to_string(usage->freeSlots) + " reservedSlots=" +
                     std::to_string(usage->reservedSlots));
    proof.expect(usage->releasedReservations == 1u && usage->reclaimedReservations == 0u,
                 "usage accounting records the released reservation exactly once");
  }

  // ---- reclamation of a lapsed lease --------------------------------------
  const SpectrumRequest shortLease =
      makeRequest(5u, flexDomain, flex.id, 1u, requestedAt, Duration::minutes(1), authorities, 0u,
                  {});
  const AllocationDecision shortLived = runtime.allocate(shortLease);
  if (!proof.expect(shortLived.allocated(), "allocate a one-minute lease on domain:3 [outcome=" +
                                                token(shortLived.outcome) + "]")) {
    return;
  }

  const Instant afterExpiry = requestedAt + Duration::minutes(5);
  const ReclaimReport reclaim = runtime.reclaimExpired(afterExpiry);
  expectStatus(proof, reclaim.status, "reclaimExpired(requestedAt + 5min)");
  proof.expect(std::find(reclaim.reclaimed.begin(), reclaim.reclaimed.end(),
                         shortLived.reservation) != reclaim.reclaimed.end(),
               "the sweep reclaimed " + typedToken("reservation", shortLived.reservation));
  const auto reclaimed = runtime.reservation(shortLived.reservation);
  proof.expect(reclaimed.has_value() && reclaimed->state == ReservationState::Reclaimed,
               "the lapsed reservation is " +
                   (reclaimed.has_value() ? token(reclaimed->state) : std::string("missing")));
  const auto surviving = runtime.reservation(reallocated.reservation);
  proof.expect(surviving.has_value() && surviving->state == ReservationState::Reserved,
               "the sweep left the unexpired reservation in state " +
                   (surviving.has_value() ? token(surviving->state) : std::string("missing")));

  const SpectrumRequest wideRequest =
      makeRequest(6u, flexDomain, flex.id, 2u, requestedAt + Duration::minutes(6), lease,
                  authorities, 0u, {});
  const AllocationDecision wide = runtime.allocate(wideRequest);
  proof.expect(wide.allocated() && wide.explanation.selected.slots.count == 2u,
               "the reclaimed spectrum carries a two-slot channel [outcome=" +
                   token(wide.outcome) + "]");

  const std::vector<AuditRecord> trail = runtime.audit(AuditSequence(0u), runtime.auditSize());
  proof.expect(trail.size() == runtime.auditSize(),
               "the audit trail exposes " + std::to_string(trail.size()) + " record(s), last=" +
                   (trail.empty() ? token(AuditKind::None) : token(trail.back().kind)));

  // ---- durable save and recover ------------------------------------------
  expectStatus(proof, runtime.save(), "save() wrote " + statePath);
  std::error_code fileError;
  proof.expect(std::filesystem::exists(std::filesystem::path(statePath), fileError) && !fileError,
               "the state file exists on disk");

  SpectrumRuntime recovered(config);
  const RecoveryReport recovery = recovered.recover();
  expectStatus(proof, recovery.status, "recover() read " + statePath);
  proof.expect(recovery.recovered, "recovery reports recovered=true");
  proof.expect(recovery.gridsRestored == 2u && recovery.domainsRestored == 3u &&
                   recovery.exclusionDomainsRestored == 1u && recovery.capabilitiesRestored == 3u,
               "recovery restored " + std::to_string(recovery.gridsRestored) + " grid(s), " +
                   std::to_string(recovery.domainsRestored) + " domain(s), " +
                   std::to_string(recovery.exclusionDomainsRestored) +
                   " exclusion domain(s) and " + std::to_string(recovery.capabilitiesRestored) +
                   " capability(ies)");
  proof.expect(recovery.reservationsRestored == 4u,
               "recovery restored " + std::to_string(recovery.reservationsRestored) +
                   " reservation record(s)");
  proof.expect(recovered.fence().epoch.raw() == runtime.epoch().raw() + 1u,
               "recovery advanced the controller epoch to " +
                   std::to_string(recovered.fence().epoch.raw()));

  const auto recoveredGrid = recovered.grid(fixed.id);
  proof.expect(recoveredGrid.has_value() && recoveredGrid->slotCount == kFixedSlotCount &&
                   recoveredGrid->kind == GridKind::Fixed,
               "the recovered runtime serves " +
                   (recoveredGrid.has_value() ? describeGrid(*recoveredGrid)
                                              : std::string("no grid")));
  const auto recoveredReservation = recovered.reservation(reallocated.reservation);
  proof.expect(recoveredReservation.has_value() &&
                   recoveredReservation->state == ReservationState::Reserved,
               "the live reservation came back as " +
                   (recoveredReservation.has_value() ? token(recoveredReservation->state)
                                                     : std::string("missing")));

  const Authorities recoveredAuthorities = authoritiesOf(recovered);
  const SpectrumRequest afterRecovery =
      makeRequest(7u, flexDomain, flex.id, 1u, requestedAt + Duration::minutes(10), lease,
                  recoveredAuthorities, 0u, {});
  const AllocationDecision afterRecoveryDecision = recovered.allocate(afterRecovery);
  proof.expect(afterRecoveryDecision.allocated() &&
                   afterRecoveryDecision.explanation.selected.slots.first == 2u,
               "the recovered runtime commits a new allocation at slot 2 [outcome=" +
                   token(afterRecoveryDecision.outcome) + "]");

  std::cout << "  stats: " << describeRuntime(runtime.stats()) << '\n';

  std::error_code cleanupError;
  std::filesystem::remove(std::filesystem::path(statePath), cleanupError);
  std::filesystem::remove(std::filesystem::path(statePath + ".tmp"), cleanupError);
  proof.expect(!std::filesystem::exists(std::filesystem::path(statePath)),
               "the scratch state file was removed");
}

// ---------------------------------------------------------------------------
// Transport proof: a real SpectrumServer on 127.0.0.1 with port 0 and a
// SpectrumClient speaking the framed protocol over loopback TCP.
// ---------------------------------------------------------------------------

void proveTransport(Proof& proof) {
  proof.section("transport: SpectrumServer on 127.0.0.1:0 and SpectrumClient over loopback TCP");

  constexpr int kMaxConnectAttempts = 4096;
  const Instant requestedAt = Instant::fromSeconds(1'800'000'100);

  SpectrumRuntime serverRuntime;
  const ChannelGrid grid = makeFixedGrid();
  const SpectrumDomain domain = makeDomain(SpectrumDomainId(1u), grid.id, ResourceClass::FiberSpan);
  expectStatus(proof, serverRuntime.registerGrid(grid), "server runtime registered grid:1");
  expectStatus(proof, serverRuntime.registerDomain(domain), "server runtime registered domain:1");
  expectStatus(proof,
               serverRuntime.publishCapability(
                   makeCapability(domain, grid.slotCount, serverRuntime.fence(), requestedAt)),
               "server runtime published the capability of domain:1");

  ServerConfig serverConfig;
  serverConfig.address = "127.0.0.1";
  serverConfig.port = 0;  // the operating system selects a free loopback port
  serverConfig.workerThreads = 2u;
  serverConfig.maxConnections = 8u;
  serverConfig.backlog = 4u;
  serverConfig.acceptPollMillis = 10u;
  serverConfig.allowShutdownOp = false;

  SpectrumServer server(serverRuntime, serverConfig);
  const Status started = server.start();
  if (!proof.expect(started.ok(), "SpectrumServer::start() bound 127.0.0.1:0 [status=" +
                                      describeStatus(started) + "]")) {
    return;
  }
  const std::uint16_t boundPort = server.port();
  proof.expect(boundPort != 0u,
               "the operating system assigned port " + std::to_string(boundPort));

  // start() only binds the listener and wakes the worker pool; the accepting
  // loop runs on this background thread until stop() is called.
  std::thread serverThread([&server]() { server.serve(); });

  SpectrumClient client;
  Status connected = client.connect("127.0.0.1", boundPort);
  for (int attempt = 1; attempt < kMaxConnectAttempts && !connected.ok(); ++attempt) {
    connected = client.connect("127.0.0.1", boundPort);
  }

  if (proof.expect(connected.ok(), "SpectrumClient connected to 127.0.0.1:" +
                                       std::to_string(boundPort) + " [status=" +
                                       describeStatus(connected) + "]")) {
    expectStatus(proof, client.ping(), "ping over the framed transport");

    const Authorities authorities = authoritiesOf(serverRuntime);
    const SpectrumRequest wireRequest =
        makeRequest(100u, domain, grid.id, 1u, requestedAt, Duration::minutes(20), authorities, 0u,
                    {});
    AllocationDecision wireDecision;
    expectStatus(proof, client.allocate(wireRequest, wireDecision),
                 "allocate over the framed transport");
    if (proof.expect(wireDecision.allocated(), "the transported allocation committed [outcome=" +
                                                   token(wireDecision.outcome) + "]")) {
      const auto serverSide = serverRuntime.reservation(wireDecision.reservation);
      if (proof.expect(serverSide.has_value(),
                       "reservation " + typedToken("reservation", wireDecision.reservation) +
                           " exists on the server runtime")) {
        proof.expect(serverSide->state == ReservationState::Reserved,
                     "the server-side state is " + token(serverSide->state));
        proof.expect(serverSide->slots.first == 0u && serverSide->slots.count == 1u,
                     "the server-side slot range is [0, 1)");
      }
    }
    proof.expect(server.servedRequests() >= 2u,
                 "the server dispatched " + std::to_string(server.servedRequests()) +
                     " request(s)");
    proof.expect(server.rejectedFrames() == 0u,
                 "the server rejected " + std::to_string(server.rejectedFrames()) + " frame(s)");
  }

  client.close();
  server.stop();
  serverThread.join();
  proof.expect(server.stopped(), "the server stopped and its background thread joined");
}

void printObservedTokens() {
  std::cout << "\ntyped tokens observed:"
            << " grid=" << token(GridKind::Fixed)
            << " support=" << token(SpectrumSupport::Supported)
            << " candidate=" << token(CandidateEligibility::Eligible)
            << " allocated=" << token(AllocationOutcome::Allocated)
            << " conflict=" << token(AllocationOutcome::RefusedConflict)
            << " exclusion=" << token(AllocationOutcome::RefusedExclusion)
            << " reserved=" << token(ReservationState::Reserved)
            << " active=" << token(ReservationState::Active)
            << " released=" << token(ReservationState::Released)
            << " reclaimed=" << token(ReservationState::Reclaimed)
            << " ok=" << token(StatusCode::Ok)
            << " conflict-status=" << token(StatusCode::Conflict)
            << " excluded-status=" << token(StatusCode::Excluded) << '\n';
}

}  // namespace

int main() {
  std::cout << "WavelengthFabric consumer proof against the installed package\n";
  std::cout << "library version " << kVersionString << "; persistence format version "
            << kPersistenceFormatVersion << "; protocol version " << kProtocolVersion << '\n';

  Proof proof;
  const std::filesystem::path statePath = temporaryStatePath();
  std::cout << "scratch state file: " << statePath.string() << '\n';

  proveCore(proof, statePath.string());
  proveTransport(proof);
  printObservedTokens();

  std::cout << "\n" << proof.checks() << " check(s), " << proof.failures() << " failure(s)\n";
  if (proof.failures() == 0) {
    std::cout << "PASS\n";
    return 0;
  }
  std::cout << "FAIL\n";
  return 1;
}
