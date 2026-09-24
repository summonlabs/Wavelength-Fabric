#include "test_common.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// Spectrum capability publication.
//
// The rules below come from src/model.cpp (validateCapabilityShape) and
// src/runtime_capability.cpp (publishCapability): evidence is required exactly
// where it is claimed, an UNSUPPORTED or UNKNOWN claim carries no allocatable
// window, a supported window and tunable range must fit the registered grid, and
// capability generations only move forward.

using namespace wavelength_fabric;
using namespace wf_test;

namespace {

template <class Enum>
[[nodiscard]] std::string token(Enum value) {
  return std::string(toToken(value));
}

[[nodiscard]] bool rejectedAsInvalid(const SpectrumCapability& capability) {
  const Status status = validateCapabilityShape(capability);
  return status.code == StatusCode::InvalidArgument && !status.message.empty();
}

[[nodiscard]] bool shapeAccepted(const SpectrumCapability& capability) {
  return validateCapabilityShape(capability).ok();
}

// A runtime with grid 1 (96 fixed slots) and domain 1 registered, plus a
// capability builder that starts from a valid baseline.
struct Registry {
  SpectrumRuntime runtime;
  ControllerFence fence;

  Registry() {
    (void)runtime.registerGrid(fixedGrid());
    (void)runtime.registerDomain(makeDomain(SpectrumDomainId(1), ChannelGridId(1)));
    fence = runtime.fence();
  }

  [[nodiscard]] SpectrumCapability build(CapabilityGeneration generation,
                                         SpectrumSupport support = SpectrumSupport::Supported,
                                         std::uint32_t first = 0,
                                         std::uint32_t count = 96) const {
    return makeCapability(SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1),
                          GridGeneration(1), support, first, count, fence, ControllerId(1),
                          generation);
  }
};

}  // namespace

WF_TEST(validate_capability_accepts_documented_shapes) {
  const ControllerFence fence{ControllerEpoch(1), ControllerIncarnation(1)};

  SpectrumCapability supported = makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, 96, fence);
  WF_CHECK(shapeAccepted(supported));

  SpectrumCapability unsupported = makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Unsupported, 0, 0, fence);
  WF_CHECK(shapeAccepted(unsupported));

  SpectrumCapability unknown = makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Unknown, 0, 0, fence);
  WF_CHECK(shapeAccepted(unknown));

  // An unsupported or unknown claim declares no window, so its tunable fields
  // are not part of the shape.
  SpectrumCapability unsupportedNoTunable = unsupported;
  unsupportedNoTunable.minTunableMhz = 0;
  unsupportedNoTunable.maxTunableMhz = 0;
  WF_CHECK(shapeAccepted(unsupportedNoTunable));

  SpectrumCapability maxWindow = supported;
  maxWindow.allocatableSlots = kMaxGridSlots;
  WF_CHECK(shapeAccepted(maxWindow));

  SpectrumCapability maxDetail = supported;
  maxDetail.detail.assign(4096, 'd');
  WF_CHECK(shapeAccepted(maxDetail));

  SpectrumCapability conversion = supported;
  conversion.conversionSupported = true;
  conversion.conversionEvidence.present = true;
  conversion.conversionEvidence.digest = 0xC0FFEEull;
  conversion.conversionEvidence.source = "controller-report";
  WF_CHECK(shapeAccepted(conversion));

  // The window is checked against the registered grid at publication, not here.
  SpectrumCapability farWindow = supported;
  farWindow.firstAllocatableSlot = 1'000'000;
  farWindow.allocatableSlots = 1;
  WF_CHECK(shapeAccepted(farWindow));
}

