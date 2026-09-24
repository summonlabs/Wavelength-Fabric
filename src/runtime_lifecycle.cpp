#include <algorithm>
#include <string>
#include <vector>

#include "impl.hpp"

namespace wavelength_fabric {

namespace {

constexpr std::int64_t kMaxLeaseExtensionNanos = 3650ll * 86'400ll * 1'000'000'000ll;

[[nodiscard]] bool advanceState(detail::RuntimeState& state, SpectrumReservation& reservation,
                                ReservationState next, std::string& error) {
  if (!isLegalTransition(reservation.state, next)) {
    error = "transition from " + std::string(toToken(reservation.state)) + " to " +
            std::string(toToken(next)) + " is not permitted";
    detail::appendAudit(state, AuditKind::ReplayRejected, reservation.updatedAt,
                        AllocationOutcome::RefusedNotPermitted, reservation.id,
                        reservation.generation, reservation.anchorDomain, reservation.slots,
                        reservation.frequency, error);
    return false;
  }
  reservation.state = next;
  return true;
}

// Restores one reservation exactly as it was before a mutation that could not
// be made durable.
void rollbackReservation(detail::RuntimeState& state, const SpectrumReservation& backup) {
  state.reservations[backup.id] = backup;
  detail::removeLiveReservation(state, backup.id);
  if (isLiveState(backup.state)) detail::addLiveReservation(state, backup);
}

struct Lookup {
  Status status;
  SpectrumReservation* reservation{nullptr};
};

[[nodiscard]] Lookup lookup(detail::RuntimeState& state, ReservationId id,
                            ReservationGeneration generation) {
  Lookup result;
  const auto it = state.reservations.find(id);
  if (it == state.reservations.end()) {
    result.status = fail(StatusCode::NotFound, "reservation " + typedToken("reservation", id) +
                                                   " is not known to this runtime");
    return result;
  }
  if (it->second.generation != generation) {
    result.status = fail(StatusCode::StaleGeneration,
                         "reservation " + typedToken("reservation", id) + " is at generation " +
                             std::to_string(it->second.generation.raw()) +
                             "; the operation names generation " +
                             std::to_string(generation.raw()));
    detail::appendAudit(state, AuditKind::ReplayRejected, it->second.updatedAt,
                        AllocationOutcome::RefusedStaleGeneration, id, generation,
                        it->second.anchorDomain, it->second.slots, it->second.frequency,
                        result.status.message);
    return result;
  }
  result.reservation = &it->second;
  result.status = okStatus();
  return result;
}

// Marks every reservation whose lease has lapsed as Expired and returns its
// spectrum to the free pool. This does not reclaim: reclamation is a separate,
// explicit accounting step. Every changed reservation is recorded so that the
// caller can roll the whole sweep back if it cannot be made durable.
void markLapsed(detail::RuntimeState& state, Instant now, ReclaimReport& report,
                std::vector<SpectrumReservation>& backups) {
  for (auto& entry : state.reservations) {
    SpectrumReservation& reservation = entry.second;
    if (!isLiveState(reservation.state)) continue;
    if (reservation.lease.validAt(now)) continue;
    std::string error;
    if (!advanceState(state, reservation, ReservationState::Expired, error)) continue;
    backups.push_back(reservation);
    reservation.updatedAt = now;
    reservation.lastOperation.bump();
    detail::removeLiveReservation(state, reservation.id);
    report.lapsed.push_back(reservation.id);
    report.expired += 1;
    detail::appendAudit(state, AuditKind::ReservationExpired, now, AllocationOutcome::Unknown,
                        reservation.id, reservation.generation, reservation.anchorDomain,
                        reservation.slots, reservation.frequency,
                        "lease expired at " + std::to_string(reservation.lease.expiresAt.nanos()) +
                            "ns and was observed at " + std::to_string(now.nanos()) + "ns");
  }
}

void rollbackAll(detail::RuntimeState& state, const std::vector<SpectrumReservation>& backups) {
  for (const SpectrumReservation& backup : backups) rollbackReservation(state, backup);
}

}  // namespace

Status SpectrumRuntime::renew(ReservationId id, ReservationGeneration generation,
                              Duration extension, const ReservationAuthority& authority, Instant now,
                              std::uint32_t maxRenewals) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  if (extension.nanos() <= 0) {
    return fail(StatusCode::InvalidArgument, "a renewal extension must be a positive duration");
  }
  if (extension.nanos() > kMaxLeaseExtensionNanos) {
    return fail(StatusCode::InvalidArgument, "a renewal extension may not exceed 3650 days");
  }

