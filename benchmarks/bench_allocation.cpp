// bench_allocation: completed useful work of the Wavelength Fabric core.
//
// The benchmark runs a fixed synthetic workload against an in-process runtime
// and reports, for each phase, how many operations completed, how much time
// those operations took, and the resulting rate. It measures completed work
// rather than submission latency: every operation counted here returned before
// the count was incremented.
//
// The runtime is given a state file for the persistence phase only. Durable
// commits are off, so the allocation phases measure allocation rather than the
// file system.
//
// Self-verification: every completed allocation is checked against usage()
// accounting for its domain, and the process exits non-zero if any check
// disagrees.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "wavelength_fabric/runtime.hpp"
#include "wavelength_fabric/text.hpp"

namespace wf = wavelength_fabric;

namespace {

using Clock = std::chrono::steady_clock;

// Workload shape. The two grids cover the same abstract frequency span with
// different slot arithmetic.
constexpr std::uint32_t kGridASlots = 96;
constexpr std::uint32_t kGridBSlots = 384;
constexpr std::int64_t kAnchorMhz = 191'400'000;
constexpr std::int64_t kGridAWidthMhz = 50'000;
constexpr std::int64_t kGridBWidthMhz = 12'500;
constexpr std::uint64_t kGridAId = 1;
constexpr std::uint64_t kGridBId = 2;
constexpr std::uint64_t kDomainAId = 1;
constexpr std::uint64_t kDomainBId = 2;

constexpr std::size_t kLiveReservations = 48;  // N: live reservations kept on grid A
constexpr std::size_t kAllocationRounds = 8;   // 8 x 384 = 3072 allocation attempts
constexpr std::size_t kEnumerationCalls = 4096;
// Grid A is fixed, so every channel on it is exactly one slot wide; the
// runtime refuses any other width for that grid.
constexpr std::uint32_t kEnumerationSlots = 1;
constexpr std::size_t kPersistenceCycles = 16;

[[nodiscard]] double toSeconds(std::int64_t nanos) {
  return static_cast<double>(nanos) / 1'000'000'000.0;
}

[[nodiscard]] std::int64_t elapsedNanos(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count();
}

// Accumulates the time spent inside the measured operations, separately from
// the wall time of the phase, which also contains its verification work.
class Meter {
 public:
  void add(std::int64_t elapsed) noexcept { nanos_ += elapsed; }
  [[nodiscard]] double seconds() const noexcept { return toSeconds(nanos_); }

