#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "impl.hpp"

namespace wavelength_fabric {

namespace {

[[nodiscard]] std::uint64_t processIdValue() noexcept {
#ifdef _WIN32
  return static_cast<std::uint64_t>(::_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ull;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

}  // namespace

namespace detail {

// A fresh incarnation for every runtime construction, including a restart that
// recovers no durable state. Process id, wall clock and a process-local
// sequence are mixed so that two runtimes never share an incarnation.
std::uint64_t nextIncarnation() noexcept {
  static std::atomic<std::uint64_t> sequence{0};
  const std::uint64_t step = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  const std::uint64_t now =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
  std::uint64_t value = mix64(processIdValue() ^ mix64(now) ^ mix64(step));
  value %= kMaxFenceValue;
  if (value == 0) value = 1;
  return value;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SpectrumRuntime::SpectrumRuntime(RuntimeConfig config) : state_(new detail::RuntimeState()) {
  detail::RuntimeState& state = *state_;
  state.config = std::move(config);

  if (state.config.maxCandidatesPerRequest == 0) state.config.maxCandidatesPerRequest = 1;
  if (state.config.maxCandidatesPerRequest > kMaxCandidatesPerRequest) {
    state.config.maxCandidatesPerRequest = kMaxCandidatesPerRequest;
  }
  if (state.config.maxRejectedExplanations > 256) state.config.maxRejectedExplanations = 256;
  if (state.config.maxAuditRecords == 0) state.config.maxAuditRecords = 1;
  if (state.config.maxAuditPageSize == 0) state.config.maxAuditPageSize = 1;
  if (state.config.maxReservations == 0) state.config.maxReservations = 1;
  if (state.config.maxGrids == 0) state.config.maxGrids = 1;
  if (state.config.maxDomains == 0) state.config.maxDomains = 1;
  if (state.config.maxExclusionDomains == 0) state.config.maxExclusionDomains = 1;

  state.fence.epoch = ControllerEpoch(1);
  state.fence.incarnation = ControllerIncarnation(detail::nextIncarnation());
  state.runtimeGeneration = RuntimeGeneration(1);
  state.recoveryGeneration = RecoveryGeneration(0);
  state.auditSequence = AuditSequence(0);
  state.nextReservationId = ReservationId(1);

  state.authority.eligibilityGeneration = EligibilityAuthorityGeneration(1);
  state.authority.reservationGeneration = ReservationAuthorityGeneration(1);
  state.authority.activationGeneration = ActivationAuthorityGeneration(1);
  state.authority.releaseGeneration = ReleaseAuthorityGeneration(1);
  state.authority.fence = state.fence;

  detail::appendAudit(state, AuditKind::RuntimeOpened, systemNow(), AllocationOutcome::Unknown,
                      ReservationId{}, ReservationGeneration{}, SpectrumDomainId{}, SlotRange{},
                      FrequencyRange{}, "runtime constructed");
}

SpectrumRuntime::~SpectrumRuntime() = default;

ControllerFence SpectrumRuntime::fence() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return state_->fence;
}

ControllerEpoch SpectrumRuntime::epoch() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return state_->fence.epoch;
}

ControllerIncarnation SpectrumRuntime::incarnation() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return state_->fence.incarnation;
}

RuntimeGeneration SpectrumRuntime::runtimeGeneration() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return state_->runtimeGeneration;
}

RecoveryGeneration SpectrumRuntime::recoveryGeneration() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return state_->recoveryGeneration;
}

AuthorityState SpectrumRuntime::authorityState() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return state_->authority;
}

RuntimeConfig SpectrumRuntime::config() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return state_->config;
}

