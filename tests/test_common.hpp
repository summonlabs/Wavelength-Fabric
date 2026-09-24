#pragma once

// Minimal deterministic test harness plus the fixtures the Wavelength Fabric
// tests share. Tests assert; they never sleep, never poll for a condition with a
// timeout, and never treat an unobserved state as a pass. A test that hangs is
// a defect in the runtime, not something to kill.

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <wavelength_fabric/runtime.hpp>
#include <wavelength_fabric/text.hpp>

namespace wf_test {

struct Registration {
  const char* name;
  std::function<void()> body;
};

class Harness {
 public:
  static Harness& instance() {
    static Harness harness;
    return harness;
  }

  void add(const char* name, std::function<void()> body) {
    registrations_.push_back(Registration{name, std::move(body)});
  }

  void fail(const char* file, int line, const std::string& message) {
    ++failures_;
    std::cout << "FAIL " << current_ << " (" << file << ":" << line << "): " << message << "\n";
  }

  void noteCheck() { ++checks_; }

  [[nodiscard]] int run() {
    std::cout << "running " << registrations_.size() << " case(s)\n";
    for (const Registration& registration : registrations_) {
      current_ = registration.name;
      const int before = failures_;
      try {
        registration.body();
      } catch (const std::exception& error) {
        fail(__FILE__, __LINE__, std::string("unexpected exception: ") + error.what());
      } catch (...) {
        fail(__FILE__, __LINE__, "unexpected non-standard exception");
      }
      if (failures_ == before) std::cout << "ok   " << registration.name << "\n";
    }
    std::cout << checks_ << " check(s), " << failures_ << " failure(s)\n";
    return failures_ == 0 ? 0 : 1;
  }

 private:
  std::vector<Registration> registrations_;
  std::string current_;
  int failures_{0};
  int checks_{0};
};

struct Registrar {
  Registrar(const char* name, std::function<void()> body) {
    Harness::instance().add(name, std::move(body));
  }
};

}  // namespace wf_test

#define WF_TEST(name)                                                           \
  static void wf_case_##name();                                                 \
  static const ::wf_test::Registrar wf_registrar_##name(#name, wf_case_##name); \
  static void wf_case_##name()