 private:
  std::int64_t nanos_{0};
};

struct Row {
  std::string phase;
  std::uint64_t operations{0};
  std::string unit;
  double workSeconds{0.0};
  double wallSeconds{0.0};
};

void printRow(const Row& row) {
  const double rate =
      row.workSeconds > 0.0 ? static_cast<double>(row.operations) / row.workSeconds : 0.0;
  std::cout << std::left << std::setw(30) << row.phase << std::right << std::setw(10)
            << row.operations << "  " << std::left << std::setw(16) << row.unit << std::right
            << std::fixed << std::setprecision(6) << std::setw(12) << row.workSeconds
            << std::setw(12) << row.wallSeconds << std::setprecision(1) << std::setw(16) << rate
            << '\n';
}

void printTableHeader() {
  std::cout << std::left << std::setw(30) << "phase" << std::right << std::setw(10) << "count"
            << "  " << std::left << std::setw(16) << "unit" << std::right << std::setw(12)
            << "work_s" << std::setw(12) << "wall_s" << std::setw(16) << "per_second" << '\n';
}

// ---------------------------------------------------------------------------
// Accounting verification
// ---------------------------------------------------------------------------

[[nodiscard]] std::uint32_t coveredSlots(const std::vector<wf::SlotRange>& runs) {
  std::uint32_t total = 0;
  for (const wf::SlotRange& run : runs) total += run.count;
  return total;
}

struct Accounting {
  std::uint64_t checks{0};
  std::uint64_t mismatches{0};
};

// Checks one domain's occupancy against the runtime's own accounting: the live
// slot total, the live reservation count, the free/live partition of the
// allocatable window, and the free run list must all agree. A disagreement is
// counted and printed here, and makes the whole benchmark fail.
void verifyDomain(wf::SpectrumRuntime& runtime, std::uint64_t domainId, wf::Instant at,
                  std::uint32_t expectedLiveSlots, std::uint32_t expectedLiveReservations,
                  Accounting& accounting, const std::string& what) {
  accounting.checks += 1;
  const std::optional<wf::SpectrumUsage> usage = runtime.usage(wf::SpectrumDomainId(domainId), at);
  if (!usage.has_value()) {
    accounting.mismatches += 1;
    std::cout << "accounting MISMATCH " << what << ": domain " << domainId
              << " has no usage record\n";
    return;
  }
  const std::uint32_t freePlusLive = usage->freeSlots + usage->liveSlots;
  const bool partitioned = freePlusLive == usage->allocatableSlots;
  const bool runsAgree = coveredSlots(usage->freeRuns) == usage->freeSlots;
  const bool ok = usage->liveSlots == expectedLiveSlots &&
                  usage->liveReservations == expectedLiveReservations && partitioned && runsAgree;
  if (!ok) {
    accounting.mismatches += 1;
    std::cout << "accounting MISMATCH " << what << ": domain " << domainId
              << " liveSlots=" << usage->liveSlots << " expected=" << expectedLiveSlots
              << " liveReservations=" << usage->liveReservations
              << " expected=" << expectedLiveReservations << " freeSlots=" << usage->freeSlots
              << " allocatableSlots=" << usage->allocatableSlots
              << " freeRunSlots=" << coveredSlots(usage->freeRuns) << '\n';
  }
}

// ---------------------------------------------------------------------------
// Scenario
// ---------------------------------------------------------------------------

[[nodiscard]] bool registerGrid(wf::SpectrumRuntime& runtime, std::uint64_t id, std::uint32_t slots,
                                std::int64_t widthMhz, wf::GridKind kind) {
  wf::ChannelGrid grid;
  grid.id = wf::ChannelGridId(id);
  grid.generation = wf::GridGeneration(1);
  grid.kind = kind;
  grid.anchorMhz = kAnchorMhz;
  grid.slotWidthMhz = widthMhz;
  grid.slotCount = slots;
  grid.minSlotsPerChannel = 1;
  grid.maxSlotsPerChannel = kind == wf::GridKind::Fixed ? 1 : 8;
  const wf::Status status = runtime.registerGrid(grid);
  if (!status.ok()) {
    std::cout << "setup FAILED register grid " << id << ": " << status.message << '\n';
    return false;
  }
  return true;
}

[[nodiscard]] bool registerDomain(wf::SpectrumRuntime& runtime, std::uint64_t id,
                                  std::uint64_t gridId, std::uint32_t gridSlots,
                                  std::int64_t widthMhz, wf::Instant at) {
  wf::SpectrumDomain domain;
  domain.id = wf::SpectrumDomainId(id);
  domain.generation = wf::SpectrumDomainGeneration(1);
  domain.klass = wf::ResourceClass::AbstractDomain;
  domain.grid = wf::ChannelGridId(gridId);
  domain.gridGeneration = wf::GridGeneration(1);
  domain.requiresContiguity = true;
  const wf::Status domainStatus = runtime.registerDomain(domain);
  if (!domainStatus.ok()) {
    std::cout << "setup FAILED register domain " << id << ": " << domainStatus.message << '\n';
    return false;
  }

  wf::SpectrumCapability capability;
  capability.domain = domain.id;
  capability.domainGeneration = domain.generation;
  capability.grid = domain.grid;
  capability.gridGeneration = domain.gridGeneration;
  capability.support = wf::SpectrumSupport::Supported;
  capability.firstAllocatableSlot = 0;
  capability.allocatableSlots = gridSlots;
  capability.minTunableMhz = kAnchorMhz;
  capability.maxTunableMhz = kAnchorMhz + static_cast<std::int64_t>(gridSlots) * widthMhz;
  capability.presenceEvidence.present = true;
  capability.presenceEvidence.digest = 0xB00000ull + id;
  capability.presenceEvidence.source = "bench_allocation";
  capability.generation = wf::CapabilityGeneration(1);
  capability.publisher = wf::ControllerId(1);
  capability.fence = runtime.fence();
  capability.publishedAt = at;
  const wf::Status capabilityStatus = runtime.publishCapability(capability);
  if (!capabilityStatus.ok()) {
    std::cout << "setup FAILED publish capability for domain " << id << ": "
              << capabilityStatus.message << '\n';
    return false;
  }
  return true;
}

class Benchmark {
 public:
  Benchmark(wf::SpectrumRuntime& runtime, std::filesystem::path statePath, wf::Instant at)
      : runtime_(runtime), statePath_(std::move(statePath)), at_(at) {}

