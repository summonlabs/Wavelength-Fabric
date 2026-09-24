#include <algorithm>
#include <string>
#include <utility>

#include "impl.hpp"

namespace wavelength_fabric {

namespace {

// Applies a lifecycle transition, rejecting anything the state machine does
// not permit. The commit path uses this so that a commit is never observable
// in a half-applied form.
[[nodiscard]] bool advanceState(detail::RuntimeState& state, SpectrumReservation& reservation,
                                ReservationState next, std::string& error) {
  if (reservation.state == next) {
    error = "reservation is already " + std::string(toToken(next));
    return false;
  }
  if (!isLegalTransition(reservation.state, next)) {
    error = "transition from " + std::string(toToken(reservation.state)) + " to " +
            std::string(toToken(next)) + " is not permitted: " +
            std::string(illegalTransitionReason(reservation.state, next));
    detail::appendAudit(state, AuditKind::ReplayRejected, state.lastActivityAt,
                        AllocationOutcome::RefusedNotPermitted, reservation.id,
                        reservation.generation, reservation.anchorDomain, reservation.slots,
                        reservation.frequency, error);
    return false;
  }
  reservation.state = next;
  return true;
}

[[nodiscard]] Instant leaseStart(const SpectrumRequest& request) noexcept {
  return request.notBefore > request.requestedAt ? request.notBefore : request.requestedAt;
}

}  // namespace

AllocationDecision SpectrumRuntime::allocate(const SpectrumRequest& request) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  AllocationDecision decision;
  const detail::Evaluation evaluation = detail::evaluate(state, request);

  decision.outcome = evaluation.outcome;
  decision.explanation.outcome = evaluation.outcome;
  decision.explanation.fence = state.fence;
  decision.explanation.candidatesEnumerated = evaluation.candidatesEnumerated;
  decision.explanation.candidatesRejected = evaluation.candidatesRejected;
  decision.explanation.candidatesOmitted = evaluation.candidatesOmitted;
  decision.explanation.candidateOrdinal = evaluation.candidateOrdinal;
  decision.explanation.selected = evaluation.selected;
  decision.explanation.rejected = evaluation.rejected;
  decision.explanation.conflicts = evaluation.conflicts;
  decision.explanation.reasons = evaluation.reasons;

  state.stats.candidatesEvaluated += evaluation.candidatesEnumerated;
  if (evaluation.candidatesOmitted > 0) state.stats.enumerationTruncations += 1;

  const Instant at = request.requestedAt.isZero() ? systemNow() : request.requestedAt;

  // ---- idempotent replay --------------------------------------------------
  if (evaluation.replay) {
    const SpectrumReservation& existing = evaluation.replayValue;
    decision.status = okStatus();
    decision.reservation = existing.id;
    decision.generation = existing.generation;
    decision.explanation.reservation = existing.id;
    decision.explanation.generation = existing.generation;
    decision.explanation.selected = detail::candidateFromReservation(existing);
    detail::appendAudit(state, AuditKind::RequestEvaluated, at, AllocationOutcome::Allocated,
                        existing.id, existing.generation, existing.anchorDomain, existing.slots,
                        existing.frequency,
                        "idempotent replay: the request identity is already committed");
    return decision;
  }

  // ---- refusal ------------------------------------------------------------
  if (!evaluation.eligible) {
    decision.status = detail::statusForOutcome(evaluation.outcome, evaluation.reasons);
    detail::appendAudit(state, AuditKind::AllocationRefused, at, evaluation.outcome,
                        ReservationId{}, ReservationGeneration{},
                        request.domains.empty() ? SpectrumDomainId{} : request.domains.front(),
                        SlotRange{}, FrequencyRange{},
                        evaluation.reasons.empty() ? std::string("refused")
                                                   : evaluation.reasons.front());
    state.stats.allocationsRefused += 1;
    state.lastActivityAt = at;
    return decision;
  }