Status SpectrumRuntime::advanceAuthority(const AuthorityState& next) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  Status fenceStatus = checkFence(state.authority, next.fence);
  if (!fenceStatus.ok()) {
    detail::appendAudit(state, AuditKind::ReplayRejected, systemNow(),
                        AllocationOutcome::RefusedStaleAuthority, ReservationId{},
                        ReservationGeneration{}, SpectrumDomainId{}, SlotRange{}, FrequencyRange{},
                        "authority advance rejected: " + fenceStatus.message);
    return fenceStatus;
  }

  if (!next.eligibilityGeneration.valid() || !next.reservationGeneration.valid() ||
      !next.activationGeneration.valid() || !next.releaseGeneration.valid()) {
    return fail(StatusCode::InvalidArgument, "authority generations must be non-zero");
  }
  if (next.eligibilityGeneration < state.authority.eligibilityGeneration ||
      next.reservationGeneration < state.authority.reservationGeneration ||
      next.activationGeneration < state.authority.activationGeneration ||
      next.releaseGeneration < state.authority.releaseGeneration) {
    return fail(StatusCode::StaleGeneration, "authority generations never move backwards");
  }
  const bool moved = next.eligibilityGeneration != state.authority.eligibilityGeneration ||
                     next.reservationGeneration != state.authority.reservationGeneration ||
                     next.activationGeneration != state.authority.activationGeneration ||
                     next.releaseGeneration != state.authority.releaseGeneration;
  if (!moved) {
    return fail(StatusCode::Duplicate, "authority state is unchanged");
  }

  state.authority = next;
  detail::appendAudit(state, AuditKind::AuthorityAdvanced, systemNow(), AllocationOutcome::Unknown,
                      ReservationId{}, ReservationGeneration{}, SpectrumDomainId{}, SlotRange{},
                      FrequencyRange{},
                      "eligibility=" + std::to_string(next.eligibilityGeneration.raw()) +
                          " reservation=" + std::to_string(next.reservationGeneration.raw()) +
                          " activation=" + std::to_string(next.activationGeneration.raw()) +
                          " release=" + std::to_string(next.releaseGeneration.raw()));
  return okStatus();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

Status SpectrumRuntime::registerGrid(const ChannelGrid& grid) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  Status shape = validateGrid(grid);
  if (!shape.ok()) return shape;

  const auto existing = state.grids.find(grid.id);
  if (existing != state.grids.end()) {
    if (grid.generation == existing->second.generation) {
      return fail(StatusCode::Duplicate, "grid " + typedToken("grid", grid.id) +
                                             " is already registered at generation " +
                                             std::to_string(grid.generation.raw()));
    }
    if (grid.generation < existing->second.generation) {
      return fail(StatusCode::StaleGeneration,
                  "grid " + typedToken("grid", grid.id) + " is registered at generation " +
                      std::to_string(existing->second.generation.raw()) + "; generation " +
                      std::to_string(grid.generation.raw()) + " is stale");
    }
  } else if (state.grids.size() >= state.config.maxGrids) {
    return fail(StatusCode::LimitExceeded, "grid registry is at its configured bound of " +
                                               std::to_string(state.config.maxGrids));
  }

  state.grids[grid.id] = grid;
  detail::appendAudit(state, AuditKind::GridRegistered, systemNow(), AllocationOutcome::Unknown,
                      ReservationId{}, ReservationGeneration{}, SpectrumDomainId{}, SlotRange{},
                      FrequencyRange{}, describeGrid(grid));
  return okStatus();
}

Status SpectrumRuntime::registerSpan(const Span& span) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  Status shape = validateSpan(span);
  if (!shape.ok()) return shape;

  const auto existing = state.spans.find(span.id);
  if (existing != state.spans.end()) {
    if (span.generation == existing->second.generation) {
      return fail(StatusCode::Duplicate, "span " + typedToken("span", span.id) +
                                             " is already registered at generation " +
                                             std::to_string(span.generation.raw()));
    }
    if (span.generation < existing->second.generation) {
      return fail(StatusCode::StaleGeneration,
                  "span " + typedToken("span", span.id) + " is registered at generation " +
                      std::to_string(existing->second.generation.raw()));
    }
  }
  state.spans[span.id] = span;
  detail::appendAudit(state, AuditKind::SpanRegistered, systemNow(), AllocationOutcome::Unknown,
                      ReservationId{}, ReservationGeneration{}, SpectrumDomainId{}, SlotRange{},
                      FrequencyRange{}, "span " + typedToken("span", span.id) + " generation " +
                                            std::to_string(span.generation.raw()));
  return okStatus();
}