  const Status authorityStatus = checkReservationAuthority(state.authority, authority);
  if (!authorityStatus.ok()) {
    detail::appendAudit(state, AuditKind::ReplayRejected, now,
                        AllocationOutcome::RefusedStaleAuthority, id, generation,
                        SpectrumDomainId{}, SlotRange{}, FrequencyRange{},
                        "renewal rejected: " + authorityStatus.message);
    return authorityStatus;
  }

  Lookup found = lookup(state, id, generation);
  if (!found.status.ok()) return found.status;
  SpectrumReservation& reservation = *found.reservation;

  if (reservation.state != ReservationState::Reserved &&
      reservation.state != ReservationState::Active) {
    return fail(StatusCode::IllegalTransition,
                "reservation " + typedToken("reservation", id) + " is " +
                    std::string(toToken(reservation.state)) + " and cannot be renewed");
  }
  if (!reservation.lease.validAt(now)) {
    detail::appendAudit(state, AuditKind::ReplayRejected, now,
                        AllocationOutcome::RefusedLeaseExpired, id, generation,
                        reservation.anchorDomain, reservation.slots, reservation.frequency,
                        "renewal rejected: the lease lapsed at " +
                            std::to_string(reservation.lease.expiresAt.nanos()) + "ns");
    state.stats.replayRejections += 1;
    return fail(StatusCode::Refused,
                "the lease lapsed at " + std::to_string(reservation.lease.expiresAt.nanos()) +
                    "ns and cannot be renewed; a fresh allocation request is required");
  }

  std::uint32_t effectiveCap = reservation.lease.maxRenewals;
  if (maxRenewals != 0) {
    if (maxRenewals <= reservation.lease.renewalCount) {
      return fail(StatusCode::LimitExceeded,
                  "the requested renewal cap " + std::to_string(maxRenewals) +
                      " is not above the renewal count " +
                      std::to_string(reservation.lease.renewalCount));
    }
    effectiveCap = maxRenewals;
  }
  if (reservation.lease.maxRenewals != 0 &&
      reservation.lease.renewalCount >= reservation.lease.maxRenewals) {
    return fail(StatusCode::LimitExceeded,
                "reservation " + typedToken("reservation", id) + " reached its renewal cap of " +
                    std::to_string(reservation.lease.maxRenewals));
  }

  std::int64_t newExpiry = 0;
  if (addOverflow(reservation.lease.expiresAt.nanos(), extension.nanos(), newExpiry)) {
    return fail(StatusCode::Overflow, "the renewed lease expiry is not representable");
  }

  const ReservationState original = reservation.state;
  const SpectrumReservation backup = reservation;
  const RuntimeStats backupStats = state.stats;

  std::string error;
  if (!advanceState(state, reservation, ReservationState::Renewing, error)) {
    return fail(StatusCode::IllegalTransition, error);
  }
  reservation.lease.expiresAt = Instant::fromNanos(newExpiry);
  reservation.lease.renewalCount += 1;
  reservation.lease.maxRenewals = effectiveCap;
  reservation.lease.generation.bump();
  reservation.generation.bump();
  reservation.updatedAt = now;
  reservation.lastOperation.bump();
  if (!advanceState(state, reservation, original, error)) {
    return fail(StatusCode::IllegalTransition, error);
  }

  state.stats.renewals += 1;
  state.lastActivityAt = now;
  detail::appendAudit(state, AuditKind::ReservationRenewed, now, AllocationOutcome::Unknown,
                      reservation.id, reservation.generation, reservation.anchorDomain,
                      reservation.slots, reservation.frequency,
                      "lease extended to " + std::to_string(reservation.lease.expiresAt.nanos()) +
                          "ns; renewal " + std::to_string(reservation.lease.renewalCount));

  const Status persisted = detail::persistAfterMutation(state);
  if (!persisted.ok()) {
    state.stats = backupStats;
    rollbackReservation(state, backup);
    return persisted;
  }
  return okStatus();
}

