// multi_domain_continuity: continuity across two optical resources.
//
// A request that spans more than one domain must state its continuity
// requirement, and when continuity is Required the runtime looks for one
// frequency range that is free on every spanned domain at the same time. This
// example allocates such a channel, prints the slot range it resolved on each
// domain, and then shows a request that cannot be satisfied even though each
// domain still has enough free spectrum on its own.
//
// Every step is checked against the documented contract and the process exits
// non-zero if any step behaves differently.

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "wavelength_fabric/grid.hpp"
#include "wavelength_fabric/runtime.hpp"
#include "wavelength_fabric/text.hpp"

namespace wf = wavelength_fabric;

namespace {

constexpr std::uint64_t kGridId = 1;
constexpr std::uint64_t kFirstDomain = 1;
constexpr std::uint64_t kSecondDomain = 2;

int failures = 0;

void check(bool condition, const std::string& what) {
  std::cout << (condition ? "ok   " : "FAIL ") << what << '\n';
  if (!condition) failures += 1;
}

void explain(const wf::DecisionExplanation& explanation) {
  std::cout << "     " << explanation.summary() << '\n';
  for (const std::string& reason : explanation.reasons) {
    std::cout << "     reason: " << reason << '\n';
  }
}

// A request that spans the listed domains, in ascending identity order, with
// both requirements stated explicitly.
[[nodiscard]] wf::SpectrumRequest makeRequest(wf::SpectrumRuntime& runtime, std::uint64_t requestId,
                                              const std::vector<std::uint64_t>& domainIds,
                                              std::uint32_t slots, wf::Instant at,
                                              wf::Duration lease) {
  const wf::AuthorityState authority = runtime.authorityState();
  wf::SpectrumRequest request;
  request.requestId = wf::AllocationRequestId(requestId);
  request.requestGeneration = wf::AllocationRequestGeneration(1);
  request.owner = wf::OwnerId(1);
  request.ownerGeneration = wf::OwnerGeneration(1);
  for (const std::uint64_t domain : domainIds) {
    request.domains.push_back(wf::SpectrumDomainId(domain));
    request.domainGenerations.push_back(wf::SpectrumDomainGeneration(1));
  }
  request.grid = wf::ChannelGridId(kGridId);
  request.gridGeneration = wf::GridGeneration(1);
  request.slots = slots;
  request.contiguity = wf::ContiguityRequirement::Required;
  request.continuity = wf::ContinuityRequirement::Required;
  request.leaseDuration = lease;
  request.requestedAt = at;
  request.notBefore = wf::Instant{};
  request.eligibilityAuthority =
      wf::EligibilityAuthority{authority.eligibilityGeneration, authority.fence};
  request.reservationAuthority =
      wf::ReservationAuthority{authority.reservationGeneration, authority.fence};
  return request;
}

void registerDomain(wf::SpectrumRuntime& runtime, std::uint64_t id) {
  wf::SpectrumDomain domain;
  domain.id = wf::SpectrumDomainId(id);
  domain.generation = wf::SpectrumDomainGeneration(1);
  domain.klass = wf::ResourceClass::AbstractDomain;
  domain.grid = wf::ChannelGridId(kGridId);
  domain.gridGeneration = wf::GridGeneration(1);
  domain.requiresContiguity = true;
  const wf::Status status = runtime.registerDomain(domain);
  check(status.ok(), "register domain " + std::to_string(id) +
                         ": status=" + std::string(wf::toToken(status.code)));
}

void publishCapability(wf::SpectrumRuntime& runtime, const wf::ChannelGrid& grid,
                       std::uint64_t domainId, wf::Instant at) {
  wf::SpectrumCapability capability;
  capability.domain = wf::SpectrumDomainId(domainId);
  capability.domainGeneration = wf::SpectrumDomainGeneration(1);
  capability.grid = grid.id;
  capability.gridGeneration = grid.generation;
  capability.support = wf::SpectrumSupport::Supported;
  capability.firstAllocatableSlot = 0;
  capability.allocatableSlots = grid.slotCount;
  capability.minTunableMhz = grid.anchorMhz;
  capability.maxTunableMhz = grid.endMhz();
  capability.presenceEvidence.present = true;
  capability.presenceEvidence.digest = 0x5EED'0000ull + domainId;
  capability.presenceEvidence.source = "multi_domain_continuity";
  capability.generation = wf::CapabilityGeneration(1);
  capability.publisher = wf::ControllerId(1);
  capability.fence = runtime.fence();
  capability.publishedAt = at;
  const wf::Status status = runtime.publishCapability(capability);
  check(status.ok(), "publish capability for domain " + std::to_string(domainId) +
                         ": status=" + std::string(wf::toToken(status.code)));
}

void reportFreeSlots(wf::SpectrumRuntime& runtime, const wf::ChannelGrid& grid, std::uint64_t domainId,
                     wf::Instant at) {
  const std::optional<wf::SpectrumUsage> usage = runtime.usage(wf::SpectrumDomainId(domainId), at);
  if (!usage.has_value()) {
    check(false, "read usage for domain " + std::to_string(domainId));
    return;
  }
  std::cout << "     domain " << domainId << " free slots=" << usage->freeSlots << " runs:";
  for (const wf::SlotRange& run : usage->freeRuns) {
    std::cout << ' ' << wf::describeSlotRange(grid, run);
  }
  std::cout << '\n';
}

}  // namespace