Status SpectrumRuntime::registerPort(const OpticalPort& port) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  Status shape = validatePort(port);
  if (!shape.ok()) return shape;

  const auto existing = state.ports.find(port.id);
  if (existing != state.ports.end()) {
    if (port.generation == existing->second.generation) {
      return fail(StatusCode::Duplicate, "port " + typedToken("port", port.id) +
                                             " is already registered at generation " +
                                             std::to_string(port.generation.raw()));
    }
    if (port.generation < existing->second.generation) {
      return fail(StatusCode::StaleGeneration,
                  "port " + typedToken("port", port.id) + " is registered at generation " +
                      std::to_string(existing->second.generation.raw()));
    }
  }
  state.ports[port.id] = port;
  detail::appendAudit(state, AuditKind::PortRegistered, systemNow(), AllocationOutcome::Unknown,
                      ReservationId{}, ReservationGeneration{}, SpectrumDomainId{}, SlotRange{},
                      FrequencyRange{}, "port " + typedToken("port", port.id) + " generation " +
                                            std::to_string(port.generation.raw()));
  return okStatus();
}

Status SpectrumRuntime::registerDomain(const SpectrumDomain& domain) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  Status shape = validateDomain(domain);
  if (!shape.ok()) return shape;

  const auto grid = state.grids.find(domain.grid);
  if (grid == state.grids.end()) {
    return fail(StatusCode::NotFound,
                "domain " + typedToken("domain", domain.id) + " references unregistered grid " +
                    typedToken("grid", domain.grid));
  }
  if (grid->second.generation != domain.gridGeneration) {
    return fail(StatusCode::StaleGeneration,
                "domain " + typedToken("domain", domain.id) + " expects grid generation " +
                    std::to_string(domain.gridGeneration.raw()) + " but the registered grid is at " +
                    std::to_string(grid->second.generation.raw()));
  }
  if (domain.span.valid() && state.spans.find(domain.span) == state.spans.end()) {
    return fail(StatusCode::NotFound, "domain " + typedToken("domain", domain.id) +
                                          " references unregistered span " +
                                          typedToken("span", domain.span));
  }

  const auto existing = state.domains.find(domain.id);
  if (existing != state.domains.end()) {
    if (domain.generation == existing->second.generation) {
      return fail(StatusCode::Duplicate, "domain " + typedToken("domain", domain.id) +
                                             " is already registered at generation " +
                                             std::to_string(domain.generation.raw()));
    }
    if (domain.generation < existing->second.generation) {
      return fail(StatusCode::StaleGeneration,
                  "domain " + typedToken("domain", domain.id) + " is registered at generation " +
                      std::to_string(existing->second.generation.raw()) + "; generation " +
                      std::to_string(domain.generation.raw()) + " is stale");
    }
  } else if (state.domains.size() >= state.config.maxDomains) {
    return fail(StatusCode::LimitExceeded, "domain registry is at its configured bound of " +
                                               std::to_string(state.config.maxDomains));
  }

  state.domains[domain.id] = domain;
  detail::appendAudit(state, AuditKind::DomainRegistered, systemNow(), AllocationOutcome::Unknown,
                      ReservationId{}, ReservationGeneration{}, domain.id, SlotRange{},
                      FrequencyRange{},
                      "domain class=" + std::string(toToken(domain.klass)) + " grid=" +
                          typedToken("grid", domain.grid) + " generation " +
                          std::to_string(domain.generation.raw()));
  return okStatus();
}