Status SpectrumRuntime::activate(ReservationId id, ReservationGeneration generation,
                                 const ActivationAuthority& authority, Instant now) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  const Status authorityStatus = checkActivationAuthority(state.authority, authority);
  if (!authorityStatus.ok()) {
    detail::appendAudit(state, AuditKind::ReplayRejected, now,
                        AllocationOutcome::RefusedStaleAuthority, id, generation,
                        SpectrumDomainId{}, SlotRange{}, FrequencyRange{},
                        "activation rejected: " + authorityStatus.message);
    return authorityStatus;
  }

  Lookup found = lookup(state, id, generation);
  if (!found.status.ok()) return found.status;
  SpectrumReservation& reservation = *found.reservation;

  if (reservation.state != ReservationState::Reserved) {
    return fail(StatusCode::IllegalTransition,
                "reservation " + typedToken("reservation", id) + " is " +
                    std::string(toToken(reservation.state)) + " and cannot be activated");
  }
  if (!reservation.lease.validAt(now)) {
    detail::appendAudit(state, AuditKind::ReplayRejected, now,
                        AllocationOutcome::RefusedLeaseExpired, id, generation,
                        reservation.anchorDomain, reservation.slots, reservation.frequency,
                        "activation rejected: the lease lapsed");
    state.stats.replayRejections += 1;
    return fail(StatusCode::Refused, "the lease lapsed at " +
                                         std::to_string(reservation.lease.expiresAt.nanos()) +
                                         "ns and cannot be activated");
  }

  const SpectrumReservation backup = reservation;
  const RuntimeStats backupStats = state.stats;

  std::string error;
  if (!advanceState(state, reservation, ReservationState::Activating, error) ||
      !advanceState(state, reservation, ReservationState::Active, error)) {
    return fail(StatusCode::IllegalTransition, error);
  }
  reservation.activatedAt = now;
  reservation.updatedAt = now;
  reservation.needsRevalidation = false;
  reservation.lastOperation.bump();

  state.stats.activations += 1;
  state.lastActivityAt = now;
  detail::appendAudit(state, AuditKind::ReservationActivated, now, AllocationOutcome::Unknown,
                      reservation.id, reservation.generation, reservation.anchorDomain,
                      reservation.slots, reservation.frequency,
                      "activated under controller epoch " +
                          std::to_string(state.fence.epoch.raw()) + " incarnation " +
                          std::to_string(state.fence.incarnation.raw()));

  const Status persisted = detail::persistAfterMutation(state);
  if (!persisted.ok()) {
    state.stats = backupStats;
    rollbackReservation(state, backup);
    return persisted;
  }
  return okStatus();
}

Status SpectrumRuntime::deactivate(ReservationId id, ReservationGeneration generation,
                                   const ActivationAuthority& authority, Instant now) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  const Status authorityStatus = checkActivationAuthority(state.authority, authority);
  if (!authorityStatus.ok()) {
    detail::appendAudit(state, AuditKind::ReplayRejected, now,
                        AllocationOutcome::RefusedStaleAuthority, id, generation,
                        SpectrumDomainId{}, SlotRange{}, FrequencyRange{},
                        "deactivation rejected: " + authorityStatus.message);
    return authorityStatus;
  }

  Lookup found = lookup(state, id, generation);
  if (!found.status.ok()) return found.status;
  SpectrumReservation& reservation = *found.reservation;

  if (reservation.state != ReservationState::Active) {
    return fail(StatusCode::IllegalTransition,
                "reservation " + typedToken("reservation", id) + " is " +
                    std::string(toToken(reservation.state)) + " and cannot be deactivated");
  }

  const SpectrumReservation backup = reservation;
  const RuntimeStats backupStats = state.stats;

  std::string error;
  if (!advanceState(state, reservation, ReservationState::Deactivating, error) ||
      !advanceState(state, reservation, ReservationState::Reserved, error)) {
    return fail(StatusCode::IllegalTransition, error);
  }
  reservation.deactivatedAt = now;
  reservation.updatedAt = now;
  reservation.lastOperation.bump();

  state.stats.deactivations += 1;
  state.lastActivityAt = now;
  detail::appendAudit(state, AuditKind::ReservationDeactivated, now, AllocationOutcome::Unknown,
                      reservation.id, reservation.generation, reservation.anchorDomain,
                      reservation.slots, reservation.frequency,
                      "returned to RESERVED; spectrum remains owned until release, expiry or "
                      "reclamation");

  const Status persisted = detail::persistAfterMutation(state);
  if (!persisted.ok()) {
    state.stats = backupStats;
    rollbackReservation(state, backup);
    return persisted;
  }
  return okStatus();
}