WF_TEST(validate_capability_rejects_every_identity_field) {
  const ControllerFence fence{ControllerEpoch(1), ControllerIncarnation(1)};
  const SpectrumCapability baseline = makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, 96, fence);

  SpectrumCapability zeroDomain = baseline;
  zeroDomain.domain = SpectrumDomainId(0);
  WF_CHECK(rejectedAsInvalid(zeroDomain));

  SpectrumCapability zeroDomainGeneration = baseline;
  zeroDomainGeneration.domainGeneration = SpectrumDomainGeneration(0);
  WF_CHECK(rejectedAsInvalid(zeroDomainGeneration));

  SpectrumCapability zeroGrid = baseline;
  zeroGrid.grid = ChannelGridId(0);
  WF_CHECK(rejectedAsInvalid(zeroGrid));

  SpectrumCapability zeroGridGeneration = baseline;
  zeroGridGeneration.gridGeneration = GridGeneration(0);
  WF_CHECK(rejectedAsInvalid(zeroGridGeneration));

  SpectrumCapability zeroPublisher = baseline;
  zeroPublisher.publisher = ControllerId(0);
  WF_CHECK(rejectedAsInvalid(zeroPublisher));

  SpectrumCapability zeroEpoch = baseline;
  zeroEpoch.fence = ControllerFence{ControllerEpoch(0), ControllerIncarnation(1)};
  WF_CHECK(rejectedAsInvalid(zeroEpoch));

  SpectrumCapability zeroIncarnation = baseline;
  zeroIncarnation.fence = ControllerFence{ControllerEpoch(1), ControllerIncarnation(0)};
  WF_CHECK(rejectedAsInvalid(zeroIncarnation));

  SpectrumCapability zeroGeneration = baseline;
  zeroGeneration.generation = CapabilityGeneration(0);
  WF_CHECK(rejectedAsInvalid(zeroGeneration));

  SpectrumCapability zeroInstant = baseline;
  zeroInstant.publishedAt = Instant{};
  WF_CHECK(rejectedAsInvalid(zeroInstant));

  SpectrumCapability negativeInstant = baseline;
  negativeInstant.publishedAt = Instant::fromNanos(-1);
  WF_CHECK(rejectedAsInvalid(negativeInstant));

  SpectrumCapability longDetail = baseline;
  longDetail.detail.assign(4097, 'd');
  WF_CHECK(rejectedAsInvalid(longDetail));
}

WF_TEST(validate_capability_evidence_rules) {
  const ControllerFence fence{ControllerEpoch(1), ControllerIncarnation(1)};
  const SpectrumCapability baseline = makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, 96, fence);

  SpectrumCapability absent = baseline;
  absent.presenceEvidence.present = false;
  absent.presenceEvidence.digest = 0x5EEDull;
  WF_CHECK(rejectedAsInvalid(absent));

  SpectrumCapability zeroDigest = baseline;
  zeroDigest.presenceEvidence.present = true;
  zeroDigest.presenceEvidence.digest = 0;
  WF_CHECK(rejectedAsInvalid(zeroDigest));

  // usable() is exactly "present with a non-zero digest".
  SpectrumCapability noSource = baseline;
  noSource.presenceEvidence.present = true;
  noSource.presenceEvidence.digest = 7;
  noSource.presenceEvidence.source.clear();
  WF_CHECK(shapeAccepted(noSource));

  SpectrumCapability conversionAbsent = baseline;
  conversionAbsent.conversionSupported = true;
  WF_CHECK(!conversionAbsent.conversionEvidence.usable());
  WF_CHECK(rejectedAsInvalid(conversionAbsent));

  SpectrumCapability conversionZeroDigest = baseline;
  conversionZeroDigest.conversionSupported = true;
  conversionZeroDigest.conversionEvidence.present = true;
  conversionZeroDigest.conversionEvidence.digest = 0;
  WF_CHECK(rejectedAsInvalid(conversionZeroDigest));

  SpectrumCapability conversionClaimed = baseline;
  conversionClaimed.conversionSupported = true;
  conversionClaimed.conversionEvidence.present = true;
  conversionClaimed.conversionEvidence.digest = 99;
  WF_CHECK(shapeAccepted(conversionClaimed));

  // Absent conversion evidence is fine while conversion is not claimed.
  SpectrumCapability noConversion = baseline;
  noConversion.conversionSupported = false;
  noConversion.conversionEvidence = CapabilityEvidence{};
  WF_CHECK(shapeAccepted(noConversion));
}

WF_TEST(validate_capability_window_and_tunable_rules) {
  const ControllerFence fence{ControllerEpoch(1), ControllerIncarnation(1)};
  const SpectrumCapability baseline = makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, 96, fence);

  SpectrumCapability noSlots = baseline;
  noSlots.allocatableSlots = 0;
  WF_CHECK(rejectedAsInvalid(noSlots));

  SpectrumCapability tooManySlots = baseline;
  tooManySlots.allocatableSlots = kMaxGridSlots + 1;
  WF_CHECK(rejectedAsInvalid(tooManySlots));

  SpectrumCapability tunableZero = baseline;
  tunableZero.minTunableMhz = 0;
  WF_CHECK(rejectedAsInvalid(tunableZero));

  SpectrumCapability tunableNegative = baseline;
  tunableNegative.minTunableMhz = -1;
  WF_CHECK(rejectedAsInvalid(tunableNegative));

  SpectrumCapability tunableEmpty = baseline;
  tunableEmpty.minTunableMhz = 191'400'000;
  tunableEmpty.maxTunableMhz = 191'400'000;
  WF_CHECK(rejectedAsInvalid(tunableEmpty));

  SpectrumCapability tunableInverted = baseline;
  tunableInverted.minTunableMhz = 191'400'000;
  tunableInverted.maxTunableMhz = 191'350'000;
  WF_CHECK(rejectedAsInvalid(tunableInverted));

  SpectrumCapability tunableTooHigh = baseline;
  tunableTooHigh.maxTunableMhz = kMaxFrequencyMhz + 1;
  WF_CHECK(rejectedAsInvalid(tunableTooHigh));

  SpectrumCapability unsupportedWindow = makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Unsupported, 0, 1, fence);
  WF_CHECK(rejectedAsInvalid(unsupportedWindow));

  SpectrumCapability unsupportedFirst = makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Unsupported, 4, 0, fence);
  WF_CHECK(rejectedAsInvalid(unsupportedFirst));

  SpectrumCapability unknownWindow = makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Unknown, 0, 3, fence);
  WF_CHECK(rejectedAsInvalid(unknownWindow));
}