Status SpectrumRuntime::registerExclusionDomain(const ExclusionDomain& exclusion) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  Status shape = validateExclusionDomain(exclusion);
  if (!shape.ok()) return shape;

  for (const SpectrumDomainId member : exclusion.members) {
    if (state.domains.find(member) == state.domains.end()) {
      return fail(StatusCode::NotFound, "exclusion domain " + typedToken("exclusion", exclusion.id) +
                                            " references unregistered domain " +
                                            typedToken("domain", member));
    }
  }

  const auto existing = state.exclusionDomains.find(exclusion.id);
  if (existing != state.exclusionDomains.end()) {
    if (exclusion.generation == existing->second.generation) {
      return fail(StatusCode::Duplicate, "exclusion domain " + typedToken("exclusion", exclusion.id) +
                                             " is already registered at generation " +
                                             std::to_string(exclusion.generation.raw()));
    }
    if (exclusion.generation < existing->second.generation) {
      return fail(StatusCode::StaleGeneration,
                  "exclusion domain " + typedToken("exclusion", exclusion.id) +
                      " is registered at generation " +
                      std::to_string(existing->second.generation.raw()));
    }
  } else if (state.exclusionDomains.size() >= state.config.maxExclusionDomains) {
    return fail(StatusCode::LimitExceeded, "exclusion domain registry is at its configured bound");
  }

  // Replacing an exclusion domain requires removing the previous membership.
  if (existing != state.exclusionDomains.end()) {
    for (const SpectrumDomainId member : existing->second.members) {
      auto list = state.domainExclusions.find(member);
      if (list == state.domainExclusions.end()) continue;
      auto& ids = list->second;
      ids.erase(std::remove(ids.begin(), ids.end(), exclusion.id), ids.end());
      if (ids.empty()) state.domainExclusions.erase(list);
    }
  }

  for (const SpectrumDomainId member : exclusion.members) {
    auto& list = state.domainExclusions[member];
    if (list.size() >= detail::kMaxExclusionDomainsPerMember &&
        std::find(list.begin(), list.end(), exclusion.id) == list.end()) {
      return fail(StatusCode::LimitExceeded,
                  "domain " + typedToken("domain", member) + " already belongs to " +
                      std::to_string(list.size()) + " exclusion domains");
    }
  }

  state.exclusionDomains[exclusion.id] = exclusion;
  for (const SpectrumDomainId member : exclusion.members) {
    auto& list = state.domainExclusions[member];
    if (std::find(list.begin(), list.end(), exclusion.id) == list.end()) {
      list.push_back(exclusion.id);
      std::sort(list.begin(), list.end());
    }
  }

  detail::appendAudit(state, AuditKind::ExclusionDomainRegistered, systemNow(),
                      AllocationOutcome::Unknown, ReservationId{}, ReservationGeneration{},
                      SpectrumDomainId{}, SlotRange{}, FrequencyRange{},
                      "exclusion " + typedToken("exclusion", exclusion.id) + " members=" +
                          std::to_string(exclusion.members.size()) + " guard=" +
                          std::to_string(exclusion.guardBandMhz) + "MHz");
  return okStatus();
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

Status SpectrumRuntime::reset() {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail::RuntimeState& state = *state_;

  state.grids.clear();
  state.spans.clear();
  state.ports.clear();
  state.domains.clear();
  state.exclusionDomains.clear();
  state.domainExclusions.clear();
  state.capabilities.clear();
  state.reservations.clear();
  state.liveByDomain.clear();
  state.requestIndex.clear();
  state.audit.clear();
  state.auditSequence = AuditSequence(0);
  state.nextReservationId = ReservationId(1);
  state.lastActivityAt = Instant{};
  state.stats = RuntimeStats{};

  detail::appendAudit(state, AuditKind::StateReset, systemNow(), AllocationOutcome::Unknown,
                      ReservationId{}, ReservationGeneration{}, SpectrumDomainId{}, SlotRange{},
                      FrequencyRange{}, "runtime state cleared");
  return okStatus();
}