Status SpectrumRuntime::release(ReservationId id, ReservationGeneration generation,
                                const ReleaseAuthority& authority, Instant now) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  const Status authorityStatus = checkReleaseAuthority(state.authority, authority);
  if (!authorityStatus.ok()) {
    detail::appendAudit(state, AuditKind::ReplayRejected, now,
                        AllocationOutcome::RefusedStaleAuthority, id, generation,
                        SpectrumDomainId{}, SlotRange{}, FrequencyRange{},
                        "release rejected: " + authorityStatus.message);
    return authorityStatus;
  }

  Lookup found = lookup(state, id, generation);
  if (!found.status.ok()) return found.status;
  SpectrumReservation& reservation = *found.reservation;

  if (!isLiveState(reservation.state)) {
    return fail(StatusCode::IllegalTransition,
                "reservation " + typedToken("reservation", id) + " is " +
                    std::string(toToken(reservation.state)) +
                    " and owns no spectrum; duplicate release never creates capacity");
  }

  const SpectrumReservation backup = reservation;
  const RuntimeStats backupStats = state.stats;

  std::string error;
  if (!advanceState(state, reservation, ReservationState::Releasing, error) ||
      !advanceState(state, reservation, ReservationState::Released, error)) {
    return fail(StatusCode::IllegalTransition, error);
  }
  reservation.releasedAt = now;
  reservation.updatedAt = now;
  reservation.lastOperation.bump();
  detail::removeLiveReservation(state, reservation.id);

  state.stats.releases += 1;
  state.lastActivityAt = now;
  detail::appendAudit(state, AuditKind::ReservationReleased, now, AllocationOutcome::Unknown,
                      reservation.id, reservation.generation, reservation.anchorDomain,
                      reservation.slots, reservation.frequency,
                      "released " + std::to_string(reservation.slots.count) + " slot(s) at " +
                          std::to_string(reservation.slots.first));

  const Status persisted = detail::persistAfterMutation(state);
  if (!persisted.ok()) {
    state.stats = backupStats;
    rollbackReservation(state, backup);
    return persisted;
  }
  return okStatus();
}

ReclaimReport SpectrumRuntime::expireLeases(Instant now) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  ReclaimReport report;
  report.evaluatedAt = now;
  report.fence = state.fence;
  report.scanned = state.reservations.size();

  std::vector<SpectrumReservation> backups;
  markLapsed(state, now, report, backups);
  state.stats.expirationSweeps += 1;
  state.lastActivityAt = now;

  const Status persisted = detail::persistAfterMutation(state);
  if (!persisted.ok()) {
    rollbackAll(state, backups);
    ReclaimReport failed;
    failed.status = persisted;
    failed.evaluatedAt = now;
    failed.fence = state.fence;
    failed.scanned = report.scanned;
    return failed;
  }
  report.status = okStatus();
  return report;
}

ReclaimReport SpectrumRuntime::reclaimExpired(Instant now) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  ReclaimReport report;
  report.evaluatedAt = now;
  report.fence = state.fence;
  report.scanned = state.reservations.size();

  std::vector<SpectrumReservation> backups;
  markLapsed(state, now, report, backups);

  for (auto& entry : state.reservations) {
    SpectrumReservation& reservation = entry.second;
    if (reservation.state != ReservationState::Expired) continue;
    backups.push_back(reservation);
    std::string error;
    if (!advanceState(state, reservation, ReservationState::Reclaiming, error) ||
        !advanceState(state, reservation, ReservationState::Reclaimed, error)) {
      continue;
    }
    reservation.reclaimedAt = now;
    reservation.updatedAt = now;
    reservation.lastOperation.bump();
    detail::removeLiveReservation(state, reservation.id);
    report.reclaimed.push_back(reservation.id);
    state.stats.reclamations += 1;
    detail::appendAudit(state, AuditKind::ReservationReclaimed, now, AllocationOutcome::Unknown,
                        reservation.id, reservation.generation, reservation.anchorDomain,
                        reservation.slots, reservation.frequency,
                        "reclaimed " + std::to_string(reservation.slots.count) + " slot(s) at " +
                            std::to_string(reservation.slots.first));
  }

  state.stats.expirationSweeps += 1;
  state.lastActivityAt = now;

  const Status persisted = detail::persistAfterMutation(state);
  if (!persisted.ok()) {
    rollbackAll(state, backups);
    ReclaimReport failed;
    failed.status = persisted;
    failed.evaluatedAt = now;
    failed.fence = state.fence;
    failed.scanned = report.scanned;
    return failed;
  }
  report.status = okStatus();
  return report;
}

}  // namespace wavelength_fabric