  // ---- capacity bound -----------------------------------------------------
  if (state.reservations.size() >= state.config.maxReservations) {
    detail::pruneTerminalReservations(state);
    if (state.reservations.size() >= state.config.maxReservations) {
      decision.outcome = AllocationOutcome::RefusedLimitExceeded;
      decision.explanation.outcome = decision.outcome;
      decision.explanation.reasons.insert(
          decision.explanation.reasons.begin(),
          "reservation table is at its configured bound of " +
              std::to_string(state.config.maxReservations) + " records");
      decision.status = fail(StatusCode::LimitExceeded, decision.explanation.reasons.front());
      detail::appendAudit(state, AuditKind::AllocationRefused, at, decision.outcome,
                          ReservationId{}, ReservationGeneration{}, request.domains.front(),
                          SlotRange{}, FrequencyRange{}, decision.explanation.reasons.front());
      state.stats.allocationsRefused += 1;
      return decision;
    }
  }

  // ---- supersede an older generation of the same request identity ---------
  bool superseded = false;
  SpectrumReservation supersededValue;
  const auto existingRequest = state.requestIndex.find(request.requestId);
  if (existingRequest != state.requestIndex.end()) {
    const auto previousIt = state.reservations.find(existingRequest->second);
    if (previousIt != state.reservations.end() && isLiveState(previousIt->second.state)) {
      supersededValue = previousIt->second;
      SpectrumReservation& previous = previousIt->second;
      std::string error;
      if (!advanceState(state, previous, ReservationState::Superseded, error)) {
        decision.status = fail(StatusCode::IllegalTransition, error);
        decision.outcome = AllocationOutcome::RefusedNotPermitted;
        decision.explanation.outcome = decision.outcome;
        decision.explanation.reasons.insert(decision.explanation.reasons.begin(), error);
        return decision;
      }
      previous.updatedAt = at;
      previous.lastOperation.bump();
      detail::removeLiveReservation(state, previous.id);
      detail::appendAudit(state, AuditKind::ReservationSuperseded, at,
                          AllocationOutcome::Unknown, previous.id, previous.generation,
                          previous.anchorDomain, previous.slots, previous.frequency,
                          "superseded by request generation " +
                              std::to_string(request.requestGeneration.raw()));
      superseded = true;
    }
  }

  // ---- commit -------------------------------------------------------------
  const Instant start = leaseStart(request);
  std::int64_t expiryNanos = 0;
  if (addOverflow(start.nanos(), request.leaseDuration.nanos(), expiryNanos)) {
    if (superseded) {
      state.reservations[supersededValue.id] = supersededValue;
      detail::addLiveReservation(state, supersededValue);
    }
    decision.status = fail(StatusCode::InvalidArgument, "lease expiry is not representable");
    decision.outcome = AllocationOutcome::RefusedInvalidRequest;
    decision.explanation.outcome = decision.outcome;
    decision.explanation.reasons.insert(decision.explanation.reasons.begin(),
                                        "lease expiry is not representable");
    return decision;
  }

  SpectrumReservation reservation;
  reservation.id = state.nextReservationId;
  state.nextReservationId.bump();
  reservation.generation = ReservationGeneration(1);
  reservation.requestId = request.requestId;
  reservation.requestGeneration = request.requestGeneration;
  reservation.owner = request.owner;
  reservation.ownerGeneration = request.ownerGeneration;
  reservation.domains = request.domains;
  reservation.domainGenerations = request.domainGenerations;
  reservation.anchorDomain = evaluation.selected.anchorDomain;
  reservation.grid = request.grid;
  reservation.gridGeneration = request.gridGeneration;
  reservation.slots = evaluation.selected.slots;
  reservation.frequency = evaluation.selected.frequency;
  reservation.perDomainSlots = evaluation.selected.perDomainSlots;
  reservation.crossGrid = evaluation.selected.crossGrid;
  reservation.contiguityRequired = request.contiguity == ContiguityRequirement::Required;
  reservation.continuityRequired = request.continuity == ContinuityRequirement::Required;
  reservation.guardBandMhz = request.guardBandMhz;
  reservation.lease.generation = LeaseGeneration(1);
  reservation.lease.grantedAt = start;
  reservation.lease.expiresAt = Instant::fromNanos(expiryNanos);
  reservation.lease.renewalCount = 0;
  reservation.lease.maxRenewals = request.maxRenewals;
  reservation.createdAt = start;
  reservation.updatedAt = at;
  reservation.commitFence = state.fence;
  reservation.lastOperation = OperationGeneration(1);
  const auto anchorCapability = state.capabilities.find(reservation.anchorDomain);
  if (anchorCapability != state.capabilities.end()) {
    reservation.capabilityGeneration = anchorCapability->second.generation;
  }
  reservation.detail = "committed under controller epoch " +
                       std::to_string(state.fence.epoch.raw()) + " incarnation " +
                       std::to_string(state.fence.incarnation.raw());