#define WF_CHECK(expr)                                                             \
  do {                                                                             \
    ::wf_test::Harness::instance().noteCheck();                                    \
    if (!(expr)) {                                                                 \
      ::wf_test::Harness::instance().fail(__FILE__, __LINE__, "expected: " #expr); \
    }                                                                              \
  } while (false)

#define WF_CHECK_EQ(actual, expected)                                                  \
  do {                                                                                 \
    ::wf_test::Harness::instance().noteCheck();                                        \
    const auto& wf_actual = (actual);                                                  \
    const auto& wf_expected = (expected);                                              \
    if (!(wf_actual == wf_expected)) {                                                 \
      std::ostringstream wf_stream;                                                    \
      wf_stream << #actual " != " #expected << " (actual=" << wf_actual                 \
                << ", expected=" << wf_expected << ")";                                \
      ::wf_test::Harness::instance().fail(__FILE__, __LINE__, wf_stream.str());        \
    }                                                                                  \
  } while (false)

#define WF_REQUIRE(expr)                                                       \
  do {                                                                         \
    ::wf_test::Harness::instance().noteCheck();                                \
    if (!(expr)) {                                                             \
      ::wf_test::Harness::instance().fail(__FILE__, __LINE__, "required: " #expr); \
      return;                                                                  \
    }                                                                          \
  } while (false)

#define WF_TEST_MAIN() \
  int main() { return ::wf_test::Harness::instance().run(); }

namespace wf_test {

using namespace wavelength_fabric;

// ---------------------------------------------------------------------------
// Fixtures
//
// Frequencies are abstract integers in MHz. The defaults model a 96-slot
// 50 GHz fixed grid and a 384-slot 12.5 GHz flex grid. Nothing here asserts
// anything about real optical hardware.
// ---------------------------------------------------------------------------

inline ChannelGrid fixedGrid(ChannelGridId id = ChannelGridId(1),
                             GridGeneration generation = GridGeneration(1),
                             std::int64_t anchorMhz = 191'300'000,
                             std::int64_t slotWidthMhz = 50'000,
                             std::uint32_t slotCount = 96) {
  ChannelGrid grid;
  grid.id = id;
  grid.generation = generation;
  grid.kind = GridKind::Fixed;
  grid.anchorMhz = anchorMhz;
  grid.slotWidthMhz = slotWidthMhz;
  grid.slotCount = slotCount;
  grid.minSlotsPerChannel = 1;
  grid.maxSlotsPerChannel = 1;
  grid.label = "fixed-" + std::to_string(id.raw());
  return grid;
}

inline ChannelGrid flexGrid(ChannelGridId id = ChannelGridId(2),
                            GridGeneration generation = GridGeneration(1),
                            std::int64_t anchorMhz = 191'300'000,
                            std::int64_t slotWidthMhz = 12'500,
                            std::uint32_t slotCount = 384,
                            std::uint32_t minSlots = 1,
                            std::uint32_t maxSlots = 32) {
  ChannelGrid grid;
  grid.id = id;
  grid.generation = generation;
  grid.kind = GridKind::Flex;
  grid.anchorMhz = anchorMhz;
  grid.slotWidthMhz = slotWidthMhz;
  grid.slotCount = slotCount;
  grid.minSlotsPerChannel = minSlots;
  grid.maxSlotsPerChannel = maxSlots;
  grid.label = "flex-" + std::to_string(id.raw());
  return grid;
}

inline SpectrumDomain makeDomain(SpectrumDomainId id, ChannelGridId grid,
                                 SpectrumDomainGeneration generation = SpectrumDomainGeneration(1),
                                 GridGeneration gridGeneration = GridGeneration(1),
                                 ResourceClass klass = ResourceClass::FiberSpan,
                                 bool requiresContiguity = true) {
  SpectrumDomain domain;
  domain.id = id;
  domain.generation = generation;
  domain.klass = klass;
  domain.grid = grid;
  domain.gridGeneration = gridGeneration;
  domain.requiresContiguity = requiresContiguity;
  domain.label = "domain-" + std::to_string(id.raw());
  return domain;
}

inline SpectrumCapability makeCapability(SpectrumDomainId domain, ChannelGridId grid,
                                         SpectrumDomainGeneration domainGeneration,
                                         GridGeneration gridGeneration, SpectrumSupport support,
                                         std::uint32_t firstAllocatableSlot,
                                         std::uint32_t allocatableSlots,
                                         const ControllerFence& fence,
                                         ControllerId publisher = ControllerId(1),
                                         CapabilityGeneration generation = CapabilityGeneration(1),
                                         bool contiguityEnforced = true,
                                         std::uint64_t evidence = 0x5EED0001ull) {
  SpectrumCapability capability;
  capability.domain = domain;
  capability.domainGeneration = domainGeneration;
  capability.grid = grid;
  capability.gridGeneration = gridGeneration;
  capability.support = support;
  capability.firstAllocatableSlot = firstAllocatableSlot;
  capability.allocatableSlots = allocatableSlots;
  capability.minTunableMhz = 191'300'000;
  capability.maxTunableMhz = 196'100'000;
  capability.contiguityEnforced = contiguityEnforced;
  capability.conversionSupported = false;
  capability.presenceEvidence.present = true;
  capability.presenceEvidence.digest = evidence;
  capability.presenceEvidence.source = "fixture";
  capability.generation = generation;
  capability.publisher = publisher;
  capability.fence = fence;
  capability.publishedAt = Instant::fromSeconds(1800000000);
  capability.detail = "fixture capability";
  return capability;
}

inline SpectrumRequest makeRequest(AllocationRequestId requestId, OwnerId owner,
                                   const std::vector<SpectrumDomainId>& domains,
                                   const std::vector<SpectrumDomainGeneration>& domainGenerations,
                                   ChannelGridId grid, GridGeneration gridGeneration,
                                   std::uint32_t slots, Instant requestedAt,
                                   Duration leaseDuration, const EligibilityAuthority& eligibility,
                                   const ReservationAuthority& reservation,
                                   ContiguityRequirement contiguity = ContiguityRequirement::Unspecified,
                                   ContinuityRequirement continuity = ContinuityRequirement::Unspecified) {
  SpectrumRequest request;
  request.requestId = requestId;
  request.requestGeneration = AllocationRequestGeneration(1);
  request.owner = owner;
  request.ownerGeneration = OwnerGeneration(1);
  request.domains = domains;
  request.domainGenerations = domainGenerations;
  request.grid = grid;
  request.gridGeneration = gridGeneration;
  request.slots = slots;
  request.contiguity = contiguity;
  request.continuity = continuity;
  request.leaseDuration = leaseDuration;
  request.requestedAt = requestedAt;
  request.policyGeneration = PolicyGeneration(1);
  request.priorityGeneration = PriorityGeneration(1);
  request.eligibilityAuthority = eligibility;
  request.reservationAuthority = reservation;
  return request;
}

// A runtime whose four authority generations have been advanced to a known
// value, plus matching tokens built from the current fence.
struct Fixture {
  RuntimeConfig config;
  SpectrumRuntime runtime;
  std::uint64_t generation{1};

  explicit Fixture(RuntimeConfig cfg = RuntimeConfig{}) : config(std::move(cfg)), runtime(config) {}

  void advanceTo(std::uint64_t value) {
    AuthorityState next;
    next.eligibilityGeneration = EligibilityAuthorityGeneration(value);
    next.reservationGeneration = ReservationAuthorityGeneration(value);
    next.activationGeneration = ActivationAuthorityGeneration(value);
    next.releaseGeneration = ReleaseAuthorityGeneration(value);
    next.fence = runtime.fence();
    const Status status = runtime.advanceAuthority(next);
    (void)status;
    generation = value;
  }

  [[nodiscard]] EligibilityAuthority eligibility() const {
    EligibilityAuthority token;
    token.generation = EligibilityAuthorityGeneration(generation);
    token.fence = runtime.fence();
    return token;
  }

  [[nodiscard]] ReservationAuthority reservation() const {
    ReservationAuthority token;
    token.generation = ReservationAuthorityGeneration(generation);
    token.fence = runtime.fence();
    return token;
  }

  [[nodiscard]] ActivationAuthority activation() const {
    ActivationAuthority token;
    token.generation = ActivationAuthorityGeneration(generation);
    token.fence = runtime.fence();
    return token;
  }

  [[nodiscard]] ReleaseAuthority release() const {
    ReleaseAuthority token;
    token.generation = ReleaseAuthorityGeneration(generation);
    token.fence = runtime.fence();
    return token;
  }

  // Registers one domain on an already registered grid and publishes a SUPPORTED
  // capability covering the whole grid (or allocatable slots when non-zero).
  SpectrumDomainId addDomain(SpectrumDomainId id, ChannelGridId grid, std::uint32_t slotCount = 96,
                             SpectrumDomainGeneration domainGen = SpectrumDomainGeneration(1),
                             GridGeneration gridGen = GridGeneration(1),
                             std::uint32_t allocatable = 0) {
    const SpectrumDomain domain = makeDomain(id, grid, domainGen, gridGen);
    (void)runtime.registerDomain(domain);
    const SpectrumCapability capability =
        makeCapability(id, grid, domainGen, gridGen, SpectrumSupport::Supported, 0,
                       allocatable == 0 ? slotCount : allocatable, runtime.fence());
    (void)runtime.publishCapability(capability);
    return id;
  }
};

}  // namespace wf_test
