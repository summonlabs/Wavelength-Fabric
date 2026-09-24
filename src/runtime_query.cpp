#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "impl.hpp"

namespace wavelength_fabric {

namespace {

void subtractRanges(std::vector<SlotRange>& pieces, SlotRange cut) {
  if (cut.empty()) return;
  std::vector<SlotRange> next;
  next.reserve(pieces.size() + 1);
  for (const SlotRange& piece : pieces) {
    if (!piece.overlaps(cut)) {
      next.push_back(piece);
      continue;
    }
    if (cut.first > piece.first) {
      next.push_back(SlotRange{piece.first, cut.first - piece.first});
    }
    const std::uint32_t cutEnd = cut.end();
    if (cutEnd < piece.end()) {
      next.push_back(SlotRange{cutEnd, piece.end() - cutEnd});
    }
  }
  pieces.swap(next);
}

void mergeRanges(std::vector<SlotRange>& ranges) {
  if (ranges.empty()) return;
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

[[nodiscard]] std::uint32_t coveredSlots(const std::vector<SlotRange>& ranges) {
  std::vector<SlotRange> merged = ranges;
  mergeRanges(merged);
  std::uint32_t total = 0;
  for (const SlotRange& range : merged) total += range.count;
  return total;
}

[[nodiscard]] bool computeUsage(const detail::RuntimeState& state, SpectrumDomainId id, Instant now,
                                SpectrumUsage& out) {
  const auto domainIt = state.domains.find(id);
  if (domainIt == state.domains.end()) return false;
  const SpectrumDomain& domain = domainIt->second;
  const auto gridIt = state.grids.find(domain.grid);
  if (gridIt == state.grids.end()) return false;
  const ChannelGrid& grid = gridIt->second;

  out = SpectrumUsage{};
  out.domain = id;
  out.domainGeneration = domain.generation;
  out.grid = domain.grid;
  out.gridGeneration = domain.gridGeneration;
  out.totalSlots = grid.slotCount;

  SlotRange window{};
  const auto capabilityIt = state.capabilities.find(id);
  if (capabilityIt != state.capabilities.end() &&
      capabilityIt->second.domainGeneration == domain.generation &&
      capabilityIt->second.support == SpectrumSupport::Supported) {
    const SlotRange candidate = allocatableWindow(capabilityIt->second);
    if (slotRangeInGrid(grid, candidate)) window = candidate;
  }
  out.allocatableSlots = window.count;

  std::vector<SlotRange> live;
  std::vector<SlotRange> active;
  std::vector<SlotRange> reserved;
  std::vector<SlotRange> lapsed;

  const auto indexed = state.liveByDomain.find(id);
  if (indexed != state.liveByDomain.end()) {
    for (const ReservationId reservationId : indexed->second) {
      const auto reservationIt = state.reservations.find(reservationId);
      if (reservationIt == state.reservations.end()) continue;
      const SpectrumReservation& reservation = reservationIt->second;
      const auto position =
          std::find(reservation.domains.begin(), reservation.domains.end(), id);
      if (position == reservation.domains.end()) continue;
      const std::size_t index = static_cast<std::size_t>(position - reservation.domains.begin());
      SlotRange range = reservation.slots;
      if (index < reservation.perDomainSlots.size()) range = reservation.perDomainSlots[index];
      if (range.empty()) continue;
      if (!reservation.lease.validAt(now)) {
        lapsed.push_back(range);
        continue;
      }
      live.push_back(range);
      if (reservation.state == ReservationState::Active) {
        active.push_back(range);
      } else {
        reserved.push_back(range);
      }
    }
  }

  out.liveSlots = coveredSlots(live);
  out.activeSlots = coveredSlots(active);
  out.reservedSlots = coveredSlots(reserved);
  out.lapsedSlots = coveredSlots(lapsed);
  out.freeSlots = window.count > out.liveSlots ? window.count - out.liveSlots : 0;
  out.liveReservations = static_cast<std::uint32_t>(live.size());
  out.activeReservations = static_cast<std::uint32_t>(active.size());
  out.lapsedReservations = static_cast<std::uint32_t>(lapsed.size());

  for (const auto& entry : state.reservations) {
    const SpectrumReservation& reservation = entry.second;
    if (std::find(reservation.domains.begin(), reservation.domains.end(), id) ==
        reservation.domains.end()) {
      continue;
    }
    if (reservation.state == ReservationState::Reclaimed) out.reclaimedReservations += 1;
    if (reservation.state == ReservationState::Released) out.releasedReservations += 1;
  }

  if (!window.empty()) {
    std::vector<SlotRange> free{window};
    std::vector<SlotRange> blocked = live;
    mergeRanges(blocked);
    for (const SlotRange& cut : blocked) subtractRanges(free, cut);
    mergeRanges(free);
    out.freeRuns = free;
  }
  return true;
}

}  // namespace

const SpectrumCandidate* CandidateSet::firstEligible() const noexcept {
  for (const SpectrumCandidate& candidate : candidates) {
    if (isEligible(candidate.eligibility)) return &candidate;
  }
  return nullptr;
}

CandidateSet SpectrumRuntime::enumerateCandidates(const SpectrumRequest& request) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return detail::enumerate(*state_, request);
}

std::optional<ChannelGrid> SpectrumRuntime::grid(ChannelGridId id) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  const auto it = state_->grids.find(id);
  if (it == state_->grids.end()) return std::nullopt;
  return it->second;
}

