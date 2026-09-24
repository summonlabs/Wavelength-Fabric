// basic_allocation: one full trip through the Wavelength Fabric core.
//
// The program registers a grid, a domain and a capability, commits an
// allocation, is refused a conflicting request, and then returns the spectrum
// twice over: by releasing a reservation and by reclaiming an expired lease.
// Every step is checked against the documented contract, and the process exits
// non-zero if any step behaves differently.
//
// The instants are fixed rather than read from the wall clock. The runtime
// never reads a clock itself, so a fixed instant makes the whole run
// reproducible.

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

#include "wavelength_fabric/runtime.hpp"
#include "wavelength_fabric/text.hpp"

namespace wf = wavelength_fabric;

namespace {

constexpr std::uint64_t kGridId = 1;
constexpr std::uint64_t kDomainId = 1;

int failures = 0;

void check(bool condition, const std::string& what) {
  std::cout << (condition ? "ok   " : "FAIL ") << what << '\n';
  if (!condition) failures += 1;
}

// The decision line already carries the summary; this prints the ordered
// reasons behind it, first reason first.
void printReasons(const wf::DecisionExplanation& explanation) {
  for (const std::string& reason : explanation.reasons) {
    std::cout << "     reason: " << reason << '\n';
  }
}

// Every request is minted under the fence the runtime currently recognises, so
// the authority tokens it carries are always current.
[[nodiscard]] wf::SpectrumRequest makeRequest(wf::SpectrumRuntime& runtime, std::uint64_t requestId,
                                              std::uint32_t slots, wf::Instant at,
                                              wf::Duration lease) {
  const wf::AuthorityState authority = runtime.authorityState();
  wf::SpectrumRequest request;
  request.requestId = wf::AllocationRequestId(requestId);
  request.requestGeneration = wf::AllocationRequestGeneration(1);
  request.owner = wf::OwnerId(1);
  request.ownerGeneration = wf::OwnerGeneration(1);
  request.domains = {wf::SpectrumDomainId(kDomainId)};
  request.domainGenerations = {wf::SpectrumDomainGeneration(1)};
  request.grid = wf::ChannelGridId(kGridId);
  request.gridGeneration = wf::GridGeneration(1);
  request.slots = slots;
  // The request is explicit about both requirements. The runtime refuses an
  // unstated requirement on a multi-slot channel rather than guessing one.
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

}  // namespace

int main() {
  wf::SpectrumRuntime runtime;
  const wf::Instant start = wf::Instant::fromSeconds(1'700'000'000);
  const wf::Duration lease = wf::Duration::seconds(60);

  // ---- four 50 GHz slots, one to four per channel ------------------------
  wf::ChannelGrid grid;
  grid.id = wf::ChannelGridId(kGridId);
  grid.generation = wf::GridGeneration(1);
  grid.kind = wf::GridKind::Flex;
  grid.anchorMhz = 191'400'000;  // an abstract anchor, not a hardware claim
  grid.slotWidthMhz = 50'000;
  grid.slotCount = 4;
  // A flex grid bounds the channel width; a fixed grid would allow exactly one
  // slot per channel and the runtime would refuse the two slot requests below.
  grid.minSlotsPerChannel = 1;
  grid.maxSlotsPerChannel = 4;
  const wf::Status gridStatus = runtime.registerGrid(grid);
  check(gridStatus.ok(), "register grid: status=" + std::string(wf::toToken(gridStatus.code)));
  std::cout << "     " << wf::describeGrid(grid) << '\n';

  // ---- one domain on that grid -------------------------------------------
  wf::SpectrumDomain domain;
  domain.id = wf::SpectrumDomainId(kDomainId);
  domain.generation = wf::SpectrumDomainGeneration(1);
  domain.klass = wf::ResourceClass::AbstractDomain;
  domain.grid = grid.id;
  domain.gridGeneration = grid.generation;
  domain.requiresContiguity = true;
  const wf::Status domainStatus = runtime.registerDomain(domain);
  check(domainStatus.ok(),
        "register domain: status=" + std::string(wf::toToken(domainStatus.code)));

  // ---- a capability is a claim, and a supported claim needs evidence ------
  wf::SpectrumCapability capability;
  capability.domain = domain.id;
  capability.domainGeneration = domain.generation;
  capability.grid = grid.id;
  capability.gridGeneration = grid.generation;
  capability.support = wf::SpectrumSupport::Supported;
  capability.firstAllocatableSlot = 0;
  capability.allocatableSlots = grid.slotCount;
  capability.minTunableMhz = grid.anchorMhz;
  capability.maxTunableMhz = grid.endMhz();
  capability.presenceEvidence.present = true;
  capability.presenceEvidence.digest = 0x1BAD'B002ull;
  capability.presenceEvidence.source = "basic_allocation";
  capability.generation = wf::CapabilityGeneration(1);
  capability.publisher = wf::ControllerId(1);
  capability.fence = runtime.fence();
  capability.publishedAt = start;
  const wf::Status capabilityStatus = runtime.publishCapability(capability);
  check(capabilityStatus.ok(),
        "publish capability: status=" + std::string(wf::toToken(capabilityStatus.code)));
  std::cout << "     " << wf::describeCapability(capability) << '\n';

  // ---- commit a two slot channel -----------------------------------------
  const wf::AllocationDecision committed =
      runtime.allocate(makeRequest(runtime, 1, 2, start, lease));
  check(committed.allocated(), "allocate two slots: outcome=" +
                                   std::string(wf::toToken(committed.outcome)));
  std::cout << "     " << committed.describe() << '\n';
  printReasons(committed.explanation);
  if (!committed.allocated()) {
    std::cout << "basic_allocation: FAIL\n";
    return 1;
  }

  // ---- an overlapping request is refused, and says why --------------------
  wf::SpectrumRequest conflicting = makeRequest(runtime, 2, 2, start, lease);
  conflicting.frequencyWindows = {committed.explanation.selected.frequency};
  const wf::AllocationDecision refused = runtime.allocate(conflicting);
  check(!refused.allocated() && refused.outcome == wf::AllocationOutcome::RefusedConflict,
        "refuse a conflicting request: outcome=" + std::string(wf::toToken(refused.outcome)) +
            " status=" + std::string(wf::toToken(refused.status.code)));
  std::cout << "     " << refused.describe() << '\n';
  printReasons(refused.explanation);

  // ---- release returns the spectrum immediately --------------------------
  const std::optional<wf::SpectrumReservation> owned = runtime.reservation(committed.reservation);
  check(owned.has_value(), "the committed reservation is readable");
  if (!owned.has_value()) {
    std::cout << "basic_allocation: FAIL\n";
    return 1;
  }
  const wf::AuthorityState authority = runtime.authorityState();
  const wf::ReleaseAuthority releaseAuthority{authority.releaseGeneration, authority.fence};
  const wf::Status released =
      runtime.release(owned->id, owned->generation, releaseAuthority, start);
  check(released.ok(), "release the reservation: status=" + std::string(wf::toToken(released.code)));
  const std::optional<wf::SpectrumUsage> afterRelease =
      runtime.usage(wf::SpectrumDomainId(kDomainId), start);
  check(afterRelease.has_value() && afterRelease->freeSlots == grid.slotCount &&
            afterRelease->liveSlots == 0 && afterRelease->liveReservations == 0,
        "release returns every slot: freeSlots=" +
            std::to_string(afterRelease.has_value() ? afterRelease->freeSlots : 0));

  // ---- a lapsed lease is reclaimed, not silently forgotten ----------------
  const wf::AllocationDecision expiring =
      runtime.allocate(makeRequest(runtime, 3, 2, start, wf::Duration::seconds(5)));
  check(expiring.allocated(), "allocate with a five second lease: outcome=" +
                                  std::string(wf::toToken(expiring.outcome)));
  const wf::Instant later = start + wf::Duration::seconds(6);
  const wf::ReclaimReport report = runtime.reclaimExpired(later);
  const std::optional<wf::SpectrumUsage> afterReclaim =
      runtime.usage(wf::SpectrumDomainId(kDomainId), later);
  check(report.reclaimed.size() == 1 && report.reclaimed.front() == expiring.reservation &&
            afterReclaim.has_value() && afterReclaim->freeSlots == grid.slotCount &&
            afterReclaim->liveSlots == 0,
        "reclaim the expired lease: reclaimed=" + std::to_string(report.reclaimed.size()) +
            " freeSlots=" + std::to_string(afterReclaim.has_value() ? afterReclaim->freeSlots : 0));
  std::cout << "     " << report.describe() << '\n';

  std::cout << "basic_allocation: " << (failures == 0 ? "PASS" : "FAIL") << '\n';
  return failures == 0 ? 0 : 1;
}