  [[nodiscard]] bool setup();
  void runAllocationPhase();
  void runEnumerationPhase();
  void runReleasePhase();
  void runReclaimPhase();
  void runPersistencePhase();

  [[nodiscard]] const std::vector<Row>& rows() const noexcept { return rows_; }
  [[nodiscard]] const Accounting& accounting() const noexcept { return accounting_; }
  [[nodiscard]] std::uint64_t allocationFailures() const noexcept { return allocationFailures_; }
  [[nodiscard]] std::uint64_t stateBytes() const noexcept { return stateBytes_; }
  [[nodiscard]] std::uint64_t sweeps() const noexcept { return sweeps_; }

 private:
  [[nodiscard]] wf::SpectrumRequest makeRequest(std::uint32_t slots, std::uint64_t domainId,
                                                wf::Duration lease) {
    const wf::AuthorityState authority = runtime_.authorityState();
    wf::SpectrumRequest request;
    request.requestId = wf::AllocationRequestId(nextRequestId_);
    nextRequestId_ += 1;
    request.requestGeneration = wf::AllocationRequestGeneration(1);
    request.owner = wf::OwnerId(1);
    request.ownerGeneration = wf::OwnerGeneration(1);
    request.domains = {wf::SpectrumDomainId(domainId)};
    request.domainGenerations = {wf::SpectrumDomainGeneration(1)};
    request.grid = wf::ChannelGridId(domainId == kDomainAId ? kGridAId : kGridBId);
    request.gridGeneration = wf::GridGeneration(1);
    request.slots = slots;
    request.contiguity = wf::ContiguityRequirement::Required;
    request.continuity = wf::ContinuityRequirement::Required;
    request.leaseDuration = lease;
    request.requestedAt = at_;
    request.notBefore = wf::Instant{};
    request.eligibilityAuthority =
        wf::EligibilityAuthority{authority.eligibilityGeneration, authority.fence};
    request.reservationAuthority =
        wf::ReservationAuthority{authority.reservationGeneration, authority.fence};
    return request;
  }

  [[nodiscard]] wf::Status releaseReservation(wf::ReservationId id) {
    const wf::AuthorityState authority = runtime_.authorityState();
    return runtime_.release(id, wf::ReservationGeneration(1),
                            wf::ReleaseAuthority{authority.releaseGeneration, authority.fence},
                            at_);
  }