std::optional<SpectrumDomain> SpectrumRuntime::domain(SpectrumDomainId id) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  const auto it = state_->domains.find(id);
  if (it == state_->domains.end()) return std::nullopt;
  return it->second;
}

std::optional<ExclusionDomain> SpectrumRuntime::exclusionDomain(ExclusionDomainId id) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  const auto it = state_->exclusionDomains.find(id);
  if (it == state_->exclusionDomains.end()) return std::nullopt;
  return it->second;
}

std::optional<SpectrumCapability> SpectrumRuntime::capability(SpectrumDomainId id) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  const auto it = state_->capabilities.find(id);
  if (it == state_->capabilities.end()) return std::nullopt;
  return it->second;
}

std::optional<SpectrumReservation> SpectrumRuntime::reservation(ReservationId id) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  const auto it = state_->reservations.find(id);
  if (it == state_->reservations.end()) return std::nullopt;
  return it->second;
}

std::vector<SpectrumReservation> SpectrumRuntime::reservations() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  std::vector<SpectrumReservation> out;
  out.reserve(state_->reservations.size());
  for (const auto& entry : state_->reservations) out.push_back(entry.second);
  return out;
}

std::vector<SpectrumReservation> SpectrumRuntime::reservationsForDomain(SpectrumDomainId id) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  std::vector<SpectrumReservation> out;
  for (const auto& entry : state_->reservations) {
    const SpectrumReservation& reservation = entry.second;
    if (std::find(reservation.domains.begin(), reservation.domains.end(), id) !=
        reservation.domains.end()) {
      out.push_back(reservation);
    }
  }
  return out;
}

std::vector<SpectrumDomain> SpectrumRuntime::domains() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  std::vector<SpectrumDomain> out;
  out.reserve(state_->domains.size());
  for (const auto& entry : state_->domains) out.push_back(entry.second);
  return out;
}

std::vector<ChannelGrid> SpectrumRuntime::grids() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  std::vector<ChannelGrid> out;
  out.reserve(state_->grids.size());
  for (const auto& entry : state_->grids) out.push_back(entry.second);
  return out;
}

std::vector<ExclusionDomain> SpectrumRuntime::exclusionDomains() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  std::vector<ExclusionDomain> out;
  out.reserve(state_->exclusionDomains.size());
  for (const auto& entry : state_->exclusionDomains) out.push_back(entry.second);
  return out;
}

std::optional<SpectrumUsage> SpectrumRuntime::usage(SpectrumDomainId id, Instant now) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  SpectrumUsage out;
  if (!computeUsage(*state_, id, now, out)) return std::nullopt;
  return out;
}

std::vector<SpectrumUsage> SpectrumRuntime::usageAll(Instant now) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  std::vector<SpectrumUsage> out;
  out.reserve(state_->domains.size());
  for (const auto& entry : state_->domains) {
    SpectrumUsage usage;
    if (computeUsage(*state_, entry.first, now, usage)) out.push_back(std::move(usage));
  }
  return out;
}

std::vector<AuditRecord> SpectrumRuntime::audit(AuditSequence since, std::size_t limit) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  std::size_t cap = limit == 0 ? state_->config.maxAuditPageSize : limit;
  if (cap > state_->config.maxAuditPageSize) cap = state_->config.maxAuditPageSize;
  std::vector<AuditRecord> out;
  out.reserve(std::min<std::size_t>(cap, state_->audit.size()));
  for (const AuditRecord& record : state_->audit) {
    if (record.sequence <= since) continue;
    if (out.size() >= cap) break;
    out.push_back(record);
  }
  return out;
}

std::size_t SpectrumRuntime::auditSize() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return state_->audit.size();
}

AuditSequence SpectrumRuntime::lastAuditSequence() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return state_->auditSequence;
}

RuntimeStats SpectrumRuntime::stats() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return state_->stats;
}

}  // namespace wavelength_fabric
