#include <string>

#include "impl.hpp"

namespace wavelength_fabric {

Status SpectrumRuntime::publishCapability(const SpectrumCapability& capability) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  Status fenceStatus = checkFence(state.authority, capability.fence);
  if (!fenceStatus.ok()) {
    detail::appendAudit(state, AuditKind::ReplayRejected, systemNow(),
                        AllocationOutcome::RefusedStaleAuthority, ReservationId{},
                        ReservationGeneration{}, capability.domain, SlotRange{}, FrequencyRange{},
                        "capability publication rejected: " + fenceStatus.message);
    return fenceStatus;
  }

  Status shape = validateCapabilityShape(capability);
  if (!shape.ok()) return shape;

  const auto domainIt = state.domains.find(capability.domain);
  if (domainIt == state.domains.end()) {
    return fail(StatusCode::NotFound, "capability references unregistered domain " +
                                          typedToken("domain", capability.domain));
  }
  const SpectrumDomain& domain = domainIt->second;
  if (domain.generation != capability.domainGeneration) {
    return fail(StatusCode::StaleGeneration,
                "capability for domain " + typedToken("domain", capability.domain) +
                    " names domain generation " +
                    std::to_string(capability.domainGeneration.raw()) +
                    " but the registered domain is at generation " +
                    std::to_string(domain.generation.raw()));
  }
  if (domain.grid != capability.grid ||
      domain.gridGeneration != capability.gridGeneration) {
    return fail(StatusCode::InvalidArgument,
                "capability grid " + typedToken("grid", capability.grid) + " generation " +
                    std::to_string(capability.gridGeneration.raw()) +
                    " must match domain grid " + typedToken("grid", domain.grid) + " generation " +
                    std::to_string(domain.gridGeneration.raw()));
  }

  const auto gridIt = state.grids.find(capability.grid);
  if (gridIt == state.grids.end()) {
    return fail(StatusCode::NotFound,
                "capability references unregistered grid " + typedToken("grid", capability.grid));
  }
  const ChannelGrid& grid = gridIt->second;
  if (grid.generation != capability.gridGeneration) {
    return fail(StatusCode::StaleGeneration,
                "capability names grid generation " +
                    std::to_string(capability.gridGeneration.raw()) +
                    " but the registered grid is at generation " +
                    std::to_string(grid.generation.raw()));
  }

  if (capability.support == SpectrumSupport::Supported) {
    std::uint64_t windowEnd = static_cast<std::uint64_t>(capability.firstAllocatableSlot) +
                              static_cast<std::uint64_t>(capability.allocatableSlots);
    if (windowEnd > static_cast<std::uint64_t>(grid.slotCount)) {
      return fail(StatusCode::InvalidArgument,
                  "allocatable window " + std::to_string(capability.firstAllocatableSlot) + "+" +
                      std::to_string(capability.allocatableSlots) +
                      " leaves grid " + typedToken("grid", grid.id) + " with " +
                      std::to_string(grid.slotCount) + " slots");
    }
    if (capability.minTunableMhz < grid.anchorMhz) {
      return fail(StatusCode::InvalidArgument, "minTunableMhz is below the grid anchor frequency");
    }
    std::int64_t gridEnd = 0;
    if (mulOverflow(grid.slotWidthMhz, static_cast<std::int64_t>(grid.slotCount), gridEnd) ||
        addOverflow(grid.anchorMhz, gridEnd, gridEnd)) {
      return fail(StatusCode::InvalidArgument, "grid frequency span is not representable");
    }
    if (capability.maxTunableMhz > gridEnd) {
      return fail(StatusCode::InvalidArgument, "maxTunableMhz is above the grid end frequency");
    }
    // A grid must be able to express at least its own narrowest channel: a
    // fixed grid is one slot per channel, and a flex grid is bounded below by
    // its own minSlotsPerChannel rather than by one slot.
    const std::uint32_t narrowestChannel =
        grid.kind == GridKind::Fixed ? 1u : grid.minSlotsPerChannel;
    if (!channelWidthSupported(grid, narrowestChannel)) {
      return fail(StatusCode::InvalidArgument, "grid does not support any channel width");
    }
  }

  const auto previous = state.capabilities.find(capability.domain);
  if (previous != state.capabilities.end()) {
    if (capability.generation == previous->second.generation) {
      return fail(StatusCode::Duplicate,
                  "capability for domain " + typedToken("domain", capability.domain) +
                      " is already published at generation " +
                      std::to_string(capability.generation.raw()));
    }
    if (capability.generation < previous->second.generation) {
      return fail(StatusCode::StaleGeneration,
                  "capability for domain " + typedToken("domain", capability.domain) +
                      " is published at generation " +
                      std::to_string(previous->second.generation.raw()) + "; generation " +
                      std::to_string(capability.generation.raw()) + " is stale");
    }
  }

  state.capabilities[capability.domain] = capability;
  detail::appendAudit(state, AuditKind::CapabilityPublished,
                      capability.publishedAt.isZero() ? systemNow() : capability.publishedAt,
                      AllocationOutcome::Unknown, ReservationId{}, ReservationGeneration{},
                      capability.domain, allocatableWindow(capability),
                      FrequencyRange{capability.minTunableMhz, capability.maxTunableMhz},
                      describeCapability(capability));
  return okStatus();
}

}  // namespace wavelength_fabric
