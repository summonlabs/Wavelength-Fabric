#include "test_common.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

// Seeded, deterministic property tests over randomized operation sequences.
//
// Every property is checked against a slow reference model declared in this
// file: a per-slot occupancy bitmap per domain plus a first-fit search written
// from scratch. The model never asks the runtime for an expectation; it
// recomputes occupancy, first fit, expiry and usage from the request stream
// alone. A failure prints the seed, the step, the parameters that produced it,
// and both the actual and the expected value.

namespace {

using namespace wavelength_fabric;

// A deliberately small grid for the pressure scenario: sixteen slots per domain
// force real contention so that refusals, not only commits, are exercised.
constexpr std::uint32_t kPressureSlots = 16;
// A roomy grid for the multi-domain scenarios, which are about placement shape
// rather than capacity pressure.
constexpr std::uint32_t kSparseSlots = 48;

constexpr std::int64_t kAnchorMhz = 191'300'000;
constexpr std::int64_t kSlotWidthMhz = 12'500;
constexpr std::uint32_t kMaxChannelSlots = 8;
constexpr std::uint32_t kMaxRequestedSlots = 4;

const std::vector<SpectrumDomainId> kDomains = {SpectrumDomainId(1), SpectrumDomainId(2),
                                                SpectrumDomainId(3)};

const Instant kBase = Instant::fromSeconds(1'800'000'000);
const Duration kLease = Duration::seconds(600);
const Duration kStepDuration = Duration::seconds(300);
const Duration kRenewExtension = Duration::seconds(300);

std::string g_context = "seed=unset step=unset";

// ---------------------------------------------------------------------------
// Rendering and assertions. Every failure carries the actual and the expected
// value, so a report is actionable without a debugger.
// ---------------------------------------------------------------------------

std::string renderValue(const std::vector<SlotRange>& ranges) {
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

std::string renderValue(const Status& status) {
  return std::string(toToken(status.code)) + "(" + status.message + ")";
}

template <class T>
std::string renderValue(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    return std::string(toToken(value));
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(value);
  } else {
    return std::string("<?>");
  }
}

#define WF_PROP(expr, detail)                                          \
  do {                                                                 \
    ::wf_test::Harness::instance().noteCheck();                        \
    if (!(expr)) {                                                     \
      ::wf_test::Harness::instance().fail(                             \
          __FILE__, __LINE__,                                          \
          std::string("expected: " #expr " : ") + std::string(detail) + \
              " [" + g_context + "]");                                 \
    }                                                                  \
  } while (false)

#define WF_PROP_EQ(actual, expected, what)                                       \
  do {                                                                           \
    ::wf_test::Harness::instance().noteCheck();                                  \
    const auto& wf_actual = (actual);                                            \
    const auto& wf_expected = (expected);                                        \
    if (!(wf_actual == wf_expected)) {                                           \
      ::wf_test::Harness::instance().fail(                                       \
          __FILE__, __LINE__,                                                    \
          std::string(what) + ": actual=" + renderValue(wf_actual) +             \
              " expected=" + renderValue(wf_expected) + " [" + g_context + "]"); \
    }                                                                            \
  } while (false)

// ---------------------------------------------------------------------------
// Deterministic generator. SplitMix64: one fixed seed fully determines the
// scenario, and the same seed always replays the identical decision sequence.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Reference model
// ---------------------------------------------------------------------------

bool modelLiveState(ReservationState state) {
  switch (state) {
    case ReservationState::Reserved:
    case ReservationState::Active:
      return true;
    default:
      return false;
  }
}

struct ModelReservation {
  ReservationId id{};
  ReservationGeneration generation{ReservationGeneration(1)};
  std::vector<SpectrumDomainId> domains;
  SlotRange slots{};
  ReservationState state{ReservationState::Reserved};
  Instant expiresAt{};
};

using Model = std::map<ReservationId, ModelReservation>;

bool modelLiveAt(const ModelReservation& reservation, Instant now) {
  return modelLiveState(reservation.state) && reservation.expiresAt > now;
}

bool spansDomain(const ModelReservation& reservation, SpectrumDomainId domain) {
  return std::find(reservation.domains.begin(), reservation.domains.end(), domain) !=
         reservation.domains.end();
}

bool sharesDomain(const ModelReservation& left, const ModelReservation& right) {
  for (const SpectrumDomainId domain : left.domains) {
    if (spansDomain(right, domain)) return true;
  }
  return false;
}

// Independent occupancy recomputation: a slot is taken when a reservation that
// is still live at the instant covers it on a domain the candidate also spans.
bool slotFree(const Model& model, const std::vector<SpectrumDomainId>& requestDomains,
              std::uint32_t slot, Instant now) {
  for (const auto& entry : model) {
    const ModelReservation& reservation = entry.second;
    if (!modelLiveAt(reservation, now)) continue;
    bool shared = false;
    for (const SpectrumDomainId domain : reservation.domains) {
      if (std::find(requestDomains.begin(), requestDomains.end(), domain) !=
          requestDomains.end()) {
        shared = true;
        break;
      }
    }
    if (!shared) continue;
    if (slot >= reservation.slots.first && slot < reservation.slots.end()) return false;
  }
  return true;
}

// First fit written from scratch: the lowest start whose whole window is free on
// every requested domain. A multi-domain window must be free on every domain it
// spans, so the free spaces are intersected.
bool referenceFirstFit(const Model& model, const std::vector<SpectrumDomainId>& requestDomains,
                       std::uint32_t slots, Instant now, std::uint32_t slotCount,
                       std::uint32_t& start) {
  if (slots == 0 || slots > slotCount) return false;
  for (std::uint32_t candidate = 0; candidate + slots <= slotCount; ++candidate) {
    bool free = true;
    for (std::uint32_t offset = 0; offset < slots; ++offset) {
      if (!slotFree(model, requestDomains, candidate + offset, now)) {
        free = false;
        break;
      }
    }
    if (free) {
      start = candidate;
      return true;
    }
  }
  return false;
}

struct ReferenceUsage {
  std::uint32_t liveSlots{0};
  std::uint32_t activeSlots{0};
  std::uint32_t reservedSlots{0};
  std::uint32_t lapsedSlots{0};
  std::uint32_t liveReservations{0};
  std::uint32_t activeReservations{0};
  std::uint32_t lapsedReservations{0};
  std::uint32_t reclaimedReservations{0};
  std::uint32_t releasedReservations{0};
  std::vector<SlotRange> freeRuns;
};

std::uint32_t coveredSlots(const std::vector<bool>& bitmap) {
  std::uint32_t total = 0;
  for (const bool taken : bitmap) {
    if (taken) ++total;
  }
  return total;
}

// Live, active and reserved accounting comes only from reservations whose lease
// is still valid; reclaimed and released accounting counts terminal
// reservations for the domain from the full reservation table.
ReferenceUsage referenceUsage(const Model& model, SpectrumDomainId domain, Instant now,
                              std::uint32_t slotCount) {
  ReferenceUsage out;
  std::vector<bool> live(slotCount, false);
  std::vector<bool> active(slotCount, false);
  std::vector<bool> reserved(slotCount, false);
  std::vector<bool> lapsed(slotCount, false);

  for (const auto& entry : model) {
    const ModelReservation& reservation = entry.second;
    if (!spansDomain(reservation, domain)) continue;
    if (reservation.state == ReservationState::Reclaimed) {
      ++out.reclaimedReservations;
      continue;
    }
    if (reservation.state == ReservationState::Released) {
      ++out.releasedReservations;
      continue;
    }
    if (!modelLiveState(reservation.state)) continue;

    const bool valid = reservation.expiresAt > now;
    std::vector<bool>* bitmap = nullptr;
    if (!valid) {
      ++out.lapsedReservations;
      bitmap = &lapsed;
    } else {
      ++out.liveReservations;
      if (reservation.state == ReservationState::Active) {
        ++out.activeReservations;
        bitmap = &active;
      } else {
        bitmap = &reserved;
      }
    }
    for (std::uint32_t slot = reservation.slots.first; slot < reservation.slots.end(); ++slot) {
      (*bitmap)[slot] = true;
      if (valid) live[slot] = true;
    }
  }

  out.liveSlots = coveredSlots(live);
  out.activeSlots = coveredSlots(active);
  out.reservedSlots = coveredSlots(reserved);
  out.lapsedSlots = coveredSlots(lapsed);

  std::uint32_t cursor = 0;
  while (cursor < slotCount) {
    if (live[cursor]) {
      ++cursor;
      continue;
    }
    std::uint32_t end = cursor;
    while (end < slotCount && !live[end]) ++end;
    out.freeRuns.push_back(SlotRange{cursor, end - cursor});
    cursor = end;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Fixture setup
// ---------------------------------------------------------------------------

void setupRuntime(SpectrumRuntime& runtime, std::uint32_t slotCount) {
  const std::int64_t endMhz =
      kAnchorMhz + kSlotWidthMhz * static_cast<std::int64_t>(slotCount);
  const ChannelGrid grid = wf_test::flexGrid(ChannelGridId(1), GridGeneration(1), kAnchorMhz,
                                             kSlotWidthMhz, slotCount, 1, kMaxChannelSlots);
  WF_CHECK(runtime.registerGrid(grid).ok());
  for (const SpectrumDomainId domain : kDomains) {
    WF_CHECK(runtime.registerDomain(wf_test::makeDomain(domain, ChannelGridId(1))).ok());
    SpectrumCapability capability = wf_test::makeCapability(
        domain, ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
        SpectrumSupport::Supported, 0, slotCount, runtime.fence());
    capability.minTunableMhz = kAnchorMhz;
    capability.maxTunableMhz = endMhz;
    WF_CHECK(runtime.publishCapability(capability).ok());
  }
}

std::vector<SpectrumDomainId> pickDomains(Rng& rng) {
  const std::uint32_t mask = 1u + rng.below((1u << kDomains.size()) - 1u);
  std::vector<SpectrumDomainId> chosen;
  for (std::size_t index = 0; index < kDomains.size(); ++index) {
    if ((mask & (1u << index)) != 0u) chosen.push_back(kDomains[index]);
  }
  return chosen;
}

std::vector<SpectrumDomainGeneration> generationsFor(
    const std::vector<SpectrumDomainId>& domains) {
  return std::vector<SpectrumDomainGeneration>(domains.size(), SpectrumDomainGeneration(1));
}

ContiguityRequirement contiguityFor(std::uint32_t slots) {
  return slots > 1 ? ContiguityRequirement::Required : ContiguityRequirement::Unspecified;
}

ContinuityRequirement continuityFor(std::size_t domainCount) {
  return domainCount > 1 ? ContinuityRequirement::Required : ContinuityRequirement::Unspecified;
}

SpectrumRequest buildRequest(const wf_test::Fixture& fixture, AllocationRequestId requestId,
                             const std::vector<SpectrumDomainId>& domains, std::uint32_t slots,
                             Instant requestedAt, ContiguityRequirement contiguity,
                             ContinuityRequirement continuity) {
  return wf_test::makeRequest(requestId, OwnerId(1), domains, generationsFor(domains),
                              ChannelGridId(1), GridGeneration(1), slots, requestedAt, kLease,
                              fixture.eligibility(), fixture.reservation(), contiguity, continuity);
}

const SpectrumCandidate* firstEligibleCandidate(const CandidateSet& set) {
  for (const SpectrumCandidate& candidate : set.candidates) {
    if (isEligible(candidate.eligibility)) return &candidate;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Full state comparison against the reference model
// ---------------------------------------------------------------------------

void requireNoOverlap(const SpectrumRuntime& runtime, Instant now) {
  const std::vector<SpectrumReservation> reservations = runtime.reservations();
  for (std::size_t left = 0; left < reservations.size(); ++left) {
    const SpectrumReservation& a = reservations[left];
    if (!a.isLiveAt(now)) continue;
    for (std::size_t right = left + 1; right < reservations.size(); ++right) {
      const SpectrumReservation& b = reservations[right];
      if (!b.isLiveAt(now)) continue;
      for (std::size_t index = 0; index < a.domains.size(); ++index) {
        const SpectrumDomainId domain = a.domains[index];
        const auto position = std::find(b.domains.begin(), b.domains.end(), domain);
        if (position == b.domains.end()) continue;
        const std::size_t other = static_cast<std::size_t>(position - b.domains.begin());
        const SlotRange rangeA = index < a.perDomainSlots.size() ? a.perDomainSlots[index] : a.slots;
        const SlotRange rangeB =
            other < b.perDomainSlots.size() ? b.perDomainSlots[other] : b.slots;
        WF_PROP(!rangeA.overlaps(rangeB),
                "reservations " + std::to_string(a.id.raw()) + " " + renderValue(rangeA) +
                    " and " + std::to_string(b.id.raw()) + " " + renderValue(rangeB) +
                    " both own domain " + std::to_string(domain.raw()) +
                    " while live at " + std::to_string(now.nanos()));
      }
    }
  }
}

void verifyState(const SpectrumRuntime& runtime, const Model& model, Instant now,
                 std::uint32_t slotCount) {
  const std::vector<SpectrumReservation> reservations = runtime.reservations();
  WF_PROP_EQ(reservations.size(), model.size(),
             "reservation count must equal the reference model's reservation count");

  for (const SpectrumReservation& reservation : reservations) {
    const auto position = model.find(reservation.id);
    WF_PROP(position != model.end(),
            "runtime reservation " + std::to_string(reservation.id.raw()) +
                " has no model counterpart");
    if (position == model.end()) continue;
    const ModelReservation& expected = position->second;
    g_context += " reservation=" + std::to_string(reservation.id.raw());
    WF_PROP_EQ(reservation.state, expected.state, "reservation state must match the model");
    WF_PROP_EQ(reservation.generation, expected.generation,
                "reservation generation must match the model");
    WF_PROP_EQ(reservation.lease.expiresAt.nanos(), expected.expiresAt.nanos(),
                "reservation lease expiry must match the model");
    WF_PROP(reservation.domains == expected.domains,
            "reservation domain list: actual=" + std::to_string(reservation.domains.size()) +
                " domains, expected=" + std::to_string(expected.domains.size()) + " domains");
    WF_PROP_EQ(reservation.slots.first, expected.slots.first,
                "reservation slot range start must match the model");
    WF_PROP_EQ(reservation.slots.count, expected.slots.count,
                "reservation slot range width must match the model");
    WF_PROP_EQ(reservation.perDomainSlots.size(), expected.domains.size(),
                "per-domain slot vector must be parallel to the domain list");
    for (std::size_t index = 0; index < reservation.perDomainSlots.size() &&
                                index < expected.domains.size();
         ++index) {
      WF_PROP_EQ(reservation.perDomainSlots[index].first, expected.slots.first,
                  "per-domain range start must equal the model's committed start");
      WF_PROP_EQ(reservation.perDomainSlots[index].count, expected.slots.count,
                  "per-domain range width must equal the model's committed width");
    }
  }

  for (const SpectrumDomainId domain : kDomains) {
    const std::optional<SpectrumUsage> usage = runtime.usage(domain, now);
    WF_PROP(usage.has_value(),
            "usage must exist for domain " + std::to_string(domain.raw()));
    if (!usage.has_value()) continue;
    const ReferenceUsage reference = referenceUsage(model, domain, now, slotCount);
    g_context += " domain=" + std::to_string(domain.raw());
    WF_PROP_EQ(usage->totalSlots, slotCount, "domain total slots");
    WF_PROP_EQ(usage->allocatableSlots, slotCount, "domain allocatable slots");
    WF_PROP_EQ(usage->liveSlots, reference.liveSlots, "domain live slots");
    WF_PROP_EQ(usage->activeSlots, reference.activeSlots, "domain active slots");
    WF_PROP_EQ(usage->reservedSlots, reference.reservedSlots, "domain reserved slots");
    WF_PROP_EQ(usage->lapsedSlots, reference.lapsedSlots, "domain lapsed slots");
    WF_PROP_EQ(usage->liveReservations, reference.liveReservations, "domain live reservations");
    WF_PROP_EQ(usage->activeReservations, reference.activeReservations,
                "domain active reservations");
    WF_PROP_EQ(usage->lapsedReservations, reference.lapsedReservations,
                "domain lapsed reservations");
    WF_PROP_EQ(usage->reclaimedReservations, reference.reclaimedReservations,
                "domain reclaimed reservations");
    WF_PROP_EQ(usage->releasedReservations, reference.releasedReservations,
                "domain released reservations");
    WF_PROP(usage->freeRuns == reference.freeRuns,
            "domain free runs: actual=" + renderValue(usage->freeRuns) +
                " expected=" + renderValue(reference.freeRuns));
    WF_PROP_EQ(usage->freeSlots, slotCount - reference.liveSlots, "domain free slots");
  }

  requireNoOverlap(runtime, now);
  WF_PROP_EQ(runtime.stats().allocationsCommitted, model.size(),
              "committed allocation count must equal the model's reservation count");
}

// ---------------------------------------------------------------------------
// Trace helpers. The trace deliberately excludes the controller fence: every
// runtime construction mints a fresh incarnation, so the fence is the one value
// that legitimately differs between two otherwise identical runs.
// ---------------------------------------------------------------------------

std::string decisionTrace(const AllocationDecision& decision) {
  std::string out = "outcome=";
  out += toToken(decision.outcome);
  out += " status=";
  out += toToken(decision.status.code);
  out += decision.allocated() ? " allocated=1" : " allocated=0";
  out += " reservation=";
  out += std::to_string(decision.reservation.raw());
  out += " generation=";
  out += std::to_string(decision.generation.raw());
  out += " ordinal=";
  out += std::to_string(decision.explanation.candidateOrdinal);
  out += " slots=";
  out += std::to_string(decision.explanation.selected.slots.first);
  out.push_back('+');
  out += std::to_string(decision.explanation.selected.slots.count);
  out += " enumerated=";
  out += std::to_string(decision.explanation.candidatesEnumerated);
  out += " rejected=";
  out += std::to_string(decision.explanation.candidatesRejected);
  out += " conflicts=";
  out += std::to_string(decision.explanation.conflicts.size());
  out += " reason=";
  if (!decision.explanation.reasons.empty()) out += decision.explanation.reasons.front();
  return out;
}

std::string stateFingerprint(const SpectrumRuntime& runtime, Instant now) {
  std::string out = "state";
  for (const SpectrumReservation& reservation : runtime.reservations()) {
    out.push_back('|');
    out += std::to_string(reservation.id.raw());
    out.push_back(':');
    out += toToken(reservation.state);
    out.push_back(':');
    out += std::to_string(reservation.generation.raw());
    out.push_back(':');
    out += std::to_string(reservation.slots.first);
    out.push_back('+');
    out += std::to_string(reservation.slots.count);
    out.push_back(':');
    out += std::to_string(reservation.lease.expiresAt.nanos());
    out.push_back(':');
    out += std::to_string(reservation.domains.size());
  }
  for (const SpectrumDomainId domain : kDomains) {
    const std::optional<SpectrumUsage> usage = runtime.usage(domain, now);
    out += "|u";
    out += std::to_string(domain.raw());
    out.push_back(':');
    if (!usage.has_value()) {
      out += "none";
      continue;
    }
    out += std::to_string(usage->liveSlots);
    out.push_back('/');
    out += std::to_string(usage->activeSlots);
    out.push_back('/');
    out += std::to_string(usage->reservedSlots);
    out.push_back('/');
    out += std::to_string(usage->lapsedSlots);
    out.push_back('/');
    out += std::to_string(usage->freeSlots);
    out.push_back('/');
    out += std::to_string(usage->releasedReservations);
    out.push_back('/');
    out += std::to_string(usage->reclaimedReservations);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Scenario runner
// ---------------------------------------------------------------------------

struct Counters {
  std::uint64_t releases{0};
  std::uint64_t renewals{0};
  std::uint64_t activations{0};
  std::uint64_t deactivations{0};
  std::uint64_t reclamations{0};
  std::uint64_t sweeps{0};
  std::uint64_t allocationsCommitted{0};
  std::uint64_t allocationsRefused{0};
  std::uint64_t singleDomainCommitted{0};
  std::uint64_t singleDomainRefused{0};
  std::uint64_t multiDomainCommitted{0};
  std::uint64_t nextReservationId{1};
};

void releaseEverything(SpectrumRuntime& runtime, const wf_test::Fixture& fixture, Model& model,
                       Instant now, Counters& counters) {
  for (auto& entry : model) {
    ModelReservation& reservation = entry.second;
    if (!modelLiveState(reservation.state)) continue;
    const Status status =
        runtime.release(reservation.id, reservation.generation, fixture.release(), now);
    WF_PROP(status.ok(), "releasing reservation " + std::to_string(reservation.id.raw()) +
                             " at the end of the scenario failed: " + renderValue(status));
    reservation.state = ReservationState::Released;
    ++counters.releases;
  }
}

void runScenario(std::uint64_t seed, std::size_t steps, std::vector<std::string>* trace,
                 Counters* counters) {
  wf_test::Fixture fixture;
  setupRuntime(fixture.runtime, kPressureSlots);
  Model model;
  Counters local;
  Instant now = kBase;
  std::uint64_t nextRequest = 1;
  Rng rng(seed);

  for (std::size_t step = 0; step < steps; ++step) {
    const std::uint32_t choice = rng.below(12);
    g_context = "seed=" + std::to_string(seed) + " step=" + std::to_string(step) +
                " now=" + std::to_string(now.nanos());

    if (choice < 5) {
      const std::vector<SpectrumDomainId> domains = pickDomains(rng);
      const std::uint32_t slots = 1 + rng.below(kMaxRequestedSlots);
      const AllocationRequestId requestId(nextRequest++);
      std::uint32_t referenceStart = 0;
      const bool fits =
          referenceFirstFit(model, domains, slots, now, kPressureSlots, referenceStart);
      g_context += " op=allocate domains=" + std::to_string(domains.size()) + " slots=" +
                   std::to_string(slots) + " request=" + std::to_string(requestId.raw());

      const SpectrumRequest request =
          buildRequest(fixture, requestId, domains, slots, now, contiguityFor(slots),
                       continuityFor(domains.size()));
      const CandidateSet candidates = fixture.runtime.enumerateCandidates(request);
      WF_PROP(candidates.status.ok(),
              "enumerating candidates failed: " + renderValue(candidates.status));
      const SpectrumCandidate* first = firstEligibleCandidate(candidates);
      if (fits) {
        WF_PROP(first != nullptr,
                "the reference model found a first fit at slot " + std::to_string(referenceStart) +
                    " but no candidate is eligible");
        if (first != nullptr) {
          WF_PROP_EQ(first->slots.first, referenceStart,
                     "the first eligible candidate must start at the reference first fit");
          WF_PROP_EQ(first->slots.count, slots,
                     "the first eligible candidate width must be the requested width");
          FrequencyRange expectedFrequency;
          const ChannelGrid grid = *fixture.runtime.grid(ChannelGridId(1));
          WF_PROP(frequencyOfSlotRange(grid, first->slots, expectedFrequency),
                  "the first eligible candidate must have a representable frequency range");
          WF_PROP(first->frequency == expectedFrequency,
                  "candidate frequency: actual=[" + std::to_string(first->frequency.lowMhz) + "," +
                      std::to_string(first->frequency.highMhz) + ") expected=[" +
                      std::to_string(expectedFrequency.lowMhz) + "," +
                      std::to_string(expectedFrequency.highMhz) + ")");
        }
      } else {
        WF_PROP(first == nullptr,
                "the reference model found no fit, but a candidate is eligible: " +
                    (first == nullptr ? std::string("-") : first->describe()));
      }

      const AllocationDecision decision = fixture.runtime.allocate(request);
      if (fits) {
        WF_PROP(decision.allocated(),
                "a reachable first fit must be committed, got outcome=" +
                    std::string(toToken(decision.outcome)) + " status=" +
                    renderValue(decision.status));
        WF_PROP(decision.status.ok(), "a committed allocation carries an Ok status, got " +
                                          renderValue(decision.status));
        WF_PROP_EQ(decision.reservation.raw(), local.nextReservationId,
                   "reservation identities must be assigned in exact ascending order");
        WF_PROP_EQ(decision.explanation.outcome, AllocationOutcome::Allocated,
                   "the explanation outcome must be Allocated");
        WF_PROP_EQ(decision.explanation.selected.slots.first, referenceStart,
                   "the committed candidate must be the reference first fit");
        WF_PROP_EQ(decision.explanation.candidateOrdinal, first == nullptr ? 0 : first->ordinal,
                   "the committed candidate ordinal must be the first eligible ordinal");
        const std::optional<SpectrumReservation> stored =
            fixture.runtime.reservation(decision.reservation);
        WF_PROP(stored.has_value(), "the committed reservation must be queryable");
        if (stored.has_value()) {
          WF_PROP_EQ(stored->slots.first, referenceStart,
                     "the stored slot range must equal the first fit");
          WF_PROP_EQ(stored->slots.count, slots, "the stored slot range width");
          WF_PROP_EQ(stored->state, ReservationState::Reserved,
                     "a committed reservation starts Reserved");
          WF_PROP_EQ(stored->lease.grantedAt.nanos(), now.nanos(),
                     "the lease starts at the request instant");
          WF_PROP_EQ(stored->lease.expiresAt.nanos(), (now + kLease).nanos(),
                     "the lease ends one lease duration after the start");
          WF_PROP(stored->domains == domains,
                  "the reservation must name exactly the requested domains: actual=" +
                      std::to_string(stored->domains.size()) + " expected=" +
                      std::to_string(domains.size()));
        }
        ModelReservation entry;
        entry.id = decision.reservation;
        entry.generation = decision.generation;
        entry.domains = domains;
        entry.slots = SlotRange{referenceStart, slots};
        entry.state = ReservationState::Reserved;
        entry.expiresAt = now + kLease;
        model.emplace(entry.id, entry);
        local.nextReservationId += 1;
        local.allocationsCommitted += 1;
        if (domains.size() == 1) {
          local.singleDomainCommitted += 1;
        } else {
          local.multiDomainCommitted += 1;
        }
      } else {
        WF_PROP(!decision.allocated(),
                "an unplaceable request must not be reported as allocated; selected=" +
                    decision.explanation.selected.describe());
        WF_PROP(isRefusal(decision.outcome),
                "an unplaceable request must carry a typed refusal, got outcome=" +
                    std::string(toToken(decision.outcome)));
        WF_PROP(!decision.status.ok(),
                "a refusal must not carry an Ok status, got " + renderValue(decision.status));
        WF_PROP(decision.reservation.none(),
                "a refusal must not name a reservation, got " +
                    std::to_string(decision.reservation.raw()));
        WF_PROP(!decision.explanation.reasons.empty(), "a refusal must carry a decisive reason");
        WF_PROP_EQ(decision.explanation.outcome, decision.outcome,
                   "the explanation outcome must agree with the decision outcome");
        if (candidates.candidates.empty()) {
          WF_PROP_EQ(decision.outcome, AllocationOutcome::RefusedNoCapacity,
                     "an empty candidate list must be reported as no capacity");
        }
        local.allocationsRefused += 1;
        if (domains.size() == 1) local.singleDomainRefused += 1;
      }
      if (trace != nullptr) trace->push_back(decisionTrace(decision));
      verifyState(fixture.runtime, model, now, kPressureSlots);
      if (trace != nullptr) trace->push_back(stateFingerprint(fixture.runtime, now));
      continue;
    }

    if (choice == 5 || choice == 6) {
      if (model.empty()) continue;
      const std::size_t index = rng.below(static_cast<std::uint32_t>(model.size()));
      auto position = model.begin();
      std::advance(position, static_cast<std::ptrdiff_t>(index));
      const ReservationId id = position->first;
      ModelReservation& reservation = position->second;
      g_context += " op=release reservation=" + std::to_string(id.raw());
      if (modelLiveState(reservation.state)) {
        if (choice == 6) {
          const Status stale = fixture.runtime.release(
              id, ReservationGeneration(reservation.generation.raw() + 1), fixture.release(), now);
          WF_PROP_EQ(stale.code, StatusCode::StaleGeneration,
                     "releasing with a mismatched generation must be refused as stale");
          verifyState(fixture.runtime, model, now, kPressureSlots);
        }
        const Status status =
            fixture.runtime.release(id, reservation.generation, fixture.release(), now);
        WF_PROP(status.ok(), "releasing a live reservation failed: " + renderValue(status));
        reservation.state = ReservationState::Released;
        local.releases += 1;
        const Status duplicate =
            fixture.runtime.release(id, reservation.generation, fixture.release(), now);
        WF_PROP_EQ(duplicate.code, StatusCode::IllegalTransition,
                   "a duplicate release must be refused and must not create capacity");
      } else {
        const Status status =
            fixture.runtime.release(id, reservation.generation, fixture.release(), now);
        WF_PROP_EQ(status.code, StatusCode::IllegalTransition,
                   "releasing a reservation that owns no spectrum must be refused");
      }
      verifyState(fixture.runtime, model, now, kPressureSlots);
      if (trace != nullptr) trace->push_back(stateFingerprint(fixture.runtime, now));
      continue;
    }

    if (choice == 7) {
      if (model.empty()) continue;
      const std::size_t index = rng.below(static_cast<std::uint32_t>(model.size()));
      auto position = model.begin();
      std::advance(position, static_cast<std::ptrdiff_t>(index));
      ModelReservation& reservation = position->second;
      g_context += " op=renew reservation=" + std::to_string(position->first.raw());
      const bool renewable = reservation.state == ReservationState::Reserved ||
                             reservation.state == ReservationState::Active;
      if (renewable && reservation.expiresAt > now) {
        const Status status = fixture.runtime.renew(position->first, reservation.generation,
                                                    kRenewExtension, fixture.reservation(), now, 0);
        WF_PROP(status.ok(), "renewing a live reservation failed: " + renderValue(status));
        reservation.expiresAt = reservation.expiresAt + kRenewExtension;
        reservation.generation = ReservationGeneration(reservation.generation.raw() + 1);
        local.renewals += 1;
      } else if (renewable) {
        const Status status = fixture.runtime.renew(position->first, reservation.generation,
                                                    kRenewExtension, fixture.reservation(), now, 0);
        WF_PROP_EQ(status.code, StatusCode::Refused,
                   "renewing a lapsed lease must be refused without extending it");
      } else {
        const Status status = fixture.runtime.renew(position->first, reservation.generation,
                                                    kRenewExtension, fixture.reservation(), now, 0);
        WF_PROP_EQ(status.code, StatusCode::IllegalTransition,
                   "renewing a released reservation must be refused");
      }
      verifyState(fixture.runtime, model, now, kPressureSlots);
      if (trace != nullptr) trace->push_back(stateFingerprint(fixture.runtime, now));
      continue;
    }

    if (choice == 8) {
      if (model.empty()) continue;
      const std::size_t index = rng.below(static_cast<std::uint32_t>(model.size()));
      auto position = model.begin();
      std::advance(position, static_cast<std::ptrdiff_t>(index));
      ModelReservation& reservation = position->second;
      g_context += " op=activate reservation=" + std::to_string(position->first.raw());
      const Status status = fixture.runtime.activate(position->first, reservation.generation,
                                                     fixture.activation(), now);
      if (reservation.state == ReservationState::Reserved && reservation.expiresAt > now) {
        WF_PROP(status.ok(), "activating a reserved reservation with a live lease failed: " +
                                 renderValue(status));
        reservation.state = ReservationState::Active;
        local.activations += 1;
      } else if (reservation.state == ReservationState::Reserved) {
        WF_PROP_EQ(status.code, StatusCode::Refused, "activating a lapsed lease must be refused");
      } else {
        WF_PROP_EQ(status.code, StatusCode::IllegalTransition,
                   "activating a reservation that is not reserved must be refused");
      }
      verifyState(fixture.runtime, model, now, kPressureSlots);
      if (trace != nullptr) trace->push_back(stateFingerprint(fixture.runtime, now));
      continue;
    }

    if (choice == 9) {
      if (model.empty()) continue;
      const std::size_t index = rng.below(static_cast<std::uint32_t>(model.size()));
      auto position = model.begin();
      std::advance(position, static_cast<std::ptrdiff_t>(index));
      ModelReservation& reservation = position->second;
      g_context += " op=deactivate reservation=" + std::to_string(position->first.raw());
      const Status status = fixture.runtime.deactivate(position->first, reservation.generation,
                                                       fixture.activation(), now);
      if (reservation.state == ReservationState::Active) {
        WF_PROP(status.ok(), "deactivating an active reservation failed: " + renderValue(status));
        reservation.state = ReservationState::Reserved;
        local.deactivations += 1;
      } else {
        WF_PROP_EQ(status.code, StatusCode::IllegalTransition,
                   "deactivating a reservation that is not active must be refused");
      }
      verifyState(fixture.runtime, model, now, kPressureSlots);
      if (trace != nullptr) trace->push_back(stateFingerprint(fixture.runtime, now));
      continue;
    }

    if (choice == 10) {
      now = now + kStepDuration;
      g_context += " op=expireLeases";
      std::vector<ReservationId> expectedLapsed;
      for (auto& entry : model) {
        ModelReservation& reservation = entry.second;
        if (!modelLiveState(reservation.state)) continue;
        if (reservation.expiresAt > now) continue;
        reservation.state = ReservationState::Expired;
        expectedLapsed.push_back(entry.first);
      }
      const ReclaimReport report = fixture.runtime.expireLeases(now);
      std::string actualLapsed;
      for (const ReservationId id : report.lapsed) actualLapsed += std::to_string(id.raw()) + ",";
      std::string wantedLapsed;
      for (const ReservationId id : expectedLapsed) wantedLapsed += std::to_string(id.raw()) + ",";
      WF_PROP(report.status.ok(), "an expiry sweep must report Ok: " + renderValue(report.status));
      WF_PROP_EQ(report.evaluatedAt.nanos(), now.nanos(), "the sweep must record its instant");
      WF_PROP_EQ(report.scanned, model.size(), "the sweep must scan every reservation once");
      WF_PROP(report.lapsed == expectedLapsed,
              "lapsed set: actual=[" + actualLapsed + "] expected=[" + wantedLapsed + "]");
      WF_PROP_EQ(report.expired, expectedLapsed.size(), "the expired counter");
      WF_PROP(report.reclaimed.empty(),
              "marking a lease lapsed must never reclaim it, got " +
                  std::to_string(report.reclaimed.size()) + " reclaimed");
      local.sweeps += 1;
      verifyState(fixture.runtime, model, now, kPressureSlots);
      if (trace != nullptr) trace->push_back(stateFingerprint(fixture.runtime, now));
      continue;
    }

    now = now + kStepDuration;
    g_context += " op=reclaimExpired";
    std::vector<ReservationId> expectedLapsed;
    for (auto& entry : model) {
      ModelReservation& reservation = entry.second;
      if (!modelLiveState(reservation.state)) continue;
      if (reservation.expiresAt > now) continue;
      reservation.state = ReservationState::Expired;
      expectedLapsed.push_back(entry.first);
    }
    std::vector<ReservationId> expectedReclaimed;
    for (auto& entry : model) {
      ModelReservation& reservation = entry.second;
      if (reservation.state != ReservationState::Expired) continue;
      reservation.state = ReservationState::Reclaimed;
      expectedReclaimed.push_back(entry.first);
    }
    const ReclaimReport report = fixture.runtime.reclaimExpired(now);
    std::string actualReclaimed;
    for (const ReservationId id : report.reclaimed) actualReclaimed += std::to_string(id.raw()) + ",";
    std::string wantedReclaimed;
    for (const ReservationId id : expectedReclaimed) wantedReclaimed += std::to_string(id.raw()) + ",";
    WF_PROP(report.status.ok(), "a reclamation sweep must report Ok: " + renderValue(report.status));
    WF_PROP_EQ(report.evaluatedAt.nanos(), now.nanos(), "the sweep must record its instant");
    WF_PROP_EQ(report.scanned, model.size(), "the sweep must scan every reservation once");
    WF_PROP(report.lapsed == expectedLapsed,
            "lapsed set: actual size=" + std::to_string(report.lapsed.size()) +
                " expected size=" + std::to_string(expectedLapsed.size()));
    WF_PROP_EQ(report.expired, expectedLapsed.size(), "the expired counter");
    WF_PROP(report.reclaimed == expectedReclaimed,
            "reclaimed set: actual=[" + actualReclaimed + "] expected=[" + wantedReclaimed + "]");
    local.reclamations += report.reclaimed.size();
    local.sweeps += 1;
    verifyState(fixture.runtime, model, now, kPressureSlots);
    if (trace != nullptr) trace->push_back(stateFingerprint(fixture.runtime, now));
  }

  // ---- releasing and reclaiming everything returns the domain to baseline ---
  releaseEverything(fixture.runtime, fixture, model, now, local);
  std::vector<ReservationId> expectedReclaimed;
  for (const auto& entry : model) {
    if (entry.second.state == ReservationState::Expired) expectedReclaimed.push_back(entry.first);
  }
  const Instant after = now + Duration::seconds(100000);
  const ReclaimReport remainder = fixture.runtime.reclaimExpired(after);
  WF_PROP(remainder.reclaimed == expectedReclaimed,
          "the final sweep reclaimed " + std::to_string(remainder.reclaimed.size()) +
              " reservation(s) but the model left " + std::to_string(expectedReclaimed.size()) +
              " expired");
  WF_PROP(remainder.lapsed.empty(),
          "no lease may lapse once every reservation was released, got " +
              std::to_string(remainder.lapsed.size()));
  WF_PROP_EQ(remainder.expired, std::uint64_t(0), "the final sweep must not lapse anything");
  WF_PROP_EQ(remainder.scanned, model.size(), "the final sweep scans every reservation once");
  for (const ReservationId id : expectedReclaimed) model[id].state = ReservationState::Reclaimed;
  local.reclamations += remainder.reclaimed.size();
  local.sweeps += 1;

  for (const SpectrumDomainId domain : kDomains) {
    const std::optional<SpectrumUsage> usage = fixture.runtime.usage(domain, after);
    WF_PROP(usage.has_value(), "usage must exist for every domain");
    if (!usage.has_value()) continue;
    g_context += " domain=" + std::to_string(domain.raw());
    WF_PROP_EQ(usage->liveSlots, std::uint32_t(0), "live slots must return to zero");
    WF_PROP_EQ(usage->lapsedSlots, std::uint32_t(0), "lapsed slots must return to zero");
    WF_PROP_EQ(usage->activeSlots, std::uint32_t(0), "active slots must return to zero");
    WF_PROP_EQ(usage->reservedSlots, std::uint32_t(0), "reserved slots must return to zero");
    WF_PROP_EQ(usage->freeSlots, kPressureSlots, "every slot must be free again");
    WF_PROP_EQ(usage->freeRuns.size(), std::size_t(1),
               "the free pool must be one contiguous run again");
    if (usage->freeRuns.size() == 1) {
      WF_PROP(usage->freeRuns[0].first == 0 && usage->freeRuns[0].count == kPressureSlots,
              "the free run must cover the whole allocatable window, got " +
                  renderValue(usage->freeRuns));
    }
    const ReferenceUsage reference = referenceUsage(model, domain, after, kPressureSlots);
    WF_PROP_EQ(usage->releasedReservations, reference.releasedReservations,
               "released accounting must match the reference model");
    WF_PROP_EQ(usage->reclaimedReservations, reference.reclaimedReservations,
               "reclaimed accounting must match the reference model");
  }
  verifyState(fixture.runtime, model, after, kPressureSlots);

  // ---- exact accounting over the whole scenario -----------------------------
  const RuntimeStats stats = fixture.runtime.stats();
  WF_PROP_EQ(stats.allocationsCommitted, local.allocationsCommitted,
             "committed allocations must match the operations performed");
  WF_PROP_EQ(stats.allocationsRefused, local.allocationsRefused,
             "refused allocations must match the operations performed");
  WF_PROP_EQ(stats.releases, local.releases, "the release counter must match");
  WF_PROP_EQ(stats.renewals, local.renewals, "the renewal counter must match");
  WF_PROP_EQ(stats.activations, local.activations, "the activation counter must match");
  WF_PROP_EQ(stats.deactivations, local.deactivations, "the deactivation counter must match");
  WF_PROP_EQ(stats.expirationSweeps, local.sweeps, "the sweep counter must match");
  WF_PROP_EQ(stats.reclamations, local.reclamations, "the reclamation counter must match");
  if (counters != nullptr) *counters = local;
}

}  // namespace

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

WF_TEST(randomized_ownership_matches_the_reference_model) {
  Counters total;
  for (const std::uint64_t seed : {1ull, 42ull, 0xDEAD'BEEFull, 999983ull}) {
    Counters counters;
    runScenario(seed, 400, nullptr, &counters);
    g_context = "seed=" + std::to_string(seed) + " summary";
    WF_PROP(counters.allocationsCommitted >= 5,
            "a scenario must commit a meaningful number of allocations, got " +
                std::to_string(counters.allocationsCommitted));
    WF_PROP(counters.singleDomainRefused >= 1,
            "the single-domain stream must exercise refusals, got " +
                std::to_string(counters.singleDomainRefused));
    total.allocationsCommitted += counters.allocationsCommitted;
    total.allocationsRefused += counters.allocationsRefused;
    total.singleDomainCommitted += counters.singleDomainCommitted;
    total.singleDomainRefused += counters.singleDomainRefused;
    total.multiDomainCommitted += counters.multiDomainCommitted;
    total.releases += counters.releases;
    total.renewals += counters.renewals;
    total.activations += counters.activations;
    total.deactivations += counters.deactivations;
    total.reclamations += counters.reclamations;
    total.sweeps += counters.sweeps;
  }
  g_context = "aggregate over four seeds";
  WF_PROP(total.allocationsCommitted >= 40,
          "the scenario must commit many allocations, got " +
              std::to_string(total.allocationsCommitted));
  WF_PROP(total.allocationsRefused >= 1,
          "the scenario must exercise refusals, got " + std::to_string(total.allocationsRefused));
  WF_PROP(total.multiDomainCommitted >= 5,
          "multi-domain placement must be exercised, got " +
              std::to_string(total.multiDomainCommitted));
  WF_PROP(total.releases >= 1, "releases must be exercised");
  WF_PROP(total.renewals >= 1, "renewals must be exercised");
  WF_PROP(total.activations >= 1, "activations must be exercised");
  WF_PROP(total.deactivations >= 1, "deactivations must be exercised");
  WF_PROP(total.reclamations >= 1, "reclamation must be exercised");
  WF_PROP(total.sweeps >= 8, "expiry and reclamation sweeps must be exercised");
}

WF_TEST(allocation_is_deterministic_for_a_fixed_seed) {
  std::vector<std::string> firstTrace;
  std::vector<std::string> secondTrace;
  runScenario(12345, 300, &firstTrace, nullptr);
  runScenario(12345, 300, &secondTrace, nullptr);
  WF_PROP(!firstTrace.empty(), "the scenario must produce a decision trace");
  WF_PROP_EQ(firstTrace.size(), secondTrace.size(), "trace length");
  for (std::size_t index = 0; index < firstTrace.size() && index < secondTrace.size(); ++index) {
    if (firstTrace[index] != secondTrace[index]) {
      WF_PROP(false, "trace line " + std::to_string(index) + ": actual=" + firstTrace[index] +
                         " expected=" + secondTrace[index]);
      break;
    }
  }

  std::vector<std::string> otherTrace;
  runScenario(54321, 300, &otherTrace, nullptr);
  WF_PROP(!otherTrace.empty(), "the alternative scenario must produce a decision trace");
  WF_PROP(otherTrace != firstTrace, "two different seeds must not produce identical traces");
}

WF_TEST(repeating_a_request_against_an_unchanged_state_is_identical) {
  wf_test::Fixture fixture;
  setupRuntime(fixture.runtime, kPressureSlots);
  const Instant now = kBase;
  const std::vector<SpectrumDomainId> domains = {SpectrumDomainId(1), SpectrumDomainId(2)};
  const SpectrumRequest request =
      buildRequest(fixture, AllocationRequestId(1), domains, 3, now, ContiguityRequirement::Required,
                   ContinuityRequirement::Required);

  const DecisionExplanation firstExplanation = fixture.runtime.explain(request);
  const DecisionExplanation secondExplanation = fixture.runtime.explain(request);
  WF_CHECK(firstExplanation.summary() == secondExplanation.summary());
  WF_CHECK(firstExplanation.outcome == AllocationOutcome::Allocated);

  const CandidateSet firstSet = fixture.runtime.enumerateCandidates(request);
  const CandidateSet secondSet = fixture.runtime.enumerateCandidates(request);
  WF_CHECK(firstSet.status.ok());
  WF_CHECK(secondSet.status.ok());
  WF_CHECK_EQ(firstSet.candidates.size(), secondSet.candidates.size());
  WF_CHECK_EQ(firstSet.eligibleCount, secondSet.eligibleCount);
  for (std::size_t index = 0; index < firstSet.candidates.size(); ++index) {
    WF_CHECK(firstSet.candidates[index].describe() == secondSet.candidates[index].describe());
  }

  const AllocationDecision committed = fixture.runtime.allocate(request);
  WF_REQUIRE(committed.allocated());
  const std::size_t reservationCount = fixture.runtime.reservations().size();
  WF_CHECK_EQ(reservationCount, std::size_t(1));
  WF_CHECK_EQ(fixture.runtime.stats().allocationsCommitted, std::uint64_t(1));
  const std::optional<SpectrumUsage> before = fixture.runtime.usage(SpectrumDomainId(1), now);

  const AllocationDecision replayed = fixture.runtime.allocate(request);
  WF_CHECK(replayed.allocated());
  WF_CHECK(replayed.status.ok());
  WF_CHECK(replayed.outcome == AllocationOutcome::Allocated);
  WF_CHECK(replayed.reservation == committed.reservation);
  WF_CHECK(replayed.generation == committed.generation);
  WF_CHECK(replayed.explanation.selected.slots.first == committed.explanation.selected.slots.first);
  WF_CHECK(replayed.explanation.selected.slots.count == committed.explanation.selected.slots.count);
  WF_CHECK(replayed.explanation.reservation == committed.reservation);
  WF_CHECK_EQ(fixture.runtime.reservations().size(), reservationCount);
  WF_CHECK_EQ(fixture.runtime.stats().allocationsCommitted, std::uint64_t(1));
  const std::optional<SpectrumUsage> after = fixture.runtime.usage(SpectrumDomainId(1), now);
  WF_REQUIRE(before.has_value());
  WF_REQUIRE(after.has_value());
  WF_CHECK_EQ(before->liveSlots, after->liveSlots);
  WF_CHECK(before->freeRuns == after->freeRuns);

  // The same identity at the same generation with a different shape is not a
  // replay and must be refused without touching ownership.
  SpectrumRequest different = request;
  different.slots = 4;
  different.contiguity = ContiguityRequirement::Required;
  const AllocationDecision clash = fixture.runtime.allocate(different);
  WF_CHECK(!clash.allocated());
  WF_CHECK(clash.outcome == AllocationOutcome::RefusedDuplicate);
  WF_CHECK(clash.status.code == StatusCode::Duplicate);
  WF_CHECK_EQ(fixture.runtime.reservations().size(), reservationCount);
}

WF_TEST(continuity_required_gives_every_domain_the_same_absolute_range) {
  for (const std::uint64_t seed : {7ull, 101ull, 0xBEEFull}) {
    wf_test::Fixture fixture;
    setupRuntime(fixture.runtime, kSparseSlots);
    Rng rng(seed);
    Instant now = kBase;
    for (std::uint32_t trial = 0; trial < 60; ++trial) {
      const std::vector<SpectrumDomainId> domains = pickDomains(rng);
      const std::uint32_t slots = 1 + rng.below(3);
      g_context = "seed=" + std::to_string(seed) + " trial=" + std::to_string(trial) +
                  " domains=" + std::to_string(domains.size()) + " slots=" + std::to_string(slots);

      const ContinuityRequirement continuity =
          (trial % 2 == 0) ? ContinuityRequirement::Required : ContinuityRequirement::NotRequired;
      const SpectrumRequest request =
          buildRequest(fixture, AllocationRequestId(static_cast<std::uint64_t>(trial) + 1), domains,
                       slots, now, contiguityFor(slots), continuity);
      const AllocationDecision decision = fixture.runtime.allocate(request);
      // Ownership is wiped between trials, so the window is empty and a channel
      // of at most three slots in a 48-slot grid always fits at slot zero.
      WF_PROP(decision.allocated(), "an empty window must accept a small channel, got outcome=" +
                                        std::string(toToken(decision.outcome)) + " status=" +
                                        renderValue(decision.status));
      if (decision.allocated()) {
        const std::optional<SpectrumReservation> stored =
            fixture.runtime.reservation(decision.reservation);
        WF_PROP(stored.has_value(), "the committed reservation must be queryable");
        if (stored.has_value()) {
          WF_PROP_EQ(stored->continuityRequired, continuity == ContinuityRequirement::Required,
                     "the stored continuity requirement must mirror the request");
          WF_PROP_EQ(stored->perDomainSlots.size(), domains.size(),
                     "every spanned domain needs a resolved slot range");
          WF_PROP(!stored->frequency.empty(), "a committed reservation owns a non-empty range");
          const std::int64_t expectedLow =
              kAnchorMhz + static_cast<std::int64_t>(stored->slots.first) * kSlotWidthMhz;
          WF_PROP_EQ(stored->frequency.lowMhz, expectedLow,
                     "the committed frequency must follow the anchor and slot width");
          const ChannelGrid grid = *fixture.runtime.grid(ChannelGridId(1));
          for (std::size_t index = 0; index < stored->perDomainSlots.size(); ++index) {
            FrequencyRange mapped;
            g_context += " domainIndex=" + std::to_string(index);
            WF_PROP(frequencyOfSlotRange(grid, stored->perDomainSlots[index], mapped),
                    "every resolved per-domain slot range must be representable");
            WF_PROP(mapped == stored->frequency,
                    "continuity requires the identical absolute range on every domain: actual=[" +
                        std::to_string(mapped.lowMhz) + "," + std::to_string(mapped.highMhz) +
                        ") expected=[" + std::to_string(stored->frequency.lowMhz) + "," +
                        std::to_string(stored->frequency.highMhz) + ")");
          }
        }
      }
      // Return every slot to the pool so the next trial starts from a clean
      // window; the sweep lapses and reclaims everything in one call.
      now = now + Duration::seconds(6000);
      const ReclaimReport sweep = fixture.runtime.reclaimExpired(now);
      WF_PROP(sweep.status.ok(), "a reclamation sweep must report Ok");
    }
  }
}

WF_TEST(continuity_unspecified_on_a_multi_domain_request_is_refused) {
  wf_test::Fixture fixture;
  setupRuntime(fixture.runtime, kPressureSlots);
  const Instant now = kBase;
  const std::vector<SpectrumDomainId> domains = {SpectrumDomainId(1), SpectrumDomainId(2)};
  const SpectrumRequest request =
      buildRequest(fixture, AllocationRequestId(1), domains, 2, now, ContiguityRequirement::Required,
                   ContinuityRequirement::Unspecified);
  const AllocationDecision decision = fixture.runtime.allocate(request);
  WF_CHECK(!decision.allocated());
  WF_CHECK(decision.outcome == AllocationOutcome::RefusedInvalidRequest);
  WF_CHECK(decision.status.code == StatusCode::InvalidArgument);
  WF_CHECK(decision.reservation.none());
  WF_CHECK(fixture.runtime.reservations().empty());
  WF_CHECK_EQ(fixture.runtime.stats().allocationsRefused, std::uint64_t(1));
  const CandidateSet set = fixture.runtime.enumerateCandidates(request);
  WF_CHECK(!set.status.ok());
  WF_CHECK(!set.status.message.empty());
  WF_CHECK(set.candidates.empty());
  WF_CHECK(!set.complete);
}

WF_TEST(a_multi_domain_request_into_a_fully_occupied_window_is_refused) {
  wf_test::Fixture fixture;
  setupRuntime(fixture.runtime, kPressureSlots);
  const Instant now = kBase;
  const std::vector<SpectrumDomainId> occupied = {SpectrumDomainId(1), SpectrumDomainId(2)};

  // Fill both domains completely, one slot at a time. First fit packs the
  // occupied region into a prefix, so the i-th channel lands on slot i.
  std::uint64_t requestId = 1;
  for (std::uint32_t slot = 0; slot < kPressureSlots; ++slot) {
    for (const SpectrumDomainId domain : occupied) {
      g_context = "fill slot=" + std::to_string(slot) + " domain=" + std::to_string(domain.raw());
      const SpectrumRequest fill =
          buildRequest(fixture, AllocationRequestId(requestId++), {domain}, 1, now,
                       ContiguityRequirement::Unspecified, ContinuityRequirement::Unspecified);
      const AllocationDecision filled = fixture.runtime.allocate(fill);
      WF_PROP(filled.allocated(), "filling an empty slot must succeed: outcome=" +
                                      std::string(toToken(filled.outcome)) + " status=" +
                                      renderValue(filled.status));
      const std::optional<SpectrumReservation> stored =
          fixture.runtime.reservation(filled.reservation);
      WF_PROP(stored.has_value(), "the filling reservation must be queryable");
      if (stored.has_value()) {
        WF_PROP_EQ(stored->slots.first, slot, "first fit must pack the occupied region into a prefix");
      }
    }
  }
  WF_PROP_EQ(fixture.runtime.reservations().size(),
             static_cast<std::size_t>(kPressureSlots) * occupied.size(),
             "every filling channel must own exactly one reservation");

  // Both spanned domains are full, so a two-slot channel across them cannot be
  // placed. The diagnostic enumeration path must not invent an eligible
  // candidate on the strength of the anchor domain alone.
  g_context = "fully occupied window";
  const SpectrumRequest blocked =
      buildRequest(fixture, AllocationRequestId(1000), occupied, 2, now,
                   ContiguityRequirement::Required, ContinuityRequirement::Required);
  const CandidateSet set = fixture.runtime.enumerateCandidates(blocked);
  WF_PROP(set.status.ok(), "enumeration must succeed: " + renderValue(set.status));
  WF_PROP(firstEligibleCandidate(set) == nullptr,
          "no candidate may be eligible when both spanned domains are fully occupied, got " +
              (firstEligibleCandidate(set) == nullptr
                   ? std::string("-")
                   : firstEligibleCandidate(set)->describe()));
  WF_PROP_EQ(set.eligibleCount, std::size_t(0), "no candidate may be counted as eligible");

  const AllocationDecision decision = fixture.runtime.allocate(blocked);
  WF_PROP(!decision.allocated(), "a request that fits nowhere must be refused, got selected=" +
                                     decision.explanation.selected.describe());
  WF_PROP(isRefusal(decision.outcome),
          "a request that fits nowhere must carry a typed refusal, got " +
              std::string(toToken(decision.outcome)));
  WF_PROP_EQ(decision.status.code, StatusCode::Conflict,
             "a fully occupied window must be refused as a conflict");
  WF_PROP(decision.reservation.none(), "a refusal must not name a reservation");
  WF_PROP_EQ(fixture.runtime.reservations().size(),
             static_cast<std::size_t>(kPressureSlots) * occupied.size(),
             "a refusal must not create a reservation");
  const std::optional<SpectrumUsage> usage = fixture.runtime.usage(SpectrumDomainId(1), now);
  WF_REQUIRE(usage.has_value());
  WF_PROP_EQ(usage->liveSlots, kPressureSlots, "the window must still be fully owned");
  WF_PROP_EQ(usage->freeSlots, std::uint32_t(0), "no slot may be free");
  requireNoOverlap(fixture.runtime, now);
}

WF_TEST(contiguity_is_exact) {
  // A multi-slot request that does not state contiguity is refused outright.
  {
    wf_test::Fixture fixture;
    setupRuntime(fixture.runtime, kPressureSlots);
    const SpectrumRequest request = buildRequest(
        fixture, AllocationRequestId(1), {SpectrumDomainId(1)}, 2, kBase,
        ContiguityRequirement::Unspecified, ContinuityRequirement::Unspecified);
    const AllocationDecision decision = fixture.runtime.allocate(request);
    WF_CHECK(!decision.allocated());
    WF_CHECK(decision.outcome == AllocationOutcome::RefusedInvalidRequest);
    WF_CHECK(decision.status.code == StatusCode::InvalidArgument);
    WF_CHECK(fixture.runtime.reservations().empty());
  }

  // A domain that permits fragmentation plus a request that permits it is
  // refused with the dedicated contiguity outcome, never silently fragmented.
  {
    SpectrumRuntime runtime;
    const std::int64_t endMhz = kAnchorMhz + kSlotWidthMhz * kPressureSlots;
    WF_CHECK(runtime
                 .registerGrid(wf_test::flexGrid(ChannelGridId(1), GridGeneration(1), kAnchorMhz,
                                                 kSlotWidthMhz, kPressureSlots, 1,
                                                 kMaxChannelSlots))
                 .ok());
    const SpectrumDomain fragmenting = wf_test::makeDomain(
        SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
        ResourceClass::FiberSpan, false);
    WF_CHECK(runtime.registerDomain(fragmenting).ok());
    SpectrumCapability capability = wf_test::makeCapability(
        SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
        SpectrumSupport::Supported, 0, kPressureSlots, runtime.fence(), ControllerId(1),
        CapabilityGeneration(1), false);
    capability.minTunableMhz = kAnchorMhz;
    capability.maxTunableMhz = endMhz;
    WF_CHECK(runtime.publishCapability(capability).ok());

    const SpectrumRequest fragmentable = wf_test::makeRequest(
        AllocationRequestId(1), OwnerId(1), {SpectrumDomainId(1)}, {SpectrumDomainGeneration(1)},
        ChannelGridId(1), GridGeneration(1), 2, kBase, kLease, {}, {},
        ContiguityRequirement::NotRequired, ContinuityRequirement::Unspecified);
    SpectrumRequest authorized = fragmentable;
    authorized.eligibilityAuthority.generation = EligibilityAuthorityGeneration(1);
    authorized.eligibilityAuthority.fence = runtime.fence();
    authorized.reservationAuthority.generation = ReservationAuthorityGeneration(1);
    authorized.reservationAuthority.fence = runtime.fence();

    const AllocationDecision refused = runtime.allocate(authorized);
    WF_CHECK(!refused.allocated());
    WF_CHECK(refused.outcome == AllocationOutcome::RefusedContiguity);
    WF_CHECK(refused.status.code == StatusCode::Refused);
    WF_CHECK(runtime.reservations().empty());

    // The same domain accepts a single-slot request and a request that demands
    // contiguity, because neither asks the runtime to fragment.
    SpectrumRequest single = authorized;
    single.requestId = AllocationRequestId(2);
    single.slots = 1;
    single.contiguity = ContiguityRequirement::Unspecified;
    const AllocationDecision allocatedSingle = runtime.allocate(single);
    WF_CHECK(allocatedSingle.allocated());
    WF_REQUIRE(allocatedSingle.allocated());
    const std::optional<SpectrumReservation> stored =
        runtime.reservation(allocatedSingle.reservation);
    WF_REQUIRE(stored.has_value());
    WF_CHECK_EQ(stored->slots.count, std::uint32_t(1));
    WF_CHECK_EQ(stored->perDomainSlots.size(), std::size_t(1));
    WF_CHECK_EQ(stored->perDomainSlots[0].count, std::uint32_t(1));

    SpectrumRequest requiring = authorized;
    requiring.requestId = AllocationRequestId(3);
    requiring.contiguity = ContiguityRequirement::Required;
    const AllocationDecision allocatedMultiple = runtime.allocate(requiring);
    WF_CHECK(allocatedMultiple.allocated());
    WF_REQUIRE(allocatedMultiple.allocated());
    const std::optional<SpectrumReservation> multiple =
        runtime.reservation(allocatedMultiple.reservation);
    WF_REQUIRE(multiple.has_value());
    WF_CHECK_EQ(multiple->slots.count, std::uint32_t(2));
    WF_CHECK_EQ(multiple->perDomainSlots[0].count, std::uint32_t(2));
  }
}

WF_TEST(every_committed_reservation_occupies_one_contiguous_run) {
  for (const std::uint64_t seed : {11ull, 222ull}) {
    wf_test::Fixture fixture;
    setupRuntime(fixture.runtime, kSparseSlots);
    Rng rng(seed);
    const Instant now = kBase;
    Model model;
    std::uint64_t nextRequest = 1;
    for (std::uint32_t trial = 0; trial < 40; ++trial) {
      const std::vector<SpectrumDomainId> domains = pickDomains(rng);
      const std::uint32_t slots = 1 + rng.below(kMaxRequestedSlots);
      g_context = "seed=" + std::to_string(seed) + " trial=" + std::to_string(trial) +
                  " domains=" + std::to_string(domains.size()) + " slots=" + std::to_string(slots);
      std::uint32_t referenceStart = 0;
      const bool fits = referenceFirstFit(model, domains, slots, now, kSparseSlots, referenceStart);
      WF_PROP(fits, "at most three live channels in a 48-slot grid must always leave room");
      if (!fits) continue;
      const SpectrumRequest request =
          buildRequest(fixture, AllocationRequestId(nextRequest++), domains, slots, now,
                       contiguityFor(slots), continuityFor(domains.size()));
      const AllocationDecision decision = fixture.runtime.allocate(request);
      WF_PROP(decision.allocated(), "a placeable request must be committed: outcome=" +
                                        std::string(toToken(decision.outcome)) + " status=" +
                                        renderValue(decision.status));
      if (!decision.allocated()) continue;
      const std::optional<SpectrumReservation> stored =
          fixture.runtime.reservation(decision.reservation);
      WF_REQUIRE(stored.has_value());
      WF_PROP_EQ(stored->slots.count, slots, "the committed width must equal the requested width");
      WF_PROP_EQ(stored->perDomainSlots.size(), domains.size(),
                 "one resolved range per spanned domain");
      for (const SlotRange& range : stored->perDomainSlots) {
        WF_PROP_EQ(range.count, slots, "a per-domain range must be exactly the requested width");
        WF_PROP(range.first + range.count <= kSparseSlots,
                "a per-domain range must stay inside the grid: actual end=" +
                    std::to_string(range.first + range.count) + " expected at most " +
                    std::to_string(kSparseSlots));
      }
      ModelReservation entry;
      entry.id = decision.reservation;
      entry.generation = decision.generation;
      entry.domains = domains;
      entry.slots = SlotRange{referenceStart, slots};
      entry.state = ReservationState::Reserved;
      entry.expiresAt = now + kLease;
      model.emplace(entry.id, entry);
      requireNoOverlap(fixture.runtime, now);

      // Keep the number of live channels small so the next placement is always
      // reachable: every third trial the window is lapsed and reclaimed.
      if (trial % 3 == 2) {
        const Instant later = now + Duration::seconds(6000);
        const ReclaimReport sweep = fixture.runtime.reclaimExpired(later);
        WF_PROP(sweep.status.ok(), "a reclamation sweep must report Ok: " + renderValue(sweep.status));
        for (auto& item : model) {
          if (modelLiveState(item.second.state)) item.second.state = ReservationState::Reclaimed;
        }
      }
    }
    WF_CHECK(model.size() >= 8);
  }
}

WF_TEST_MAIN()
