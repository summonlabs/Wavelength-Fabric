#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "wavelength_fabric/audit.hpp"
#include "wavelength_fabric/authority.hpp"
#include "wavelength_fabric/candidate.hpp"
#include "wavelength_fabric/capability.hpp"
#include "wavelength_fabric/decision.hpp"
#include "wavelength_fabric/error.hpp"
#include "wavelength_fabric/identity.hpp"
#include "wavelength_fabric/request.hpp"
#include "wavelength_fabric/reservation.hpp"
#include "wavelength_fabric/resource.hpp"
#include "wavelength_fabric/time.hpp"
#include "wavelength_fabric/version.hpp"

// The Wavelength Fabric runtime.
//
// The runtime is a single authoritative in-process state machine guarded by one
// mutex. Every public method either completes fully or leaves state untouched.
// All time inputs are supplied by the caller, so identical inputs and identical
// policy produce identical decisions.
//
// Threading contract: exactly one mutex (SpectrumRuntime::mutex_) guards all
// mutable state. It is a non-recursive std::mutex and is never reacquired from
// inside a critical section. No callback is ever invoked while the mutex is
// held. The optional TCP front end takes its own connection lock, releases it,
// and only then calls into the runtime, so there is no lock inversion between
// transport bookkeeping and runtime state.

namespace wavelength_fabric {

namespace detail {
struct RuntimeState;
}  // namespace detail

struct RuntimeConfig {
  std::uint32_t maxGrids{4096};
  std::uint32_t maxDomains{16384};
  std::uint32_t maxExclusionDomains{4096};
  std::uint32_t maxReservations{1u << 20};
  std::uint32_t maxAuditRecords{65536};
  std::uint32_t maxCandidatesPerRequest{kMaxCandidatesPerRequest};
  std::uint32_t maxRejectedExplanations{16};
  std::uint32_t maxAuditPageSize{4096};
  std::uint64_t maxStateBytes{256ull * 1024ull * 1024ull};

  // Empty means the runtime is purely in-memory.
  std::string statePath;
  // When true, a commit is persisted before the allocating call returns. A
  // failed persist reclassifies the decision as a refusal and rolls the
  // in-memory commit back.
  bool durableCommits{false};
  bool fsyncState{true};

  ControllerId controller{ControllerId(1)};
};

struct RuntimeStats {
  std::uint64_t allocationsCommitted{0};
  std::uint64_t allocationsRefused{0};
  std::uint64_t candidatesEvaluated{0};
  std::uint64_t enumerationTruncations{0};
  std::uint64_t renewals{0};
  std::uint64_t releases{0};
  std::uint64_t activations{0};
  std::uint64_t deactivations{0};
  std::uint64_t expirationSweeps{0};
  std::uint64_t reclamations{0};
  std::uint64_t persistenceWrites{0};
  std::uint64_t persistenceFailures{0};
  std::uint64_t replayRejections{0};
  std::uint64_t corruptionDetections{0};
};

class SpectrumRuntime {
 public:
  explicit SpectrumRuntime(RuntimeConfig config = {});
  ~SpectrumRuntime();

  SpectrumRuntime(const SpectrumRuntime&) = delete;
  SpectrumRuntime& operator=(const SpectrumRuntime&) = delete;
  SpectrumRuntime(SpectrumRuntime&&) = delete;
  SpectrumRuntime& operator=(SpectrumRuntime&&) = delete;

  // ------------------------------------------------------------------
  // Identity of this controller boot.
  // ------------------------------------------------------------------
  [[nodiscard]] ControllerFence fence() const;
  [[nodiscard]] ControllerEpoch epoch() const;
  [[nodiscard]] ControllerIncarnation incarnation() const;
  [[nodiscard]] RuntimeGeneration runtimeGeneration() const;
  [[nodiscard]] RecoveryGeneration recoveryGeneration() const;
  [[nodiscard]] AuthorityState authorityState() const;
  [[nodiscard]] RuntimeConfig config() const;

  // Advances the recognised authority generations. Generations may only move
  // forward and the fence must be current; a backwards or stale advance is
  // refused.
  [[nodiscard]] Status advanceAuthority(const AuthorityState& next);