  // A reservation is born in None and is driven through the guarded state
  // machine: None -> Requested -> Evaluated -> Committing -> Reserved.
  std::string transitionError;
  if (!advanceState(state, reservation, ReservationState::Requested, transitionError) ||
      !advanceState(state, reservation, ReservationState::Evaluated, transitionError) ||
      !advanceState(state, reservation, ReservationState::Committing, transitionError) ||
      !advanceState(state, reservation, ReservationState::Reserved, transitionError)) {
    if (superseded) {
      state.reservations[supersededValue.id] = supersededValue;
      detail::addLiveReservation(state, supersededValue);
    }
    decision.status = fail(StatusCode::IllegalTransition, transitionError);
    decision.outcome = AllocationOutcome::RefusedNotPermitted;
    decision.explanation.outcome = decision.outcome;
    decision.explanation.reasons.insert(decision.explanation.reasons.begin(), transitionError);
    return decision;
  }

  const bool hadRequestIndex = existingRequest != state.requestIndex.end();
  const ReservationId previousRequestReservation =
      hadRequestIndex ? existingRequest->second : ReservationId{};

  const ReservationId committedId = reservation.id;
  state.reservations.emplace(committedId, reservation);
  state.requestIndex[request.requestId] = committedId;
  detail::addLiveReservation(state, reservation);
  state.lastActivityAt = at;
  state.stats.allocationsCommitted += 1;
  detail::appendAudit(state, AuditKind::AllocationCommitted, at, AllocationOutcome::Allocated,
                      committedId, reservation.generation, reservation.anchorDomain,
                      reservation.slots, reservation.frequency,
                      "committed " + std::to_string(reservation.slots.count) + " slot(s) at " +
                          std::to_string(reservation.slots.first) + " across " +
                          std::to_string(reservation.domains.size()) + " domain(s)");
  detail::pruneTerminalReservations(state);

  decision.status = okStatus();
  decision.reservation = committedId;
  decision.generation = reservation.generation;
  decision.explanation.reservation = committedId;
  decision.explanation.generation = reservation.generation;

  // ---- durable commit boundary -------------------------------------------
  if (state.config.durableCommits && !state.config.statePath.empty()) {
    const Status saved = detail::saveLocked(state);
    if (!saved.ok()) {
      // Roll the commit back exactly. No ownership survives a commit that did
      // not cross the durable boundary.
      state.reservations.erase(committedId);
      detail::removeLiveReservation(state, committedId);
      if (hadRequestIndex) {
        state.requestIndex[request.requestId] = previousRequestReservation;
      } else {
        state.requestIndex.erase(request.requestId);
      }
      if (superseded) {
        state.reservations[supersededValue.id] = supersededValue;
        detail::addLiveReservation(state, supersededValue);
      }
      state.stats.allocationsCommitted -= 1;
      decision.status = saved;
      decision.outcome = AllocationOutcome::Unknown;
      decision.explanation.outcome = AllocationOutcome::Unknown;
      decision.reservation = ReservationId{};
      decision.generation = ReservationGeneration{};
      decision.explanation.reservation = ReservationId{};
      decision.explanation.generation = ReservationGeneration{};
      decision.explanation.reasons.insert(
          decision.explanation.reasons.begin(),
          "durable commit failed; the allocation was rolled back and no spectrum is owned: " +
              saved.message);
      detail::appendAudit(state, AuditKind::PersistenceFailed, at, AllocationOutcome::Unknown,
                          ReservationId{}, ReservationGeneration{}, reservation.anchorDomain,
                          reservation.slots, reservation.frequency,
                          "durable commit rolled back: " + saved.message);
      return decision;
    }
  }