  wf::SpectrumRuntime& runtime_;
  std::filesystem::path statePath_;
  wf::Instant at_;
  wf::Duration lease_{wf::Duration::hours(1)};
  std::uint64_t nextRequestId_{1};
  std::vector<Row> rows_;
  Accounting accounting_;
  std::uint64_t allocationFailures_{0};
  std::uint64_t stateBytes_{0};
  std::uint64_t sweeps_{0};
};

[[nodiscard]] bool Benchmark::setup() {
  if (!registerGrid(runtime_, kGridAId, kGridASlots, kGridAWidthMhz, wf::GridKind::Fixed)) {
    return false;
  }
  if (!registerGrid(runtime_, kGridBId, kGridBSlots, kGridBWidthMhz, wf::GridKind::Flex)) {
    return false;
  }
  if (!registerDomain(runtime_, kDomainAId, kGridAId, kGridASlots, kGridAWidthMhz, at_)) {
    return false;
  }
  if (!registerDomain(runtime_, kDomainBId, kGridBId, kGridBSlots, kGridBWidthMhz, at_)) {
    return false;
  }

  // The live set: N reservations on grid A, one slot each, which the
  // enumeration phase then has to work around.
  for (std::size_t index = 0; index < kLiveReservations; ++index) {
    const wf::AllocationDecision decision = runtime_.allocate(makeRequest(1, kDomainAId, lease_));
    if (!decision.allocated()) {
      std::cout << "setup FAILED live reservation " << index << ": "
                << wf::toToken(decision.outcome) << '\n';
      return false;
    }
  }
  verifyDomain(runtime_, kDomainAId, at_, static_cast<std::uint32_t>(kLiveReservations),
               static_cast<std::uint32_t>(kLiveReservations), accounting_, "live set");
  return true;
}

// Phase 1: allocation completions on grid B. Every round fills the grid; the
// meter sees allocation calls only, and the drain between rounds is excluded
// (and measured again by the release phase).
void Benchmark::runAllocationPhase() {
  Meter meter;
  const Clock::time_point phaseStart = Clock::now();
  std::uint64_t completed = 0;
  std::vector<wf::ReservationId> roundReservations;
  roundReservations.reserve(kGridBSlots);
  for (std::size_t round = 0; round < kAllocationRounds; ++round) {
    roundReservations.clear();
    std::uint32_t occupied = 0;
    for (std::size_t index = 0; index < kGridBSlots; ++index) {
      const Clock::time_point started = Clock::now();
      const wf::AllocationDecision decision = runtime_.allocate(makeRequest(1, kDomainBId, lease_));
      const Clock::time_point finished = Clock::now();
      meter.add(elapsedNanos(started, finished));
      if (!decision.allocated()) {
        allocationFailures_ += 1;
        std::cout << "allocation FAILED round=" << round << " index=" << index
                  << " outcome=" << wf::toToken(decision.outcome) << '\n';
        return;
      }
      completed += 1;
      occupied += 1;
      verifyDomain(runtime_, kDomainBId, at_, occupied, static_cast<std::uint32_t>(index + 1),
                   accounting_, "allocation");
      roundReservations.push_back(decision.reservation);
    }
    for (const wf::ReservationId id : roundReservations) {
      const wf::Status status = releaseReservation(id);
      if (!status.ok()) {
        std::cout << "drain FAILED reservation=" << id.raw() << " message=" << status.message
                  << '\n';
        return;
      }
    }
    verifyDomain(runtime_, kDomainBId, at_, 0, 0, accounting_, "drain");
  }
  const double wall = toSeconds(elapsedNanos(phaseStart, Clock::now()));
  rows_.push_back(Row{"allocate grid B", completed, "allocations", meter.seconds(), wall});
}

// Phase 2: candidate enumeration on grid A, read-only, around the live set.
void Benchmark::runEnumerationPhase() {
  Meter meter;
  const Clock::time_point phaseStart = Clock::now();
  std::uint64_t calls = 0;
  std::uint64_t candidates = 0;
  std::uint64_t eligible = 0;
  for (std::size_t index = 0; index < kEnumerationCalls; ++index) {
    const Clock::time_point started = Clock::now();
    const wf::CandidateSet set =
        runtime_.enumerateCandidates(makeRequest(kEnumerationSlots, kDomainAId, lease_));
    const Clock::time_point finished = Clock::now();
    meter.add(elapsedNanos(started, finished));
    if (!set.status.ok()) {
      std::cout << "enumeration FAILED index=" << index << " message=" << set.status.message
                << '\n';
      return;
    }
    calls += 1;
    candidates += set.candidates.size();
    eligible += set.eligibleCount;
  }
  const double wall = toSeconds(elapsedNanos(phaseStart, Clock::now()));
  const double work = meter.seconds();
  rows_.push_back(Row{"enumerate grid A", calls, "enumerations", work, wall});
  rows_.push_back(Row{"enumerate candidates", candidates, "candidates", work, wall});
  std::cout << "note: enumeration returned " << candidates << " candidates, " << eligible
            << " of them eligible\n";
}

// Phase 3a: releases. Each round refills grid B outside the meter and then
// releases every reservation inside it.
void Benchmark::runReleasePhase() {
  Meter meter;
  const Clock::time_point phaseStart = Clock::now();
  std::uint64_t released = 0;
  std::vector<wf::ReservationId> roundReservations;
  roundReservations.reserve(kGridBSlots);
  for (std::size_t round = 0; round < kAllocationRounds; ++round) {
    roundReservations.clear();
    for (std::size_t index = 0; index < kGridBSlots; ++index) {
      const wf::AllocationDecision decision = runtime_.allocate(makeRequest(1, kDomainBId, lease_));
      if (!decision.allocated()) {
        std::cout << "release setup FAILED round=" << round << " index=" << index << '\n';
        return;
      }
      roundReservations.push_back(decision.reservation);
    }
    for (const wf::ReservationId id : roundReservations) {
      const Clock::time_point started = Clock::now();
      const wf::Status status = releaseReservation(id);
      const Clock::time_point finished = Clock::now();
      meter.add(elapsedNanos(started, finished));
      if (!status.ok()) {
        std::cout << "release FAILED reservation=" << id.raw() << " message=" << status.message
                  << '\n';
        return;
      }
      released += 1;
    }
    verifyDomain(runtime_, kDomainBId, at_, 0, 0, accounting_, "release drain");
  }
  const double wall = toSeconds(elapsedNanos(phaseStart, Clock::now()));
  rows_.push_back(Row{"release grid B", released, "releases", meter.seconds(), wall});
}

// Phase 3b: reclamation of lapsed leases. Each round allocates a full grid B
// with a one second lease outside the meter and reclaims it inside.
void Benchmark::runReclaimPhase() {
  Meter meter;
  const Clock::time_point phaseStart = Clock::now();
  std::uint64_t reclaimed = 0;
  const wf::Duration shortLease = wf::Duration::seconds(1);
  const wf::Instant sweepAt = at_ + wf::Duration::seconds(2);
  for (std::size_t round = 0; round < kAllocationRounds; ++round) {
    for (std::size_t index = 0; index < kGridBSlots; ++index) {
      const wf::AllocationDecision decision =
          runtime_.allocate(makeRequest(1, kDomainBId, shortLease));
      if (!decision.allocated()) {
        std::cout << "reclaim setup FAILED round=" << round << " index=" << index << '\n';
        return;
      }
    }
    const Clock::time_point started = Clock::now();
    const wf::ReclaimReport report = runtime_.reclaimExpired(sweepAt);
    const Clock::time_point finished = Clock::now();
    meter.add(elapsedNanos(started, finished));
    sweeps_ += 1;
    if (!report.status.ok() || report.reclaimed.size() != kGridBSlots) {
      std::cout << "reclaim FAILED round=" << round << " reclaimed=" << report.reclaimed.size()
                << " message=" << report.status.message << '\n';
      return;
    }
    reclaimed += report.reclaimed.size();
    verifyDomain(runtime_, kDomainBId, sweepAt, 0, 0, accounting_, "reclaim");
  }
  const double wall = toSeconds(elapsedNanos(phaseStart, Clock::now()));
  rows_.push_back(Row{"reclaim grid B", reclaimed, "reclamations", meter.seconds(), wall});
}

// Phase 4: durability. One save followed by one recovery per cycle, with the
// occupancy checked afterwards: recovery must not change what is owned.
void Benchmark::runPersistencePhase() {
  Meter saveMeter;
  Meter recoverMeter;
  const Clock::time_point phaseStart = Clock::now();
  std::uint64_t saves = 0;
  std::uint64_t recoveries = 0;
  for (std::size_t cycle = 0; cycle < kPersistenceCycles; ++cycle) {
    const Clock::time_point saveStarted = Clock::now();
    const wf::Status saved = runtime_.save();
    const Clock::time_point saveFinished = Clock::now();
    saveMeter.add(elapsedNanos(saveStarted, saveFinished));
    if (!saved.ok()) {
      std::cout << "save FAILED cycle=" << cycle << " message=" << saved.message << '\n';
      return;
    }
    saves += 1;

    std::error_code error;
    stateBytes_ = static_cast<std::uint64_t>(std::filesystem::file_size(statePath_, error));
    if (error) stateBytes_ = 0;

    const Clock::time_point recoverStarted = Clock::now();
    const wf::RecoveryReport report = runtime_.recover();
    const Clock::time_point recoverFinished = Clock::now();
    recoverMeter.add(elapsedNanos(recoverStarted, recoverFinished));
    if (!report.status.ok() || !report.recovered) {
      std::cout << "recover FAILED cycle=" << cycle << " message=" << report.status.message
                << '\n';
      return;
    }
    recoveries += 1;
    verifyDomain(runtime_, kDomainAId, at_, static_cast<std::uint32_t>(kLiveReservations),
                 static_cast<std::uint32_t>(kLiveReservations), accounting_, "recovery");
    verifyDomain(runtime_, kDomainBId, at_, 0, 0, accounting_, "recovery");
  }
  const double wall = toSeconds(elapsedNanos(phaseStart, Clock::now()));
  rows_.push_back(Row{"save state", saves, "saves", saveMeter.seconds(), wall});
  rows_.push_back(Row{"recover state", recoveries, "recoveries", recoverMeter.seconds(), wall});
}

}  // namespace