  // ------------------------------------------------------------------
  // Registration. Every registration is fence-checked and generation-checked.
  // ------------------------------------------------------------------
  [[nodiscard]] Status registerGrid(const ChannelGrid& grid);
  [[nodiscard]] Status registerSpan(const Span& span);
  [[nodiscard]] Status registerPort(const OpticalPort& port);
  [[nodiscard]] Status registerDomain(const SpectrumDomain& domain);
  [[nodiscard]] Status registerExclusionDomain(const ExclusionDomain& exclusion);
  [[nodiscard]] Status publishCapability(const SpectrumCapability& capability);

  // ------------------------------------------------------------------
  // Queries.
  // ------------------------------------------------------------------
  [[nodiscard]] std::optional<ChannelGrid> grid(ChannelGridId id) const;
  [[nodiscard]] std::optional<SpectrumDomain> domain(SpectrumDomainId id) const;
  [[nodiscard]] std::optional<ExclusionDomain> exclusionDomain(ExclusionDomainId id) const;
  [[nodiscard]] std::optional<SpectrumCapability> capability(SpectrumDomainId id) const;
  [[nodiscard]] std::optional<SpectrumReservation> reservation(ReservationId id) const;
  [[nodiscard]] std::vector<SpectrumReservation> reservations() const;
  [[nodiscard]] std::vector<SpectrumReservation> reservationsForDomain(SpectrumDomainId id) const;
  [[nodiscard]] std::vector<SpectrumDomain> domains() const;
  [[nodiscard]] std::vector<ChannelGrid> grids() const;
  [[nodiscard]] std::vector<ExclusionDomain> exclusionDomains() const;
  [[nodiscard]] std::optional<SpectrumUsage> usage(SpectrumDomainId id, Instant now) const;
  [[nodiscard]] std::vector<SpectrumUsage> usageAll(Instant now) const;
  [[nodiscard]] std::vector<AuditRecord> audit(AuditSequence since, std::size_t limit) const;
  [[nodiscard]] std::size_t auditSize() const;
  [[nodiscard]] AuditSequence lastAuditSequence() const;
  [[nodiscard]] RuntimeStats stats() const;

  // ------------------------------------------------------------------
  // Candidate enumeration and explanation. Read-only.
  // ------------------------------------------------------------------
  [[nodiscard]] CandidateSet enumerateCandidates(const SpectrumRequest& request) const;
  [[nodiscard]] DecisionExplanation explain(const SpectrumRequest& request) const;

  // ------------------------------------------------------------------
  // Allocation. The only path that creates ownership.
  // ------------------------------------------------------------------
  [[nodiscard]] AllocationDecision allocate(const SpectrumRequest& request);

  // ------------------------------------------------------------------
  // Reservation lifecycle. All operations are generation- and fence-fenced.
  // ------------------------------------------------------------------
  [[nodiscard]] Status renew(ReservationId id, ReservationGeneration generation, Duration extension,
                             const ReservationAuthority& authority, Instant now,
                             std::uint32_t maxRenewals);
  [[nodiscard]] Status activate(ReservationId id, ReservationGeneration generation,
                                const ActivationAuthority& authority, Instant now);
  [[nodiscard]] Status deactivate(ReservationId id, ReservationGeneration generation,
                                  const ActivationAuthority& authority, Instant now);
  [[nodiscard]] Status release(ReservationId id, ReservationGeneration generation,
                               const ReleaseAuthority& authority, Instant now);

  // Marks every lapsed reservation Expired without returning its spectrum.
  [[nodiscard]] ReclaimReport expireLeases(Instant now);
  // Moves every lapsed reservation to Reclaimed, returning its spectrum to the
  // free pool with exact accounting.
  [[nodiscard]] ReclaimReport reclaimExpired(Instant now);

  // ------------------------------------------------------------------
  // Durability.
  // ------------------------------------------------------------------
  [[nodiscard]] Status save();
  [[nodiscard]] RecoveryReport recover();
  [[nodiscard]] Status reset();

 private:
  mutable std::mutex mutex_;
  std::unique_ptr<detail::RuntimeState> state_;
};

[[nodiscard]] std::string describeRuntime(const RuntimeStats& stats);

}  // namespace wavelength_fabric