WF_TEST(allocatable_window_contract) {
  const ControllerFence fence{ControllerEpoch(1), ControllerIncarnation(1)};
  SpectrumCapability capability = makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 90, 6, fence);
  const SlotRange window = allocatableWindow(capability);
  WF_CHECK_EQ(window.first, std::uint32_t{90});
  WF_CHECK_EQ(window.count, std::uint32_t{6});
  WF_CHECK_EQ(window.end(), std::uint32_t{96});

  SpectrumCapability unsupported = makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Unsupported, 0, 0, fence);
  const SlotRange emptyWindow = allocatableWindow(unsupported);
  WF_CHECK(emptyWindow.empty());
  WF_CHECK_EQ(emptyWindow.first, std::uint32_t{0});
  WF_CHECK_EQ(emptyWindow.count, std::uint32_t{0});
}

WF_TEST(publish_capability_requires_the_current_fence) {
  Registry registry;

  SpectrumCapability staleEpoch = registry.build(CapabilityGeneration(1));
  staleEpoch.fence.epoch.bump();
  const Status epochRefused = registry.runtime.publishCapability(staleEpoch);
  WF_CHECK(epochRefused.code == StatusCode::StaleEpoch);
  WF_CHECK(!registry.runtime.capability(SpectrumDomainId(1)).has_value());

  SpectrumCapability staleIncarnation = registry.build(CapabilityGeneration(1));
  staleIncarnation.fence.incarnation.bump();
  WF_CHECK(registry.runtime.publishCapability(staleIncarnation).code == StatusCode::StaleIncarnation);
  WF_CHECK(!registry.runtime.capability(SpectrumDomainId(1)).has_value());

  SpectrumCapability invalidFence = registry.build(CapabilityGeneration(1));
  invalidFence.fence.incarnation = ControllerIncarnation(0);
  WF_CHECK(registry.runtime.publishCapability(invalidFence).code == StatusCode::InvalidArgument);

  SpectrumCapability good = registry.build(CapabilityGeneration(1));
  WF_CHECK(registry.runtime.publishCapability(good).ok());

  // A stale fence is rejected before the shape is examined: the missing window
  // would be an InvalidArgument if the fence were current.
  SpectrumCapability shapeless = registry.build(CapabilityGeneration(2), SpectrumSupport::Supported, 0, 0);
  WF_CHECK(validateCapabilityShape(shapeless).code == StatusCode::InvalidArgument);
  shapeless.fence.incarnation.bump();
  WF_CHECK(registry.runtime.publishCapability(shapeless).code == StatusCode::StaleIncarnation);

  const std::optional<SpectrumCapability> stored = registry.runtime.capability(SpectrumDomainId(1));
  WF_REQUIRE(stored.has_value());
  WF_CHECK_EQ(stored->generation.raw(), std::uint64_t{1});
}