  return decision;
}

DecisionExplanation SpectrumRuntime::explain(const SpectrumRequest& request) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  const detail::RuntimeState& state = *state_;

  const detail::Evaluation evaluation = detail::evaluate(state, request);
  DecisionExplanation explanation;
  explanation.outcome = evaluation.outcome;
  explanation.fence = state.fence;
  explanation.candidatesEnumerated = evaluation.candidatesEnumerated;
  explanation.candidatesRejected = evaluation.candidatesRejected;
  explanation.candidatesOmitted = evaluation.candidatesOmitted;
  explanation.candidateOrdinal = evaluation.candidateOrdinal;
  explanation.selected = evaluation.selected;
  explanation.rejected = evaluation.rejected;
  explanation.conflicts = evaluation.conflicts;
  explanation.reasons = evaluation.reasons;

  if (evaluation.replay) {
    explanation.reservation = evaluation.replayValue.id;
    explanation.generation = evaluation.replayValue.generation;
    explanation.selected = detail::candidateFromReservation(evaluation.replayValue);
  }
  return explanation;
}

namespace detail {

Status statusForOutcome(AllocationOutcome outcome, const std::vector<std::string>& reasons) {
  const std::string message = reasons.empty() ? std::string("request refused") : reasons.front();
  switch (outcome) {
    case AllocationOutcome::Allocated:
      return okStatus();
    case AllocationOutcome::RefusedUnsupported:
      return fail(StatusCode::Unsupported, message);
    case AllocationOutcome::RefusedUnknownCapability:
      return fail(StatusCode::Unknown, message);
    case AllocationOutcome::RefusedStaleGeneration:
      return fail(StatusCode::StaleGeneration, message);
    case AllocationOutcome::RefusedStaleIncarnation:
      return fail(StatusCode::StaleIncarnation, message);
    case AllocationOutcome::RefusedStaleEpoch:
      return fail(StatusCode::StaleEpoch, message);
    case AllocationOutcome::RefusedStaleAuthority:
      return fail(StatusCode::StaleAuthority, message);
    case AllocationOutcome::RefusedStaleCapability:
      return fail(StatusCode::StaleGeneration, message);
    case AllocationOutcome::RefusedDuplicate:
      return fail(StatusCode::Duplicate, message);
    case AllocationOutcome::RefusedLimitExceeded:
      return fail(StatusCode::LimitExceeded, message);
    case AllocationOutcome::RefusedUnknownDomain:
      return fail(StatusCode::NotFound, message);
    case AllocationOutcome::RefusedInvalidRequest:
      return fail(StatusCode::InvalidArgument, message);
    case AllocationOutcome::RefusedChannelWidth:
      return fail(StatusCode::InvalidArgument, message);
    case AllocationOutcome::RefusedConversion:
      return fail(StatusCode::NotPermitted, message);
    case AllocationOutcome::RefusedExclusion:
      return fail(StatusCode::Excluded, message);
    case AllocationOutcome::RefusedConflict:
      return fail(StatusCode::Conflict, message);
    case AllocationOutcome::RefusedContiguity:
    case AllocationOutcome::RefusedContinuity:
    case AllocationOutcome::RefusedConstraint:
      return fail(StatusCode::Refused, message);
    case AllocationOutcome::RefusedNoCapacity:
      return fail(StatusCode::Unavailable, message);
    case AllocationOutcome::RefusedLeaseExpired:
      return fail(StatusCode::Refused, message);
    case AllocationOutcome::RefusedNotPermitted:
      return fail(StatusCode::NotPermitted, message);
    case AllocationOutcome::Unknown:
      return fail(StatusCode::Unknown, message);
  }
  return fail(StatusCode::Unknown, message);
}

}  // namespace detail
}  // namespace wavelength_fabric
