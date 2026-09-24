#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "wavelength_fabric/runtime.hpp"
#include "wavelength_fabric/serialization.hpp"

// Internal runtime state and the shared helpers used by the runtime sources.
//
// Nothing in this header is installed. Every helper documented here requires
// that the caller already holds SpectrumRuntime::mutex_; none of them acquires
// a lock, and none of them calls back into a public runtime method.

namespace wavelength_fabric {
namespace detail {

// A domain may participate in at most this many exclusion domains. The bound
// keeps conflict scanning proportional to a small constant.
inline constexpr std::size_t kMaxExclusionDomainsPerMember = 8;

// Maximum number of label/detail characters persisted for a single record.
inline constexpr std::size_t kMaxLabelBytes = 256;

struct RuntimeState {
  RuntimeConfig config;

  ControllerFence fence;
  RuntimeGeneration runtimeGeneration{};
  RecoveryGeneration recoveryGeneration{};
  AuthorityState authority;

  std::map<ChannelGridId, ChannelGrid> grids;
  std::map<SpanId, Span> spans;
  std::map<PortId, OpticalPort> ports;
  std::map<SpectrumDomainId, SpectrumDomain> domains;
  std::map<ExclusionDomainId, ExclusionDomain> exclusionDomains;
  std::map<SpectrumDomainId, std::vector<ExclusionDomainId>> domainExclusions;
  std::map<SpectrumDomainId, SpectrumCapability> capabilities;

  // Every reservation ever committed, in ascending identity order.
  std::map<ReservationId, SpectrumReservation> reservations;
  // Live reservations per domain. Maintained on every state change.
  std::map<SpectrumDomainId, std::vector<ReservationId>> liveByDomain;
  // Request identity to committed reservation, used for idempotent replay and
  // supersede semantics.
  std::map<AllocationRequestId, ReservationId> requestIndex;

  std::deque<AuditRecord> audit;
  AuditSequence auditSequence{};
  ReservationId nextReservationId{ReservationId(1)};

  // Highest instant at which an ownership-affecting operation completed. Used
  // by recovery to re-validate the no-overlap invariant.
  Instant lastActivityAt{};

  RuntimeStats stats;
};

// ---------------------------------------------------------------------------
// Audit
// ---------------------------------------------------------------------------
void appendAudit(RuntimeState& state, AuditKind kind, Instant at, AllocationOutcome outcome,
                 ReservationId reservation, ReservationGeneration generation,
                 SpectrumDomainId domain, SlotRange slots, FrequencyRange frequency,
                 std::string detail);

// ---------------------------------------------------------------------------
// Indices
// ---------------------------------------------------------------------------
void addLiveReservation(RuntimeState& state, const SpectrumReservation& reservation);
void removeLiveReservation(RuntimeState& state, ReservationId id);
void rebuildIndices(RuntimeState& state);
[[nodiscard]] bool isLiveIndexed(const RuntimeState& state, ReservationId id);

// ---------------------------------------------------------------------------
// Ownership rules
// ---------------------------------------------------------------------------
[[nodiscard]] bool isLiveAt(const SpectrumReservation& reservation, Instant now) noexcept;

// Frequency range expanded by a guard band, clamped at zero.
[[nodiscard]] FrequencyRange expandByGuard(FrequencyRange range, std::int64_t guardMhz) noexcept;

// Two half-open frequency ranges conflict when neither is separated from the
// other by its own guard band.
[[nodiscard]] bool frequencyConflict(FrequencyRange a, std::int64_t guardA, FrequencyRange b,
                                     std::int64_t guardB) noexcept;

// Two reservations conflict when they overlap on a shared resource.
[[nodiscard]] bool reservationsConflict(const SpectrumReservation& a,
                                        const SpectrumReservation& b) noexcept;

// ---------------------------------------------------------------------------
// Candidate enumeration
// ---------------------------------------------------------------------------
struct BlockedInterval {
  std::int64_t lowMhz{0};
  std::int64_t highMhz{0};
  SpectrumDomainId domain{};
  ReservationId reservation{};
  ExclusionDomainId exclusion{};
};

struct Evaluation {
  AllocationOutcome outcome{AllocationOutcome::Unknown};
  std::vector<std::string> reasons;
  std::vector<SpectrumCandidate> rejected;
  std::vector<ReservationId> conflicts;
  SpectrumCandidate selected;
  std::size_t candidatesEnumerated{0};
  std::size_t candidatesRejected{0};
  std::size_t candidatesOmitted{0};
  std::size_t candidateOrdinal{0};
  bool eligible{false};

  // Set when the request replays a request identity that is already committed.
  // A replay never creates a second reservation and never mutates state.
  bool replay{false};
  SpectrumReservation replayValue{};

  void refuse(AllocationOutcome value, std::string reason);
  void noteConflict(ReservationId id);
  void noteConflicts(const std::vector<ReservationId>& ids);
};

// Enumerates candidates in canonical order. The request shape is re-validated;
// a structurally invalid request yields a CandidateSet with a non-Ok status.
[[nodiscard]] CandidateSet enumerate(const RuntimeState& state, const SpectrumRequest& request);
[[nodiscard]] CandidateSet enumerateWithOutcome(const RuntimeState& state, const SpectrumRequest& request,
                                                AllocationOutcome& outcome);

// Full evaluation of a request against current ownership, without mutating it.
[[nodiscard]] Evaluation evaluate(const RuntimeState& state, const SpectrumRequest& request);

// Maps a status produced by an authority check to the matching typed outcome.
[[nodiscard]] AllocationOutcome outcomeForStatus(StatusCode code) noexcept;

// Maps a typed refusal back to a status for the caller, using the decisive
// reason recorded in the explanation.
[[nodiscard]] Status statusForOutcome(AllocationOutcome outcome,
                                      const std::vector<std::string>& reasons);

// Structural equality between a committed reservation and an incoming request,
// ignoring time. Used for idempotent replay detection.
[[nodiscard]] bool sameRequestShape(const SpectrumReservation& reservation,
                                    const SpectrumRequest& request);

// Builds the candidate view of an already committed reservation.
[[nodiscard]] SpectrumCandidate candidateFromReservation(const SpectrumReservation& reservation);

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
[[nodiscard]] Status encodeState(const RuntimeState& state, std::vector<std::uint8_t>& out);
[[nodiscard]] bool decodeState(std::span<const std::uint8_t> bytes, RuntimeState& out,
                              RecoveryReport& report);
[[nodiscard]] Status writeStateFile(const std::string& path, const std::vector<std::uint8_t>& bytes,
                                    bool fsync);

// Encodes and writes the current state. The caller must already hold the
// runtime mutex. Updates statistics and the audit trail.
[[nodiscard]] Status saveLocked(RuntimeState& state);

// A fresh controller incarnation, unique per runtime construction within and
// across processes.
[[nodiscard]] std::uint64_t nextIncarnation() noexcept;

// Persists the state after an ownership-affecting mutation when durable commits
// are enabled. Returns Ok when durable commits are off. A caller that receives
// a failure must roll its mutation back: no ownership change survives a commit
// that did not cross the durable boundary.
[[nodiscard]] Status persistAfterMutation(RuntimeState& state);
[[nodiscard]] Status readStateFile(const std::string& path, std::vector<std::uint8_t>& out,
                                   std::uint64_t maxBytes);

// ---------------------------------------------------------------------------
// Terminal reservation pruning
// ---------------------------------------------------------------------------
void pruneTerminalReservations(RuntimeState& state);

}  // namespace detail
}  // namespace wavelength_fabric