WF_TEST(publish_capability_requires_matching_registrations) {
  Registry registry;

  SpectrumCapability unknownDomain = registry.build(CapabilityGeneration(1));
  unknownDomain.domain = SpectrumDomainId(9);
  WF_CHECK(registry.runtime.publishCapability(unknownDomain).code == StatusCode::NotFound);

  SpectrumCapability staleDomainGeneration = registry.build(CapabilityGeneration(1));
  staleDomainGeneration.domainGeneration = SpectrumDomainGeneration(2);
  WF_CHECK(registry.runtime.publishCapability(staleDomainGeneration).code == StatusCode::StaleGeneration);

  (void)registry.runtime.registerGrid(fixedGrid(ChannelGridId(2)));
  SpectrumCapability foreignGrid = registry.build(CapabilityGeneration(1));
  foreignGrid.grid = ChannelGridId(2);
  WF_CHECK(registry.runtime.publishCapability(foreignGrid).code == StatusCode::InvalidArgument);

  SpectrumCapability staleGridGeneration = registry.build(CapabilityGeneration(1));
  staleGridGeneration.gridGeneration = GridGeneration(2);
  WF_CHECK(registry.runtime.publishCapability(staleGridGeneration).code == StatusCode::InvalidArgument);

  WF_CHECK(!registry.runtime.capability(SpectrumDomainId(1)).has_value());

  // A grid re-registered at a newer generation makes a capability that names the
  // older grid generation stale even though the domain still names it.
  SpectrumRuntime other;
  (void)other.registerGrid(fixedGrid());
  (void)other.registerDomain(makeDomain(SpectrumDomainId(1), ChannelGridId(1)));
  ChannelGrid newer = fixedGrid();
  newer.generation = GridGeneration(2);
  WF_CHECK(other.registerGrid(newer).ok());
  const SpectrumCapability capability = makeCapability(
      SpectrumDomainId(1), ChannelGridId(1), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, 96, other.fence());
  WF_CHECK(other.publishCapability(capability).code == StatusCode::StaleGeneration);
}