int main() {
  std::cout << "bench_allocation: synthetic in-memory workload over abstract spectrum data\n";
  std::cout << "=====================================================================\n";
  std::cout << "DISCLAIMER: every number below comes from a SYNTHETIC in-memory workload on\n"
               "            abstract spectrum slots. It describes this process's slot arithmetic,\n"
               "            not optical hardware, and it must not be used to describe or predict\n"
               "            optical hardware performance.\n\n";
  std::cout << "workload: grid A fixed " << kGridASlots << " slots @ " << kGridAWidthMhz
            << " MHz, grid B flex " << kGridBSlots << " slots @ " << kGridBWidthMhz << " MHz\n";
  std::cout << "workload: N=" << kLiveReservations << " live reservations on grid A, M="
            << (kAllocationRounds * kGridBSlots) << " allocation attempts on grid B in "
            << kAllocationRounds << " rounds of " << kGridBSlots << '\n';
  std::cout << "workload: " << kEnumerationCalls << " enumerations of " << kEnumerationSlots
            << "-slot channels on grid A, " << kPersistenceCycles << " save/recover cycles\n\n";

  std::error_code error;
  const std::filesystem::path tempDirectory = std::filesystem::temp_directory_path(error);
  if (error) {
    std::cout << "benchmark FAILED: no temporary directory is available: " << error.message()
              << '\n';
    return 1;
  }
  const std::filesystem::path statePath = tempDirectory / "wf_bench_allocation_state.bin";
  std::filesystem::remove(statePath, error);
  error.clear();

  wf::RuntimeConfig config;
  config.statePath = statePath.string();
  config.durableCommits = false;
  wf::SpectrumRuntime runtime(config);

  const wf::Instant at = wf::Instant::fromSeconds(1'700'000'000);
  Benchmark benchmark(runtime, statePath, at);
  if (!benchmark.setup()) {
    std::cout << "bench_allocation: FAILED during setup\n";
    return 1;
  }

  benchmark.runAllocationPhase();
  benchmark.runEnumerationPhase();
  benchmark.runReleasePhase();
  benchmark.runReclaimPhase();
  benchmark.runPersistencePhase();

  std::cout << '\n';
  printTableHeader();
  for (const Row& row : benchmark.rows()) printRow(row);
  std::cout << "\nnote: work_s sums the time spent inside the counted operations; wall_s is the\n"
               "      wall time of the phase, which for the allocation, release and reclaim\n"
               "      phases also contains the per-operation accounting checks and the untimed\n"
               "      refill or drain between rounds.\n";
  std::cout << "note: " << benchmark.sweeps()
            << " reclamation sweeps produced the reclamations above\n";
  std::cout << "note: the durable image is " << benchmark.stateBytes() << " bytes at "
            << statePath.string() << '\n';

  const Accounting& accounting = benchmark.accounting();
  std::cout << "verification: " << accounting.checks
            << " usage() accounting checks over completed allocations, " << accounting.mismatches
            << " mismatches\n";

  std::filesystem::remove(statePath, error);

  const bool ok = accounting.mismatches == 0 && benchmark.allocationFailures() == 0 &&
                  accounting.checks > 0;
  std::cout << "\nDISCLAIMER: these figures are synthetic in-memory measurements on abstract\n"
               "            spectrum data and do NOT describe or predict optical hardware\n"
               "            performance.\n";
  std::cout << "bench_allocation: " << (ok ? "PASS" : "FAIL") << '\n';
  return ok ? 0 : 1;
}