int main() {
  wf::SpectrumRuntime runtime;
  const wf::Instant start = wf::Instant::fromSeconds(1'700'000'000);
  const wf::Duration lease = wf::Duration::seconds(60);

  // ---- one grid, two domains on it ---------------------------------------
  wf::ChannelGrid grid;
  grid.id = wf::ChannelGridId(kGridId);
  grid.generation = wf::GridGeneration(1);
  // A flex grid is required for a four slot channel: the runtime refuses a
  // channel width the grid cannot express.
  grid.kind = wf::GridKind::Flex;
  grid.anchorMhz = 191'400'000;
  grid.slotWidthMhz = 50'000;
  grid.slotCount = 8;
  grid.minSlotsPerChannel = 1;
  grid.maxSlotsPerChannel = 8;
  const wf::Status gridStatus = runtime.registerGrid(grid);
  check(gridStatus.ok(), "register grid: status=" + std::string(wf::toToken(gridStatus.code)));
  std::cout << "     " << wf::describeGrid(grid) << '\n';

  registerDomain(runtime, kFirstDomain);
  registerDomain(runtime, kSecondDomain);
  publishCapability(runtime, grid, kFirstDomain, start);
  publishCapability(runtime, grid, kSecondDomain, start);

  // ---- one channel, continuous across both domains ------------------------
  const std::vector<std::uint64_t> bothDomains{kFirstDomain, kSecondDomain};
  const wf::AllocationDecision continuous =
      runtime.allocate(makeRequest(runtime, 1, bothDomains, 4, start, lease));
  check(continuous.allocated(), "allocate four continuous slots across both domains: outcome=" +
                                    std::string(wf::toToken(continuous.outcome)));
  std::cout << "     " << continuous.describe() << '\n';
  explain(continuous.explanation);
  if (!continuous.allocated()) {
    std::cout << "multi_domain_continuity: FAIL\n";
    return 1;
  }

  const std::optional<wf::SpectrumReservation> reservation =
      runtime.reservation(continuous.reservation);
  check(reservation.has_value() && reservation->perDomainSlots.size() == bothDomains.size() &&
            reservation->continuityRequired,
        "the commit resolved one slot range per domain");
  if (!reservation.has_value()) {
    std::cout << "multi_domain_continuity: FAIL\n";
    return 1;
  }
  std::cout << "     per-domain slot ranges resolved at commit time:\n";
  for (std::size_t index = 0; index < reservation->domains.size(); ++index) {
    std::cout << "     domain " << reservation->domains[index].raw();
    if (index < reservation->perDomainSlots.size()) {
      std::cout << ' ' << wf::describeSlotRange(grid, reservation->perDomainSlots[index]);
    }
    std::cout << '\n';
  }

  // ---- a second channel that only domain two can carry --------------------
  const wf::AllocationDecision narrow =
      runtime.allocate(makeRequest(runtime, 2, {kSecondDomain}, 2, start, lease));
  check(narrow.allocated(), "allocate two slots on the second domain alone: outcome=" +
                                std::string(wf::toToken(narrow.outcome)));
  if (narrow.allocated()) {
    std::cout << "     domain " << kSecondDomain << ' '
              << wf::renderSlotRange(narrow.explanation.selected.slots) << ' '
              << wf::renderFrequencyRange(narrow.explanation.selected.frequency) << '\n';
  }

  // ---- where the free spectrum actually is -------------------------------
  reportFreeSlots(runtime, grid, kFirstDomain, start);
  reportFreeSlots(runtime, grid, kSecondDomain, start);

  // ---- the first domain alone could still carry four slots ---------------
  const wf::DecisionExplanation localOnly =
      runtime.explain(makeRequest(runtime, 3, {kFirstDomain}, 4, start, lease));
  check(localOnly.outcome == wf::AllocationOutcome::Allocated,
        "the first domain alone could carry four slots: outcome=" +
            std::string(wf::toToken(localOnly.outcome)));

  // ---- but no single range is free on both domains at once ---------------
  const wf::SpectrumRequest pair = makeRequest(runtime, 4, bothDomains, 4, start, lease);
  const wf::DecisionExplanation explanation = runtime.explain(pair);
  check(wf::isRefusal(explanation.outcome),
        "a continuous four slot channel is not available across both domains: outcome=" +
            std::string(wf::toToken(explanation.outcome)));
  std::cout << "     " << explanation.summary() << '\n';
  explain(explanation);

  const wf::AllocationDecision refused = runtime.allocate(pair);
  check(!refused.allocated(),
        "the same request is refused when it is committed: outcome=" +
            std::string(wf::toToken(refused.outcome)) +
            " status=" + std::string(wf::toToken(refused.status.code)));
  std::cout << "     " << refused.describe() << '\n';
  explain(refused.explanation);

  std::cout << "multi_domain_continuity: " << (failures == 0 ? "PASS" : "FAIL") << '\n';
  return failures == 0 ? 0 : 1;
}