WF_TEST(publish_capability_window_must_fit_the_registered_grid) {
  Registry registry;

  WF_CHECK(registry.runtime.publishCapability(registry.build(CapabilityGeneration(1), SpectrumSupport::Supported, 90, 6)).ok());
  WF_CHECK(registry.runtime.publishCapability(registry.build(CapabilityGeneration(2), SpectrumSupport::Supported, 91, 6)).code == StatusCode::InvalidArgument);
  WF_CHECK(registry.runtime.publishCapability(registry.build(CapabilityGeneration(3), SpectrumSupport::Supported, 96, 1)).code == StatusCode::InvalidArgument);
  WF_CHECK(registry.runtime.publishCapability(registry.build(CapabilityGeneration(4), SpectrumSupport::Supported, 0, 97)).code == StatusCode::InvalidArgument);
  WF_CHECK(registry.runtime.publishCapability(registry.build(CapabilityGeneration(5), SpectrumSupport::Supported, 1'000'000, 1)).code == StatusCode::InvalidArgument);

  const std::optional<SpectrumCapability> stored = registry.runtime.capability(SpectrumDomainId(1));
  WF_REQUIRE(stored.has_value());
  WF_CHECK_EQ(stored->generation.raw(), std::uint64_t{1});
  WF_CHECK_EQ(stored->allocatableSlots, std::uint32_t{6});

  SpectrumCapability belowAnchor = registry.build(CapabilityGeneration(6));
  belowAnchor.minTunableMhz = 191'299'999;
  WF_CHECK(registry.runtime.publishCapability(belowAnchor).code == StatusCode::InvalidArgument);

  SpectrumCapability aboveEnd = registry.build(CapabilityGeneration(7));
  aboveEnd.maxTunableMhz = 196'100'001;
  WF_CHECK(registry.runtime.publishCapability(aboveEnd).code == StatusCode::InvalidArgument);

  SpectrumCapability exactSpan = registry.build(CapabilityGeneration(8));
  exactSpan.minTunableMhz = 191'300'000;
  exactSpan.maxTunableMhz = 196'100'000;
  WF_CHECK(registry.runtime.publishCapability(exactSpan).ok());

  // A grid only has to express its own narrowest channel: one slot for a fixed
  // grid, minSlotsPerChannel for a flex grid. A [2, 8] grid therefore carries a
  // supported capability.
  SpectrumRuntime narrow;
  (void)narrow.registerGrid(flexGrid(ChannelGridId(2), GridGeneration(1), 191'300'000, 12'500, 384, 2, 8));
  (void)narrow.registerDomain(makeDomain(SpectrumDomainId(1), ChannelGridId(2)));
  SpectrumCapability capability = makeCapability(
      SpectrumDomainId(1), ChannelGridId(2), SpectrumDomainGeneration(1), GridGeneration(1),
      SpectrumSupport::Supported, 0, 16, narrow.fence());
  capability.minTunableMhz = 191'300'000;
  capability.maxTunableMhz = 192'100'000;
  WF_CHECK(narrow.publishCapability(capability).ok());
  const std::optional<SpectrumCapability> narrowStored = narrow.capability(SpectrumDomainId(1));
  WF_REQUIRE(narrowStored.has_value());
  WF_CHECK(narrowStored->support == SpectrumSupport::Supported);
  WF_CHECK_EQ(narrowStored->domain.raw(), std::uint64_t{1});
  WF_CHECK_EQ(narrowStored->grid.raw(), std::uint64_t{2});
  WF_CHECK_EQ(narrowStored->generation.raw(), std::uint64_t{1});
  WF_CHECK_EQ(narrowStored->minTunableMhz, std::int64_t{191'300'000});
  WF_CHECK_EQ(narrowStored->maxTunableMhz, std::int64_t{192'100'000});
  const SlotRange narrowWindow = allocatableWindow(*narrowStored);
  WF_CHECK_EQ(narrowWindow.first, std::uint32_t{0});
  WF_CHECK_EQ(narrowWindow.count, std::uint32_t{16});
  WF_CHECK(narrowStored->presenceEvidence.usable());
}

WF_TEST(publish_capability_generations_are_monotonic) {
  Registry registry;

  const SpectrumCapability first = registry.build(CapabilityGeneration(1));
  WF_CHECK(registry.runtime.publishCapability(first).ok());

  WF_CHECK(registry.runtime.publishCapability(registry.build(CapabilityGeneration(1))).code ==
           StatusCode::Duplicate);

  SpectrumCapability zeroGeneration = registry.build(CapabilityGeneration(0));
  WF_CHECK(registry.runtime.publishCapability(zeroGeneration).code == StatusCode::InvalidArgument);

  SpectrumCapability second = registry.build(CapabilityGeneration(2), SpectrumSupport::Unsupported, 0, 0);
  WF_CHECK(registry.runtime.publishCapability(second).ok());

  WF_CHECK(registry.runtime.publishCapability(registry.build(CapabilityGeneration(2))).code ==
           StatusCode::Duplicate);
  WF_CHECK(registry.runtime.publishCapability(registry.build(CapabilityGeneration(1))).code ==
           StatusCode::StaleGeneration);

  // A stale generation that is also shape-invalid is refused by the shape check
  // first, so the caller is never told "stale" about a capability that could
  // never have been published.
  SpectrumCapability staleShapeless = registry.build(CapabilityGeneration(1), SpectrumSupport::Supported, 0, 0);
  WF_CHECK(registry.runtime.publishCapability(staleShapeless).code == StatusCode::InvalidArgument);

  const std::optional<SpectrumCapability> stored = registry.runtime.capability(SpectrumDomainId(1));
  WF_REQUIRE(stored.has_value());
  WF_CHECK_EQ(stored->generation.raw(), std::uint64_t{2});
  WF_CHECK(stored->support == SpectrumSupport::Unsupported);
}

WF_TEST(published_capability_round_trips) {
  Registry registry;

  SpectrumCapability capability = registry.build(CapabilityGeneration(1));
  capability.publisher = ControllerId(7);
  capability.contiguityEnforced = false;
  capability.conversionSupported = true;
  capability.conversionEvidence.present = true;
  capability.conversionEvidence.digest = 0xABCDEFull;
  capability.conversionEvidence.source = "evidence";
  capability.detail = "round trip";
  capability.publishedAt = Instant::fromSeconds(1'800'000'000);
  WF_CHECK(registry.runtime.publishCapability(capability).ok());

  const std::optional<SpectrumCapability> stored = registry.runtime.capability(SpectrumDomainId(1));
  WF_REQUIRE(stored.has_value());
  WF_CHECK_EQ(stored->domain.raw(), std::uint64_t{1});
  WF_CHECK_EQ(stored->domainGeneration.raw(), std::uint64_t{1});
  WF_CHECK_EQ(stored->grid.raw(), std::uint64_t{1});
  WF_CHECK_EQ(stored->gridGeneration.raw(), std::uint64_t{1});
  WF_CHECK(stored->support == SpectrumSupport::Supported);
  WF_CHECK_EQ(stored->generation.raw(), std::uint64_t{1});
  WF_CHECK_EQ(stored->publisher.raw(), std::uint64_t{7});
  WF_CHECK(stored->fence == registry.fence);
  WF_CHECK_EQ(stored->publishedAt.nanos(), std::int64_t{1'800'000'000'000'000'000});
  WF_CHECK(!stored->contiguityEnforced);
  WF_CHECK(stored->conversionSupported);
  WF_CHECK(stored->conversionEvidence.usable());
  WF_CHECK_EQ(stored->conversionEvidence.digest, std::uint64_t{0xABCDEF});
  WF_CHECK_EQ(stored->detail, std::string("round trip"));
  const SlotRange expectedWindow{0, 96};
  WF_CHECK(allocatableWindow(*stored) == expectedWindow);

  WF_CHECK(!registry.runtime.capability(SpectrumDomainId(2)).has_value());
  WF_CHECK(stored->presenceEvidence.usable());
}

WF_TEST_MAIN()