// ---------------------------------------------------------------------------
// detail helpers
// ---------------------------------------------------------------------------
namespace detail {

void appendAudit(RuntimeState& state, AuditKind kind, Instant at, AllocationOutcome outcome,
                 ReservationId reservation, ReservationGeneration generation,
                 SpectrumDomainId domain, SlotRange slots, FrequencyRange frequency,
                 std::string detail) {
  AuditRecord record;
  state.auditSequence.bump();
  record.sequence = state.auditSequence;
  record.at = at;
  record.kind = kind;
  record.fence = state.fence;
  record.outcome = outcome;
  record.reservation = reservation;
  record.reservationGeneration = generation;
  record.domain = domain;
  record.slots = slots;
  record.frequency = frequency;
  record.detail = std::move(detail);

  while (state.audit.size() >= state.config.maxAuditRecords) {
    state.audit.pop_front();
  }
  state.audit.push_back(std::move(record));
}

void addLiveReservation(RuntimeState& state, const SpectrumReservation& reservation) {
  if (!isLiveState(reservation.state)) return;
  for (const SpectrumDomainId domain : reservation.domains) {
    auto& list = state.liveByDomain[domain];
    if (std::find(list.begin(), list.end(), reservation.id) == list.end()) {
      list.push_back(reservation.id);
      std::sort(list.begin(), list.end());
    }
  }
}

void removeLiveReservation(RuntimeState& state, ReservationId id) {
  if (!id.valid()) return;
  for (auto it = state.liveByDomain.begin(); it != state.liveByDomain.end();) {
    auto& list = it->second;
    list.erase(std::remove(list.begin(), list.end(), id), list.end());
    if (list.empty()) {
      it = state.liveByDomain.erase(it);
    } else {
      ++it;
    }
  }
}

bool isLiveIndexed(const RuntimeState& state, ReservationId id) {
  for (const auto& entry : state.liveByDomain) {
    if (std::find(entry.second.begin(), entry.second.end(), id) != entry.second.end()) return true;
  }
  return false;
}

void rebuildIndices(RuntimeState& state) {
  state.liveByDomain.clear();
  state.requestIndex.clear();
  for (const auto& entry : state.reservations) {
    const SpectrumReservation& reservation = entry.second;
    state.requestIndex[reservation.requestId] = reservation.id;
    addLiveReservation(state, reservation);
  }
}

bool isLiveAt(const SpectrumReservation& reservation, Instant now) noexcept {
  return isLiveState(reservation.state) && reservation.lease.validAt(now);
}

FrequencyRange expandByGuard(FrequencyRange range, std::int64_t guardMhz) noexcept {
  FrequencyRange out = range;
  const std::int64_t guard = guardMhz < 0 ? 0 : guardMhz;
  std::int64_t low = 0;
  if (subOverflow(range.lowMhz, guard, low)) low = 0;
  out.lowMhz = low < 0 ? 0 : low;
  std::int64_t high = 0;
  if (addOverflow(range.highMhz, guard, high)) high = range.highMhz;
  out.highMhz = high;
  return out;
}

bool frequencyConflict(FrequencyRange a, std::int64_t guardA, FrequencyRange b,
                       std::int64_t guardB) noexcept {
  if (a.empty() || b.empty()) return false;
  const std::int64_t guardLow = guardA < 0 ? 0 : guardA;
  const std::int64_t guardOther = guardB < 0 ? 0 : guardB;
  std::int64_t aLow = 0;
  if (subOverflow(a.lowMhz, guardLow, aLow)) aLow = 0;
  std::int64_t bLow = 0;
  if (subOverflow(b.lowMhz, guardOther, bLow)) bLow = 0;
  return aLow < b.highMhz && bLow < a.highMhz;
}

bool reservationsConflict(const SpectrumReservation& a, const SpectrumReservation& b) noexcept {
  if (a.id == b.id) return false;
  bool shared = false;
  for (const SpectrumDomainId left : a.domains) {
    if (std::find(b.domains.begin(), b.domains.end(), left) != b.domains.end()) {
      shared = true;
      break;
    }
  }
  if (!shared) return false;
  return frequencyConflict(a.frequency, a.guardBandMhz, b.frequency, b.guardBandMhz);
}

void Evaluation::refuse(AllocationOutcome value, std::string reason) {
  if (reasons.empty()) outcome = value;
  reasons.push_back(std::move(reason));
}

void Evaluation::noteConflict(ReservationId id) {
  if (!id.valid()) return;
  if (std::find(conflicts.begin(), conflicts.end(), id) != conflicts.end()) return;
  conflicts.push_back(id);
  std::sort(conflicts.begin(), conflicts.end());
}

void Evaluation::noteConflicts(const std::vector<ReservationId>& ids) {
  for (const ReservationId id : ids) noteConflict(id);
}

// Keeps room for one more commit by discarding the oldest terminal records.
// Pruning is only reachable at or above the configured bound, and it never
// discards a record that still owns spectrum: the audit trail retains the
// history of everything that is pruned.
void pruneTerminalReservations(RuntimeState& state) {
  std::size_t guard = 0;
  while (state.reservations.size() >= state.config.maxReservations && guard < 1000000) {
    ++guard;
    bool pruned = false;
    for (auto it = state.reservations.begin(); it != state.reservations.end(); ++it) {
      if (!isTerminalState(it->second.state)) continue;
      const ReservationId id = it->first;
      const auto request = state.requestIndex.find(it->second.requestId);
      if (request != state.requestIndex.end() && request->second == id) {
        state.requestIndex.erase(request);
      }
      state.reservations.erase(it);
      pruned = true;
      break;
    }
    if (!pruned) return;
  }
}

}  // namespace detail
}  // namespace wavelength_fabric
