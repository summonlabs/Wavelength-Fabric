#include "test_common.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Structural validation of an allocation request.
//
// Every rule below is taken from validateRequestShape in src/model.cpp: identity
// validity, bounded collection sizes, contiguous frequency and slot ranges, the
// explicit-requirement rules for multi-resource and multi-slot requests, the
// guard band and lease bounds, and the two required authority tokens. An
// unstated requirement is never guessed.

using namespace wavelength_fabric;
using namespace wf_test;

namespace {

template <class Enum>
[[nodiscard]] std::string token(Enum value) {
  return std::string(toToken(value));
}

[[nodiscard]] ControllerFence validFence() {
  return ControllerFence{ControllerEpoch(1), ControllerIncarnation(1)};
}

[[nodiscard]] EligibilityAuthority eligibilityToken() {
  EligibilityAuthority token;
  token.generation = EligibilityAuthorityGeneration(1);
  token.fence = validFence();
  return token;
}

[[nodiscard]] ReservationAuthority reservationToken() {
  ReservationAuthority token;
  token.generation = ReservationAuthorityGeneration(1);
  token.fence = validFence();
  return token;
}

[[nodiscard]] Instant fixedInstant() { return Instant::fromSeconds(1'800'000'000); }

[[nodiscard]] SpectrumRequest validRequest() {
  return makeRequest(AllocationRequestId(1), OwnerId(1), {SpectrumDomainId(1)},
                     {SpectrumDomainGeneration(1)}, ChannelGridId(1), GridGeneration(1), 1,
                     fixedInstant(), Duration::hours(1), eligibilityToken(), reservationToken());
}

[[nodiscard]] SpectrumRequest requestWithDomains(std::size_t count) {
  SpectrumRequest request = validRequest();
  request.domains.clear();
  request.domainGenerations.clear();
  for (std::size_t index = 1; index <= count; ++index) {
    request.domains.push_back(SpectrumDomainId(index));
    request.domainGenerations.push_back(SpectrumDomainGeneration(1));
  }
  if (count > 1) request.continuity = ContinuityRequirement::Required;
  return request;
}

[[nodiscard]] bool rejected(const SpectrumRequest& request) {
  const Status status = validateRequestShape(request);
  return status.code == StatusCode::InvalidArgument && !status.message.empty();
}

[[nodiscard]] bool accepted(const SpectrumRequest& request) {
  return validateRequestShape(request).ok();
}

}  // namespace

WF_TEST(validate_request_shape_accepts_the_baseline) {
  const SpectrumRequest baseline = validRequest();
  WF_CHECK(accepted(baseline));
  WF_CHECK_EQ(baseline.domains.size(), std::size_t{1});
  WF_CHECK_EQ(baseline.slots, std::uint32_t{1});

  SpectrumRequest withNotBefore = baseline;
  withNotBefore.notBefore = fixedInstant() + Duration::minutes(30);
  WF_CHECK(accepted(withNotBefore));

  SpectrumRequest zeroNotBefore = baseline;
  zeroNotBefore.notBefore = Instant{};
  WF_CHECK(accepted(zeroNotBefore));

  SpectrumRequest explicitContiguity = baseline;
  explicitContiguity.contiguity = ContiguityRequirement::Required;
  WF_CHECK(accepted(explicitContiguity));
}

WF_TEST(validate_request_shape_rejects_identity_fields) {
  SpectrumRequest zeroRequestId = validRequest();
  zeroRequestId.requestId = AllocationRequestId(0);
  WF_CHECK(rejected(zeroRequestId));

  SpectrumRequest zeroRequestGeneration = validRequest();
  zeroRequestGeneration.requestGeneration = AllocationRequestGeneration(0);
  WF_CHECK(rejected(zeroRequestGeneration));

  SpectrumRequest zeroOwner = validRequest();
  zeroOwner.owner = OwnerId(0);
  WF_CHECK(rejected(zeroOwner));

  SpectrumRequest zeroOwnerGeneration = validRequest();
  zeroOwnerGeneration.ownerGeneration = OwnerGeneration(0);
  WF_CHECK(rejected(zeroOwnerGeneration));
}

WF_TEST(validate_request_shape_bounds_and_orders_domains) {
  SpectrumRequest noDomains = validRequest();
  noDomains.domains.clear();
  noDomains.domainGenerations.clear();
  WF_CHECK(rejected(noDomains));

  SpectrumRequest duplicateDomains = validRequest();
  duplicateDomains.domains = {SpectrumDomainId(2), SpectrumDomainId(2)};
  duplicateDomains.domainGenerations = {SpectrumDomainGeneration(1), SpectrumDomainGeneration(1)};
  duplicateDomains.continuity = ContinuityRequirement::Required;
  WF_CHECK(rejected(duplicateDomains));

  SpectrumRequest unsortedDomains = validRequest();
  unsortedDomains.domains = {SpectrumDomainId(2), SpectrumDomainId(1)};
  unsortedDomains.domainGenerations = {SpectrumDomainGeneration(1), SpectrumDomainGeneration(1)};
  unsortedDomains.continuity = ContinuityRequirement::Required;
  WF_CHECK(rejected(unsortedDomains));

  SpectrumRequest zeroDomain = validRequest();
  zeroDomain.domains = {SpectrumDomainId(0)};
  WF_CHECK(rejected(zeroDomain));

  SpectrumRequest zeroDomainGeneration = validRequest();
  zeroDomainGeneration.domainGenerations = {SpectrumDomainGeneration(0)};
  WF_CHECK(rejected(zeroDomainGeneration));

  SpectrumRequest tooFewGenerations = validRequest();
  tooFewGenerations.domains = {SpectrumDomainId(1), SpectrumDomainId(2)};
  tooFewGenerations.domainGenerations = {SpectrumDomainGeneration(1)};
  tooFewGenerations.continuity = ContinuityRequirement::Required;
  WF_CHECK(rejected(tooFewGenerations));

  SpectrumRequest tooManyGenerations = validRequest();
  tooManyGenerations.domainGenerations = {SpectrumDomainGeneration(1), SpectrumDomainGeneration(1)};
  WF_CHECK(rejected(tooManyGenerations));

  WF_CHECK_EQ(kMaxRequestDomains, std::uint32_t{32});
  WF_CHECK(accepted(requestWithDomains(kMaxRequestDomains)));
  WF_CHECK(rejected(requestWithDomains(static_cast<std::size_t>(kMaxRequestDomains) + 1)));

  // Every domain in a full request must still carry a valid generation.
  SpectrumRequest worst = requestWithDomains(kMaxRequestDomains);
  worst.domainGenerations.back() = SpectrumDomainGeneration(0);
  WF_CHECK(rejected(worst));
}

WF_TEST(validate_request_shape_grid_identity_and_channel_width) {
  SpectrumRequest zeroGrid = validRequest();
  zeroGrid.grid = ChannelGridId(0);
  WF_CHECK(rejected(zeroGrid));

  SpectrumRequest zeroGridGeneration = validRequest();
  zeroGridGeneration.gridGeneration = GridGeneration(0);
  WF_CHECK(rejected(zeroGridGeneration));

  SpectrumRequest zeroSlots = validRequest();
  zeroSlots.slots = 0;
  WF_CHECK(rejected(zeroSlots));

  SpectrumRequest tooWide = validRequest();
  tooWide.slots = kMaxSlotsPerChannel + 1;
  tooWide.contiguity = ContiguityRequirement::Required;
  WF_CHECK(rejected(tooWide));

  SpectrumRequest widestAccepted = validRequest();
  widestAccepted.slots = kMaxSlotsPerChannel;
  widestAccepted.contiguity = ContiguityRequirement::Required;
  WF_CHECK(accepted(widestAccepted));
}

WF_TEST(validate_request_shape_requires_explicit_requirements) {
  SpectrumRequest twoDomainsUnstated = requestWithDomains(2);
  twoDomainsUnstated.continuity = ContinuityRequirement::Unspecified;
  WF_CHECK(rejected(twoDomainsUnstated));

  SpectrumRequest twoDomainsRequired = requestWithDomains(2);
  twoDomainsRequired.continuity = ContinuityRequirement::Required;
  WF_CHECK(accepted(twoDomainsRequired));

  SpectrumRequest twoDomainsNotRequired = requestWithDomains(2);
  twoDomainsNotRequired.continuity = ContinuityRequirement::NotRequired;
  WF_CHECK(accepted(twoDomainsNotRequired));

  SpectrumRequest multiSlotUnstated = validRequest();
  multiSlotUnstated.slots = 2;
  WF_CHECK(rejected(multiSlotUnstated));

  SpectrumRequest multiSlotRequired = validRequest();
  multiSlotRequired.slots = 2;
  multiSlotRequired.contiguity = ContiguityRequirement::Required;
  WF_CHECK(accepted(multiSlotRequired));

  SpectrumRequest multiSlotNotRequired = validRequest();
  multiSlotNotRequired.slots = 2;
  multiSlotNotRequired.contiguity = ContiguityRequirement::NotRequired;
  WF_CHECK(accepted(multiSlotNotRequired));

  // A single-resource, single-slot request states nothing and is accepted: the
  // explicit-requirement rules only apply when they are meaningful.
  SpectrumRequest singleUnstated = validRequest();
  singleUnstated.contiguity = ContiguityRequirement::Unspecified;
  singleUnstated.continuity = ContinuityRequirement::Unspecified;
  WF_CHECK(accepted(singleUnstated));

  SpectrumRequest bothUnstated = requestWithDomains(3);
  bothUnstated.continuity = ContinuityRequirement::Unspecified;
  bothUnstated.slots = 4;
  bothUnstated.contiguity = ContiguityRequirement::Unspecified;
  WF_CHECK(rejected(bothUnstated));

  SpectrumRequest bothStated = requestWithDomains(3);
  bothStated.continuity = ContinuityRequirement::Required;
  bothStated.slots = 4;
  bothStated.contiguity = ContiguityRequirement::Required;
  WF_CHECK(accepted(bothStated));
}

WF_TEST(validate_request_shape_bounds_frequency_windows) {
  SpectrumRequest sixtyFour = validRequest();
  for (std::uint32_t index = 0; index < kMaxFrequencyWindows; ++index) {
    sixtyFour.frequencyWindows.push_back(FrequencyRange{static_cast<std::int64_t>(index) + 1,
                                                        static_cast<std::int64_t>(index) + 2});
  }
  WF_CHECK_EQ(sixtyFour.frequencyWindows.size(), std::size_t{64});
  WF_CHECK(accepted(sixtyFour));

  SpectrumRequest sixtyFive = sixtyFour;
  sixtyFive.frequencyWindows.push_back(FrequencyRange{100, 200});
  WF_CHECK(rejected(sixtyFive));

  SpectrumRequest emptyWindow = validRequest();
  emptyWindow.frequencyWindows.push_back(FrequencyRange{191'300'000, 191'300'000});
  WF_CHECK(rejected(emptyWindow));

  SpectrumRequest invertedWindow = validRequest();
  invertedWindow.frequencyWindows.push_back(FrequencyRange{191'400'000, 191'300'000});
  WF_CHECK(rejected(invertedWindow));

  SpectrumRequest belowModel = validRequest();
  belowModel.frequencyWindows.push_back(FrequencyRange{0, 100});
  WF_CHECK(rejected(belowModel));

  SpectrumRequest aboveModel = validRequest();
  aboveModel.frequencyWindows.push_back(FrequencyRange{100, kMaxFrequencyMhz + 1});
  WF_CHECK(rejected(aboveModel));

  SpectrumRequest wholeModel = validRequest();
  wholeModel.frequencyWindows.push_back(FrequencyRange{kMinFrequencyMhz, kMaxFrequencyMhz});
  WF_CHECK(accepted(wholeModel));
}

WF_TEST(validate_request_shape_bounds_constraints) {
  SpectrumRequest maxSlots = validRequest();
  for (std::uint32_t index = 0; index < kMaxExcludedRanges; ++index) {
    maxSlots.constraints.excludedSlots.push_back(SlotRange{index, 1});
  }
  WF_CHECK_EQ(maxSlots.constraints.excludedSlots.size(), std::size_t{64});
  WF_CHECK(accepted(maxSlots));

  SpectrumRequest tooManySlots = maxSlots;
  tooManySlots.constraints.excludedSlots.push_back(SlotRange{200, 1});
  WF_CHECK(rejected(tooManySlots));

  SpectrumRequest emptyExcludedSlots = validRequest();
  emptyExcludedSlots.constraints.excludedSlots.push_back(SlotRange{4, 0});
  WF_CHECK(rejected(emptyExcludedSlots));

  SpectrumRequest hugeExcludedSlots = validRequest();
  hugeExcludedSlots.constraints.excludedSlots.push_back(SlotRange{0xFFFF'FFFFu, 1});
  WF_CHECK(accepted(hugeExcludedSlots));

  SpectrumRequest maxFrequencies = validRequest();
  for (std::uint32_t index = 0; index < kMaxExcludedRanges; ++index) {
    maxFrequencies.constraints.excludedFrequencies.push_back(
        FrequencyRange{static_cast<std::int64_t>(index) + 1, static_cast<std::int64_t>(index) + 2});
  }
  WF_CHECK(accepted(maxFrequencies));

  SpectrumRequest tooManyFrequencies = maxFrequencies;
  tooManyFrequencies.constraints.excludedFrequencies.push_back(FrequencyRange{500, 600});
  WF_CHECK(rejected(tooManyFrequencies));

  SpectrumRequest emptyExcludedFrequency = validRequest();
  emptyExcludedFrequency.constraints.excludedFrequencies.push_back(FrequencyRange{42, 42});
  WF_CHECK(rejected(emptyExcludedFrequency));

  // Excluded frequencies are only required to be non-empty; unlike a request
  // frequency window they carry no model bound.
  SpectrumRequest negativeExcluded = validRequest();
  negativeExcluded.constraints.excludedFrequencies.push_back(FrequencyRange{-5, 5});
  WF_CHECK(accepted(negativeExcluded));

  SpectrumRequest maxConflicts = validRequest();
  for (std::uint64_t index = 1; index <= kMaxExcludedRanges; ++index) {
    maxConflicts.constraints.mustNotConflictWith.push_back(ReservationId(index));
  }
  WF_CHECK(accepted(maxConflicts));

  SpectrumRequest tooManyConflicts = maxConflicts;
  tooManyConflicts.constraints.mustNotConflictWith.push_back(ReservationId(1000));
  WF_CHECK(rejected(tooManyConflicts));

  SpectrumRequest zeroConflict = validRequest();
  zeroConflict.constraints.mustNotConflictWith.push_back(ReservationId(0));
  WF_CHECK(rejected(zeroConflict));
}

WF_TEST(validate_request_shape_bounds_guard_band_and_lease) {
  SpectrumRequest negativeGuard = validRequest();
  negativeGuard.guardBandMhz = -1;
  WF_CHECK(rejected(negativeGuard));

  SpectrumRequest zeroGuard = validRequest();
  zeroGuard.guardBandMhz = 0;
  WF_CHECK(accepted(zeroGuard));

  SpectrumRequest maxGuard = validRequest();
  maxGuard.guardBandMhz = kMaxGuardBandMhz;
  WF_CHECK(accepted(maxGuard));

  SpectrumRequest guardOverflow = validRequest();
  guardOverflow.guardBandMhz = kMaxGuardBandMhz + 1;
  WF_CHECK(rejected(guardOverflow));

  SpectrumRequest zeroLease = validRequest();
  zeroLease.leaseDuration = Duration::zero();
  WF_CHECK(rejected(zeroLease));

  SpectrumRequest negativeLease = validRequest();
  negativeLease.leaseDuration = Duration::nanos(-1);
  WF_CHECK(rejected(negativeLease));

  SpectrumRequest oneNanosecond = validRequest();
  oneNanosecond.leaseDuration = Duration::nanos(1);
  WF_CHECK(accepted(oneNanosecond));

  SpectrumRequest tenYears = validRequest();
  tenYears.leaseDuration = Duration::days(3650);
  WF_CHECK(accepted(tenYears));

  SpectrumRequest justOverTenYears = validRequest();
  justOverTenYears.leaseDuration = Duration::nanos(Duration::days(3650).nanos() + 1);
  WF_CHECK(rejected(justOverTenYears));
}

WF_TEST(validate_request_shape_bounds_instants) {
  SpectrumRequest zeroRequestedAt = validRequest();
  zeroRequestedAt.requestedAt = Instant{};
  WF_CHECK(rejected(zeroRequestedAt));

  SpectrumRequest negativeRequestedAt = validRequest();
  negativeRequestedAt.requestedAt = Instant::fromNanos(-1);
  WF_CHECK(rejected(negativeRequestedAt));

  SpectrumRequest firstNanosecond = validRequest();
  firstNanosecond.requestedAt = Instant::fromNanos(1);
  WF_CHECK(accepted(firstNanosecond));

  SpectrumRequest negativeNotBefore = validRequest();
  negativeNotBefore.notBefore = Instant::fromNanos(-1);
  WF_CHECK(rejected(negativeNotBefore));

  SpectrumRequest zeroNotBefore = validRequest();
  zeroNotBefore.notBefore = Instant{};
  WF_CHECK(accepted(zeroNotBefore));

  SpectrumRequest leaseEndOverflow = validRequest();
  leaseEndOverflow.requestedAt = Instant::max();
  leaseEndOverflow.leaseDuration = Duration::nanos(1);
  WF_CHECK(rejected(leaseEndOverflow));

  SpectrumRequest leaseEndExact = validRequest();
  leaseEndExact.requestedAt = Instant::max() - Duration::nanos(1);
  leaseEndExact.leaseDuration = Duration::nanos(1);
  WF_CHECK(accepted(leaseEndExact));

  SpectrumRequest leaseEndOnePast = validRequest();
  leaseEndOnePast.requestedAt = Instant::max() - Duration::nanos(1);
  leaseEndOnePast.leaseDuration = Duration::nanos(2);
  WF_CHECK(rejected(leaseEndOnePast));
}

WF_TEST(validate_request_shape_requires_both_authority_tokens) {
  SpectrumRequest noEligibility = validRequest();
  noEligibility.eligibilityAuthority = EligibilityAuthority{};
  WF_CHECK(rejected(noEligibility));

  SpectrumRequest zeroEligibilityGeneration = validRequest();
  zeroEligibilityGeneration.eligibilityAuthority.generation = EligibilityAuthorityGeneration(0);
  WF_CHECK(rejected(zeroEligibilityGeneration));

  SpectrumRequest zeroEligibilityEpoch = validRequest();
  zeroEligibilityEpoch.eligibilityAuthority.fence.epoch = ControllerEpoch(0);
  WF_CHECK(rejected(zeroEligibilityEpoch));

  SpectrumRequest zeroEligibilityIncarnation = validRequest();
  zeroEligibilityIncarnation.eligibilityAuthority.fence.incarnation = ControllerIncarnation(0);
  WF_CHECK(rejected(zeroEligibilityIncarnation));

  SpectrumRequest noReservation = validRequest();
  noReservation.reservationAuthority = ReservationAuthority{};
  WF_CHECK(rejected(noReservation));

  SpectrumRequest zeroReservationGeneration = validRequest();
  zeroReservationGeneration.reservationAuthority.generation = ReservationAuthorityGeneration(0);
  WF_CHECK(rejected(zeroReservationGeneration));

  SpectrumRequest zeroReservationEpoch = validRequest();
  zeroReservationEpoch.reservationAuthority.fence.epoch = ControllerEpoch(0);
  WF_CHECK(rejected(zeroReservationEpoch));

  SpectrumRequest zeroReservationIncarnation = validRequest();
  zeroReservationIncarnation.reservationAuthority.fence.incarnation = ControllerIncarnation(0);
  WF_CHECK(rejected(zeroReservationIncarnation));

  // Shape validation only requires a well-formed token; whether the token is
  // current is decided by the runtime against its authoritative state.
  SpectrumRequest foreignTokens = validRequest();
  foreignTokens.eligibilityAuthority.generation = EligibilityAuthorityGeneration(999);
  foreignTokens.eligibilityAuthority.fence = ControllerFence{ControllerEpoch(9), ControllerIncarnation(9)};
  foreignTokens.reservationAuthority.generation = ReservationAuthorityGeneration(999);
  foreignTokens.reservationAuthority.fence = ControllerFence{ControllerEpoch(9), ControllerIncarnation(9)};
  WF_CHECK(accepted(foreignTokens));
}

WF_TEST_MAIN()
