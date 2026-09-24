#include "test_common.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Real multithreaded races on ONE runtime.
//
// Eight threads are released together by a barrier built from an atomic arrival
// counter (each thread spins until every thread has arrived): four allocators
// attempting allocations over the same three domains, one lifecycle thread
// renewing, activating, deactivating and releasing live reservations, one
// sweeper expiring and reclaiming lapsed leases, and two readers calling usage,
// enumerateCandidates, audit and reservations while the writers mutate.
//
// Nothing sleeps and nothing polls with a deadline. Threads never assert:
// the harness is not thread safe, so every worker records what it observed and
// the owning thread asserts the exact accounting afterwards. The invariants
// asserted after the join are: at most one live owner per slot range, exact
// occupancy accounting recomputed from scratch, no duplicated identities,
// strictly increasing audit sequences and exact statistics.

namespace {

using namespace wavelength_fabric;

constexpr std::size_t kThreads = 8;
constexpr std::size_t kAllocatorThreads = 4;
constexpr std::uint32_t kSlotCount = 32;
constexpr std::uint32_t kMaxChannelSlots = 8;
constexpr std::uint32_t kMaxRequestedSlots = 4;
constexpr std::int64_t kAnchorMhz = 191'300'000;
constexpr std::int64_t kSlotWidthMhz = 12'500;
constexpr std::uint32_t kAllocationsPerThread = 220;
constexpr std::uint32_t kLifecycleOps = 900;
constexpr std::uint32_t kSweeps = 60;
constexpr std::uint32_t kExactRounds = 25;
// Writers that take part in the second barrier: the four allocators, the
// sweeper and the lifecycle thread.
constexpr int kWriterThreads = static_cast<int>(kAllocatorThreads) + 2;
constexpr std::uint32_t kReaderPasses = 250;
constexpr std::size_t kAuditPage = 128;

const Instant kStart = Instant::fromSeconds(1'800'000'000);
const Instant kLate = kStart + Duration::seconds(1000);
const Duration kLease = Duration::seconds(600);
const Duration kRenewExtension = Duration::seconds(120);

struct Rng {
  std::uint64_t state;

  explicit Rng(std::uint64_t seed) : state(seed) {}

  std::uint64_t next() {
    state += 0x9E37'79B9'7F4A'7C15ull;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBull;
    return z ^ (z >> 31);
  }

  std::uint32_t below(std::uint32_t bound) {
    return static_cast<std::uint32_t>(next() % static_cast<std::uint64_t>(bound));
  }
};

// Everything a worker observed. Each worker writes only its own slot, so the
// workers never share mutable state outside the runtime.
struct ThreadResult {
  std::uint64_t allocations{0};
  std::uint64_t committed{0};
  std::uint64_t refused{0};
  std::uint64_t releases{0};
  std::uint64_t renewals{0};
  std::uint64_t activations{0};
  std::uint64_t deactivations{0};
  std::uint64_t sweeps{0};
  std::uint64_t reclamations{0};
  std::uint64_t passes{0};
  std::uint64_t unexpected{0};
  std::uint64_t badDecision{0};
  std::uint64_t badUsage{0};
  std::uint64_t badEnumeration{0};
  std::uint64_t badAudit{0};
  std::uint64_t badIdentities{0};
  std::uint64_t badStats{0};
  std::uint64_t releaseIllegal{0};
  std::uint64_t releaseStale{0};
  std::uint64_t releaseNotFound{0};
  std::uint64_t releaseOther{0};
  std::uint64_t renewalAttempts{0};
  std::uint64_t activationAttempts{0};
  std::uint64_t deactivationAttempts{0};
  std::uint64_t releaseAttempts{0};
  std::uint64_t exactRounds{0};
  std::uint64_t highestReservation{0};
};

std::vector<SpectrumDomainId> domainsFor(std::uint32_t mask) {
  std::vector<SpectrumDomainId> domains;
  for (std::uint32_t index = 0; index < 3; ++index) {
    if ((mask & (1u << index)) != 0u) domains.push_back(SpectrumDomainId(index + 1));
  }
  return domains;
}

std::vector<SpectrumDomainGeneration> generationsFor(std::size_t count) {
  return std::vector<SpectrumDomainGeneration>(count, SpectrumDomainGeneration(1));
}

ContiguityRequirement contiguityFor(std::uint32_t slots) {
  return slots > 1 ? ContiguityRequirement::Required : ContiguityRequirement::Unspecified;
}

ContinuityRequirement continuityFor(std::size_t domains) {
  return domains > 1 ? ContinuityRequirement::Required : ContinuityRequirement::Unspecified;
}

struct Tokens {
  EligibilityAuthority eligibility;
  ReservationAuthority reservation;
  ActivationAuthority activation;
  ReleaseAuthority release;
};

Tokens tokensFor(const ControllerFence& fence) {
  Tokens tokens;
  tokens.eligibility.generation = EligibilityAuthorityGeneration(1);
  tokens.eligibility.fence = fence;
  tokens.reservation.generation = ReservationAuthorityGeneration(1);
  tokens.reservation.fence = fence;
  tokens.activation.generation = ActivationAuthorityGeneration(1);
  tokens.activation.fence = fence;
  tokens.release.generation = ReleaseAuthorityGeneration(1);
  tokens.release.fence = fence;
  return tokens;
}

void setupRuntime(SpectrumRuntime& runtime) {
  const std::int64_t endMhz =
      kAnchorMhz + kSlotWidthMhz * static_cast<std::int64_t>(kSlotCount);
  const ChannelGrid grid = wf_test::flexGrid(ChannelGridId(1), GridGeneration(1), kAnchorMhz,
                                             kSlotWidthMhz, kSlotCount, 1, kMaxChannelSlots);
  WF_CHECK(runtime.registerGrid(grid).ok());
  for (std::uint64_t value = 1; value <= 3; ++value) {
    const SpectrumDomainId domain(value);
    WF_CHECK(runtime.registerDomain(wf_test::makeDomain(domain, ChannelGridId(1))).ok());
    SpectrumCapability capability = wf_test::makeCapability(
        domain, ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
        SpectrumSupport::Supported, 0, kSlotCount, runtime.fence());
    capability.minTunableMhz = kAnchorMhz;
    capability.maxTunableMhz = endMhz;
    WF_CHECK(runtime.publishCapability(capability).ok());
  }
}

void runAllocator(SpectrumRuntime& runtime, const Tokens& tokens, std::size_t index,
                  ThreadResult& result) {
  Rng rng(0x1000'0000ull + index);
  for (std::uint32_t attempt = 0; attempt < kAllocationsPerThread; ++attempt) {
    const std::vector<SpectrumDomainId> domains = domainsFor(1u + rng.below(7u));
    const std::uint32_t slots = 1 + rng.below(kMaxRequestedSlots);
    const AllocationRequestId requestId(index * 1'000'000ull + attempt + 1);
    const SpectrumRequest request =
        wf_test::makeRequest(requestId, OwnerId(index + 1), domains, generationsFor(domains.size()),
                             ChannelGridId(1), GridGeneration(1), slots, kStart, kLease,
                             tokens.eligibility, tokens.reservation, contiguityFor(slots),
                             continuityFor(domains.size()));
    const AllocationDecision decision = runtime.allocate(request);
    ++result.allocations;
    if (decision.allocated()) {
      ++result.committed;
      result.highestReservation =
          std::max(result.highestReservation, decision.reservation.raw());
      if (!decision.status.ok() || decision.outcome != AllocationOutcome::Allocated ||
              decision.reservation.none()) {
        ++result.badDecision;
      }
    } else {
      ++result.refused;
      if (decision.status.ok() || !isRefusal(decision.outcome) || !decision.reservation.none()) {
        ++result.badDecision;
      }
    }
  }
}

// Performs an allocation and then walks one reservation through activate,
// deactivate, renew and release. Only the lifecycle thread writes during this
// phase, so every transition is deterministic and its accounting is exact.
void runExactTransitions(SpectrumRuntime& runtime, const Tokens& tokens, ThreadResult& result) {
  std::uint64_t requestId = 9'000'000;
  for (std::uint32_t round = 0; round < kExactRounds; ++round) {
    // Return every slot to the pool, then build a reservation to walk.
    const ReclaimReport cleared = runtime.reclaimExpired(kLate);
    if (!cleared.status.ok()) {
      ++result.unexpected;
      return;
    }
    result.reclamations += cleared.reclaimed.size();
    ++result.sweeps;

    const SpectrumRequest request = wf_test::makeRequest(
        AllocationRequestId(requestId++), OwnerId(7), {SpectrumDomainId(1)},
        {SpectrumDomainGeneration(1)}, ChannelGridId(1), GridGeneration(1), 4, kStart, kLease,
        tokens.eligibility, tokens.reservation, ContiguityRequirement::Required,
        ContinuityRequirement::Unspecified);
    const AllocationDecision decision = runtime.allocate(request);
    if (!decision.allocated()) {
      ++result.unexpected;
      return;
    }
    ++result.allocations;
    ++result.committed;
    const ReservationId id = decision.reservation;
    ReservationGeneration generation = decision.generation;

    const Status activated = runtime.activate(id, generation, tokens.activation, kStart);
    if (!activated.ok()) {
      ++result.unexpected;
      return;
    }
    ++result.activationAttempts;
    ++result.activations;

    const Status deactivated = runtime.deactivate(id, generation, tokens.activation, kStart);
    if (!deactivated.ok()) {
      ++result.unexpected;
      return;
    }
    ++result.deactivationAttempts;
    ++result.deactivations;

    const Status renewed =
        runtime.renew(id, generation, kRenewExtension, tokens.reservation, kStart, 0);
    if (!renewed.ok()) {
      ++result.unexpected;
      return;
    }
    ++result.renewalAttempts;
    ++result.renewals;
    generation = ReservationGeneration(generation.raw() + 1);

    ++result.releaseAttempts;
    const Status released = runtime.release(id, generation, tokens.release, kStart);
    if (!released.ok()) {
      ++result.unexpected;
      return;
    }
    ++result.releases;
    ++result.exactRounds;
  }
}

void runLifecycle(SpectrumRuntime& runtime, const Tokens& tokens, ThreadResult& result) {
  Rng rng(0x2000'0000ull);
  for (std::uint32_t operation = 0; operation < kLifecycleOps; ++operation) {
    const std::vector<SpectrumReservation> reservations = runtime.reservations();
    if (reservations.empty()) continue;
    const std::uint32_t choice = rng.below(4);

    // Prefer a target the chosen operation can actually complete, so every
    // transition of the state machine is exercised; when none exists, fall back
    // to an arbitrary target so the refusal paths are exercised as well.
    std::vector<std::size_t> eligible;
    for (std::size_t index = 0; index < reservations.size(); ++index) {
      const SpectrumReservation& candidate = reservations[index];
      const bool usable =
          (choice == 0 &&
           (candidate.state == ReservationState::Reserved ||
            candidate.state == ReservationState::Active) &&
           candidate.isLiveAt(kStart)) ||
          (choice == 1 && candidate.state == ReservationState::Reserved &&
           candidate.isLiveAt(kStart)) ||
          (choice == 2 && candidate.state == ReservationState::Active) ||
          (choice == 3 && isLiveState(candidate.state));
      if (usable) eligible.push_back(index);
    }
    const std::size_t picked =
        eligible.empty() ? rng.below(static_cast<std::uint32_t>(reservations.size()))
                         : eligible[rng.below(static_cast<std::uint32_t>(eligible.size()))];
    const SpectrumReservation& target = reservations[picked];
    if (choice == 0) {
      ++result.renewalAttempts;
      const Status status = runtime.renew(target.id, target.generation, kRenewExtension,
                                          tokens.reservation, kStart, 0);
      if (status.ok()) {
        ++result.renewals;
      } else if (status.code != StatusCode::Refused && status.code != StatusCode::IllegalTransition &&
                 status.code != StatusCode::LimitExceeded &&
                 status.code != StatusCode::StaleGeneration && status.code != StatusCode::NotFound) {
        ++result.unexpected;
      }
      continue;
    }
    if (choice == 1) {
      ++result.activationAttempts;
      const Status status = runtime.activate(target.id, target.generation, tokens.activation, kStart);
      if (status.ok()) {
        ++result.activations;
      } else if (status.code != StatusCode::Refused && status.code != StatusCode::IllegalTransition &&
                 status.code != StatusCode::NotFound) {
        ++result.unexpected;
      }
      continue;
    }
    if (choice == 2) {
      ++result.deactivationAttempts;
      const Status status =
          runtime.deactivate(target.id, target.generation, tokens.activation, kStart);
      if (status.ok()) {
        ++result.deactivations;
      } else if (status.code != StatusCode::IllegalTransition && status.code != StatusCode::NotFound) {
        ++result.unexpected;
      }
      continue;
    }
    ++result.releaseAttempts;
    const Status status = runtime.release(target.id, target.generation, tokens.release, kStart);
    if (status.ok()) {
      ++result.releases;
    } else if (status.code == StatusCode::IllegalTransition) {
      ++result.releaseIllegal;
    } else if (status.code == StatusCode::StaleGeneration) {
      ++result.releaseStale;
    } else if (status.code == StatusCode::NotFound) {
      ++result.releaseNotFound;
    } else {
      ++result.releaseOther;
      ++result.unexpected;
    }
  }
}

// Expiry and reclamation are destructive: lapsing every lease at a late instant
// leaves no live owner for the lifecycle thread to work on. Two sweeps in ten
// are destructive, the rest reclaim what a previous sweep already lapsed, so
// the sweep paths run continuously without starving the other writers.
void runSweeper(SpectrumRuntime& runtime, ThreadResult& result) {
  for (std::uint32_t sweep = 0; sweep < kSweeps; ++sweep) {
    const std::uint32_t phase = sweep % 10;
    if (phase == 0) {
      const ReclaimReport report = runtime.expireLeases(kLate);
      if (!report.status.ok() || report.evaluatedAt != kLate || !report.reclaimed.empty()) {
        ++result.unexpected;
      }
      ++result.sweeps;
      continue;
    }
    const Instant instant = phase == 5 ? kLate : kStart;
    const ReclaimReport report = runtime.reclaimExpired(instant);
    if (!report.status.ok() || report.evaluatedAt != instant ||
        report.scanned < report.reclaimed.size()) {
      ++result.unexpected;
    }
    for (std::size_t index = 1; index < report.reclaimed.size(); ++index) {
      if (!(report.reclaimed[index - 1] < report.reclaimed[index])) ++result.unexpected;
    }
    result.reclamations += report.reclaimed.size();
    ++result.sweeps;
  }
}

void runReader(SpectrumRuntime& runtime, const Tokens& tokens, std::size_t index,
               ThreadResult& result) {
  Rng rng(0x3000'0000ull + index);
  RuntimeStats previous{};
  for (std::uint32_t pass = 0; pass < kReaderPasses; ++pass) {
    ++result.passes;

    for (std::uint64_t value = 1; value <= 3; ++value) {
      const std::optional<SpectrumUsage> usage = runtime.usage(SpectrumDomainId(value), kStart);
      if (!usage.has_value()) {
        ++result.badUsage;
        continue;
      }
      if (usage->liveSlots > usage->allocatableSlots) ++result.badUsage;
      if (usage->freeSlots != usage->allocatableSlots - usage->liveSlots) ++result.badUsage;
      std::uint32_t covered = 0;
      std::uint32_t previousStart = 0;
      bool first = true;
      for (const SlotRange& run : usage->freeRuns) {
        if (run.count == 0) ++result.badUsage;
        if (!first && run.first <= previousStart) ++result.badUsage;
        previousStart = run.first;
        first = false;
        covered += run.count;
      }
      if (covered != usage->freeSlots) ++result.badUsage;
      if (usage->liveSlots + usage->freeSlots != usage->allocatableSlots) ++result.badUsage;
    }

    {
      const std::vector<SpectrumDomainId> domains = domainsFor(1u + rng.below(7u));
      const std::uint32_t slots = 1 + rng.below(kMaxRequestedSlots);
      const SpectrumRequest request = wf_test::makeRequest(
          AllocationRequestId(900'000), OwnerId(9), domains, generationsFor(domains.size()),
          ChannelGridId(1), GridGeneration(1), slots, kStart, kLease, tokens.eligibility,
          tokens.reservation, contiguityFor(slots), continuityFor(domains.size()));
      const CandidateSet set = runtime.enumerateCandidates(request);
      if (!set.status.ok()) {
        ++result.badEnumeration;
      } else {
        if (set.eligibleCount > set.candidates.size()) ++result.badEnumeration;
        for (std::size_t position = 0; position < set.candidates.size(); ++position) {
          const SpectrumCandidate& candidate = set.candidates[position];
          if (candidate.ordinal != position) ++result.badEnumeration;
          if (candidate.slots.count == 0) ++result.badEnumeration;
          if (candidate.slots.first + candidate.slots.count > kSlotCount) ++result.badEnumeration;
          if (candidate.perDomainSlots.size() != domains.size()) ++result.badEnumeration;
        }
      }
    }

    {
      const std::vector<AuditRecord> page = runtime.audit(AuditSequence(0), kAuditPage);
      AuditSequence previousSequence{};
      for (const AuditRecord& record : page) {
        if (!(previousSequence < record.sequence)) ++result.badAudit;
        if (record.sequence.none()) ++result.badAudit;
        previousSequence = record.sequence;
      }
      if (page.size() > kAuditPage) ++result.badAudit;
    }

    {
      const std::vector<SpectrumReservation> reservations = runtime.reservations();
      for (std::size_t position = 1; position < reservations.size(); ++position) {
        if (!(reservations[position - 1].id < reservations[position].id)) ++result.badIdentities;
      }
      for (const SpectrumReservation& reservation : reservations) {
        const std::optional<SpectrumReservation> looked = runtime.reservation(reservation.id);
        if (!looked.has_value() || looked->id != reservation.id) ++result.badIdentities;
      }
    }

    const RuntimeStats stats = runtime.stats();
    if (stats.allocationsCommitted < previous.allocationsCommitted ||
        stats.allocationsRefused < previous.allocationsRefused ||
        stats.releases < previous.releases || stats.renewals < previous.renewals ||
        stats.activations < previous.activations ||
        stats.deactivations < previous.deactivations ||
        stats.expirationSweeps < previous.expirationSweeps ||
        stats.reclamations < previous.reclamations) {
      ++result.badStats;
    }
    previous = stats;
  }
}

std::string renderSlotRanges(const std::vector<SlotRange>& ranges) {
  std::string out = "[";
  for (std::size_t index = 0; index < ranges.size(); ++index) {
    if (index != 0) out.push_back(',');
    out += std::to_string(ranges[index].first);
    out.push_back('+');
    out += std::to_string(ranges[index].count);
  }
  out.push_back(']');
  return out;
}

}  // namespace

WF_TEST(eight_threads_never_create_a_second_live_owner) {
  RuntimeConfig config;
  config.maxAuditRecords = 1u << 20;
  config.maxAuditPageSize = 4096;
  SpectrumRuntime runtime(config);
  setupRuntime(runtime);
  const Tokens tokens = tokensFor(runtime.fence());
  const ControllerFence fence = runtime.fence();

  std::vector<ThreadResult> results(kThreads);
  std::atomic<int> ready{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  std::atomic<int> writersDone{0};
  for (std::size_t index = 0; index < kThreads; ++index) {
    threads.emplace_back([&runtime, &tokens, &results, &ready, &writersDone, index]() {
      ready.fetch_add(1, std::memory_order_release);
      while (ready.load(std::memory_order_acquire) < static_cast<int>(kThreads)) {
        // Spin until every thread has arrived; there is no sleep and no deadline.
      }
      if (index < kAllocatorThreads) {
        runAllocator(runtime, tokens, index, results[index]);
        writersDone.fetch_add(1, std::memory_order_release);
      } else if (index == kAllocatorThreads) {
        runLifecycle(runtime, tokens, results[index]);
        writersDone.fetch_add(1, std::memory_order_release);
        // Wait for every other writer to stop, then walk reservations through
        // the transitions with no competing writer, so those paths are executed
        // deterministically while the readers keep reading.
        while (writersDone.load(std::memory_order_acquire) < kWriterThreads) {
        }
        runExactTransitions(runtime, tokens, results[index]);
      } else if (index == kAllocatorThreads + 1) {
        runSweeper(runtime, results[index]);
        writersDone.fetch_add(1, std::memory_order_release);
      } else {
        runReader(runtime, tokens, index, results[index]);
      }
    });
  }
  for (std::thread& thread : threads) thread.join();

  ThreadResult total;
  for (const ThreadResult& result : results) {
    total.allocations += result.allocations;
    total.committed += result.committed;
    total.refused += result.refused;
    total.releases += result.releases;
    total.renewals += result.renewals;
    total.activations += result.activations;
    total.deactivations += result.deactivations;
    total.sweeps += result.sweeps;
    total.reclamations += result.reclamations;
    total.releaseIllegal += result.releaseIllegal;
    total.releaseStale += result.releaseStale;
    total.releaseNotFound += result.releaseNotFound;
    total.releaseOther += result.releaseOther;
    total.renewalAttempts += result.renewalAttempts;
    total.activationAttempts += result.activationAttempts;
    total.deactivationAttempts += result.deactivationAttempts;
    total.releaseAttempts += result.releaseAttempts;
    total.exactRounds += result.exactRounds;
    total.passes += result.passes;
    total.unexpected += result.unexpected;
    total.badDecision += result.badDecision;
    total.badUsage += result.badUsage;
    total.badEnumeration += result.badEnumeration;
    total.badAudit += result.badAudit;
    total.badIdentities += result.badIdentities;
    total.badStats += result.badStats;
    total.highestReservation = std::max(total.highestReservation, result.highestReservation);
  }

  std::cout << "worker totals: allocations=" << total.allocations
            << " committed=" << total.committed << " refused=" << total.refused
            << " releases=" << total.releases << " (illegal=" << total.releaseIllegal
            << " stale=" << total.releaseStale << " notFound=" << total.releaseNotFound
            << " other=" << total.releaseOther << ") renewals=" << total.renewals
            << " activations=" << total.activations << " deactivations=" << total.deactivations
            << " sweeps=" << total.sweeps << " reclamations=" << total.reclamations
            << " readerPasses=" << total.passes << " unexpected=" << total.unexpected
            << " attempts renew/activate/deactivate/release=" << total.renewalAttempts << "/"
            << total.activationAttempts << "/" << total.deactivationAttempts << "/"
            << total.releaseAttempts << " exactRounds=" << total.exactRounds << "\n";

  // ---- the workers, and the runtime thread contract --------------------------
  WF_CHECK_EQ(total.unexpected, std::uint64_t(0));
  WF_CHECK_EQ(total.badDecision, std::uint64_t(0));
  WF_CHECK_EQ(total.allocations,
              static_cast<std::uint64_t>(kAllocatorThreads) * kAllocationsPerThread + kExactRounds);
  WF_CHECK(total.committed >= 1);
  WF_CHECK(total.refused >= 1);
  WF_CHECK(total.passes >= 1);
  // Every transition path was attempted many times concurrently, and the
  // barrier-isolated phase completed every round, so each transition is
  // guaranteed to have succeeded at least kExactRounds times.
  WF_CHECK(total.renewalAttempts >= 1);
  WF_CHECK(total.activationAttempts >= 1);
  WF_CHECK(total.deactivationAttempts >= 1);
  WF_CHECK(total.releaseAttempts >= 1);
  WF_CHECK_EQ(total.exactRounds, static_cast<std::uint64_t>(kExactRounds));
  WF_CHECK(total.releases >= kExactRounds);
  WF_CHECK(total.renewals >= kExactRounds);
  WF_CHECK(total.activations >= kExactRounds);
  WF_CHECK(total.deactivations >= kExactRounds);
  WF_CHECK(total.sweeps >= kSweeps);

  // ---- readers saw a consistent runtime on every pass ------------------------
  WF_CHECK_EQ(total.badUsage, std::uint64_t(0));
  WF_CHECK_EQ(total.badEnumeration, std::uint64_t(0));
  WF_CHECK_EQ(total.badAudit, std::uint64_t(0));
  WF_CHECK_EQ(total.badIdentities, std::uint64_t(0));
  WF_CHECK_EQ(total.badStats, std::uint64_t(0));

  // ---- exact statistics ------------------------------------------------------
  const RuntimeStats stats = runtime.stats();
  WF_CHECK_EQ(stats.allocationsCommitted, total.committed);
  WF_CHECK_EQ(stats.allocationsRefused, total.refused);
  WF_CHECK_EQ(stats.renewals, total.renewals);
  WF_CHECK_EQ(stats.activations, total.activations);
  WF_CHECK_EQ(stats.deactivations, total.deactivations);
  WF_CHECK_EQ(stats.expirationSweeps, total.sweeps);
  WF_CHECK_EQ(stats.reclamations, total.reclamations);

  // ---- no duplicated identities, and every reservation is reachable ----------
  const std::vector<SpectrumReservation> reservations = runtime.reservations();
  WF_CHECK_EQ(reservations.size(), static_cast<std::size_t>(stats.allocationsCommitted));
  for (std::size_t index = 0; index < reservations.size(); ++index) {
    if (index != 0) {
      WF_CHECK(reservations[index - 1].id < reservations[index].id);
    }
    const std::optional<SpectrumReservation> looked = runtime.reservation(reservations[index].id);
    WF_CHECK(looked.has_value());
    if (looked.has_value()) WF_CHECK(looked->id == reservations[index].id);
  }

  // ---- at most one live owner per slot range ---------------------------------
  for (std::size_t left = 0; left < reservations.size(); ++left) {
    const SpectrumReservation& a = reservations[left];
    if (!a.isLiveAt(kStart)) continue;
    for (std::size_t right = left + 1; right < reservations.size(); ++right) {
      const SpectrumReservation& b = reservations[right];
      if (!b.isLiveAt(kStart)) continue;
      bool shared = false;
      for (const SpectrumDomainId domain : a.domains) {
        if (std::find(b.domains.begin(), b.domains.end(), domain) != b.domains.end()) {
          shared = true;
          break;
        }
      }
      if (!shared) continue;
      for (std::size_t index = 0; index < a.domains.size(); ++index) {
        const SpectrumDomainId domain = a.domains[index];
        const auto position = std::find(b.domains.begin(), b.domains.end(), domain);
        if (position == b.domains.end()) continue;
        const std::size_t other = static_cast<std::size_t>(position - b.domains.begin());
        const SlotRange rangeA = index < a.perDomainSlots.size() ? a.perDomainSlots[index] : a.slots;
        const SlotRange rangeB =
            other < b.perDomainSlots.size() ? b.perDomainSlots[other] : b.slots;
        WF_CHECK(!rangeA.overlaps(rangeB));
      }
    }
  }

  // ---- occupancy recomputed from scratch -------------------------------------
  for (std::uint64_t value = 1; value <= 3; ++value) {
    const SpectrumDomainId domain(value);
    std::vector<bool> occupied(kSlotCount, false);
    std::uint32_t liveCount = 0;
    std::uint32_t activeCount = 0;
    for (const SpectrumReservation& reservation : reservations) {
      if (!reservation.isLiveAt(kStart)) continue;
      const auto position =
          std::find(reservation.domains.begin(), reservation.domains.end(), domain);
      if (position == reservation.domains.end()) continue;
      const std::size_t index = static_cast<std::size_t>(position - reservation.domains.begin());
      const SlotRange range =
          index < reservation.perDomainSlots.size() ? reservation.perDomainSlots[index]
                                                    : reservation.slots;
      for (std::uint32_t slot = range.first; slot < range.end() && slot < kSlotCount; ++slot) {
        occupied[slot] = true;
      }
      ++liveCount;
      if (reservation.state == ReservationState::Active) ++activeCount;
    }
    std::uint32_t covered = 0;
    for (const bool taken : occupied) {
      if (taken) ++covered;
    }
    std::vector<SlotRange> expectedFreeRuns;
    std::uint32_t cursor = 0;
    while (cursor < kSlotCount) {
      if (occupied[cursor]) {
        ++cursor;
        continue;
      }
      std::uint32_t end = cursor;
      while (end < kSlotCount && !occupied[end]) ++end;
      expectedFreeRuns.push_back(SlotRange{cursor, end - cursor});
      cursor = end;
    }
    const std::optional<SpectrumUsage> usage = runtime.usage(domain, kStart);
    WF_REQUIRE(usage.has_value());
    WF_CHECK_EQ(usage->liveSlots, covered);
    WF_CHECK_EQ(usage->freeSlots, kSlotCount - covered);
    WF_CHECK_EQ(usage->liveReservations, liveCount);
    WF_CHECK_EQ(usage->activeReservations, activeCount);
    WF_CHECK_EQ(usage->activeSlots + usage->reservedSlots, usage->liveSlots);
    WF_CHECK(usage->freeRuns == expectedFreeRuns);
    if (usage->freeRuns != expectedFreeRuns) {
      std::cout << "free runs actual=" << renderSlotRanges(usage->freeRuns)
                << " expected=" << renderSlotRanges(expectedFreeRuns) << "\n";
    }
  }

  // ---- monotone audit sequences ----------------------------------------------
  std::size_t seen = 0;
  AuditSequence since{};
  while (true) {
    const std::vector<AuditRecord> page = runtime.audit(since, 512);
    if (page.empty()) break;
    for (const AuditRecord& record : page) {
      WF_CHECK(since < record.sequence);
      since = record.sequence;
      ++seen;
    }
    if (page.size() < 512) break;
  }
  WF_CHECK_EQ(seen, runtime.auditSize());
  WF_CHECK_EQ(since.raw(), static_cast<std::uint64_t>(runtime.auditSize()));
  WF_CHECK(runtime.lastAuditSequence() == since);

  // ---- releasing and reclaiming everything returns the baseline --------------
  std::uint64_t drained = 0;
  for (const SpectrumReservation& reservation : reservations) {
    if (!isLiveState(reservation.state)) continue;
    const Status status =
        runtime.release(reservation.id, reservation.generation, tokens.release, kStart);
    WF_CHECK(status.ok());
    if (status.ok()) ++drained;
  }
  const ReclaimReport finalSweep = runtime.reclaimExpired(kLate);
  WF_CHECK(finalSweep.status.ok());
  for (std::uint64_t value = 1; value <= 3; ++value) {
    const std::optional<SpectrumUsage> usage = runtime.usage(SpectrumDomainId(value), kStart);
    WF_REQUIRE(usage.has_value());
    WF_CHECK_EQ(usage->liveSlots, std::uint32_t(0));
    WF_CHECK_EQ(usage->lapsedSlots, std::uint32_t(0));
    WF_CHECK_EQ(usage->freeSlots, kSlotCount);
    WF_CHECK_EQ(usage->freeRuns.size(), std::size_t(1));
    if (usage->freeRuns.size() == 1) {
      WF_CHECK(usage->freeRuns[0].first == 0 && usage->freeRuns[0].count == kSlotCount);
    }
  }
  for (const SpectrumReservation& reservation : runtime.reservations()) {
    WF_CHECK(!isLiveState(reservation.state));
  }
  const RuntimeStats after = runtime.stats();
  WF_CHECK_EQ(after.releases, total.releases + drained);
  WF_CHECK_EQ(after.reclamations, total.reclamations + finalSweep.reclaimed.size());
  WF_CHECK_EQ(after.expirationSweeps, total.sweeps + 1);
  WF_CHECK_EQ(after.allocationsCommitted, total.committed);
  WF_CHECK(runtime.fence().incarnation.raw() == fence.incarnation.raw());
}

WF_TEST_MAIN()
