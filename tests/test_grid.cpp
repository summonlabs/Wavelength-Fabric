#include "test_common.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

// Channel grid validation and slot/frequency arithmetic.
//
// Every expectation below is derived from grid.hpp and src/grid.cpp: the
// rejection branches are ordered, every derived quantity uses checked
// arithmetic, and a grid that fails validation is never registered.

using namespace wavelength_fabric;
using namespace wf_test;

namespace {

template <class Enum>
[[nodiscard]] std::string token(Enum value) {
  return std::string(toToken(value));
}

// True when a definition is refused as structurally invalid and the refusal
// names the field it rejected.
[[nodiscard]] bool rejectedAsInvalid(const ChannelGrid& grid) {
  const Status status = validateGrid(grid);
  return status.code == StatusCode::InvalidArgument && !status.message.empty();
}

[[nodiscard]] bool accepted(const ChannelGrid& grid) { return validateGrid(grid).ok(); }

}  // namespace

WF_TEST(grid_kind_tokens_round_trip) {
  WF_CHECK_EQ(token(GridKind::Fixed), std::string("fixed"));
  WF_CHECK_EQ(token(GridKind::Flex), std::string("flex"));
  WF_CHECK_EQ(token(GridKind::Unknown), std::string("unknown"));
  WF_CHECK_EQ(token(static_cast<GridKind>(7)), std::string("unknown"));

  WF_CHECK(gridKindFromToken("fixed") == GridKind::Fixed);
  WF_CHECK(gridKindFromToken("flex") == GridKind::Flex);
  WF_CHECK(gridKindFromToken("unknown") == GridKind::Unknown);
  // Exact-match lookup: an unrecognised token is Unknown, never a guess.
  WF_CHECK(gridKindFromToken("Fixed") == GridKind::Unknown);
  WF_CHECK(gridKindFromToken("FIXED") == GridKind::Unknown);
  WF_CHECK(gridKindFromToken("") == GridKind::Unknown);
  WF_CHECK(gridKindFromToken("fixed ") == GridKind::Unknown);
  WF_CHECK(gridKindFromToken("flex-grid") == GridKind::Unknown);

  for (const GridKind kind : {GridKind::Fixed, GridKind::Flex, GridKind::Unknown}) {
    WF_CHECK(gridKindFromToken(toToken(kind)) == kind);
  }
}

WF_TEST(validate_grid_accepts_documented_bounds) {
  WF_CHECK(accepted(fixedGrid()));
  WF_CHECK(accepted(flexGrid()));

  ChannelGrid maxSlots = fixedGrid();
  maxSlots.slotCount = kMaxGridSlots;
  maxSlots.slotWidthMhz = kMinSlotWidthMhz;
  maxSlots.anchorMhz = kMinFrequencyMhz;
  WF_CHECK(accepted(maxSlots));

  ChannelGrid maxChannel = flexGrid();
  maxChannel.minSlotsPerChannel = 1;
  maxChannel.maxSlotsPerChannel = kMaxSlotsPerChannel;
  WF_CHECK(accepted(maxChannel));

  ChannelGrid maxWidth = fixedGrid();
  maxWidth.slotWidthMhz = kMaxSlotWidthMhz;
  maxWidth.slotCount = 1;
  WF_CHECK(accepted(maxWidth));

  ChannelGrid edgeAnchor = fixedGrid();
  edgeAnchor.anchorMhz = kMaxFrequencyMhz;
  edgeAnchor.slotWidthMhz = 1;
  edgeAnchor.slotCount = 1;
  WF_CHECK(rejectedAsInvalid(edgeAnchor));  // end frequency leaves the model

  ChannelGrid label256 = fixedGrid();
  label256.label.assign(256, 'g');
  WF_CHECK(accepted(label256));

  ChannelGrid flexEqual = flexGrid();
  flexEqual.minSlotsPerChannel = 4;
  flexEqual.maxSlotsPerChannel = 4;
  WF_CHECK(accepted(flexEqual));
}

WF_TEST(validate_grid_rejects_every_field) {
  ChannelGrid zeroId = fixedGrid();
  zeroId.id = ChannelGridId(0);
  WF_CHECK(rejectedAsInvalid(zeroId));

  ChannelGrid zeroGeneration = fixedGrid();
  zeroGeneration.generation = GridGeneration(0);
  WF_CHECK(rejectedAsInvalid(zeroGeneration));

  ChannelGrid unknownKind = fixedGrid();
  unknownKind.kind = GridKind::Unknown;
  WF_CHECK(rejectedAsInvalid(unknownKind));

  ChannelGrid exoticKind = fixedGrid();
  exoticKind.kind = static_cast<GridKind>(9);
  WF_CHECK(rejectedAsInvalid(exoticKind));

  ChannelGrid anchorZero = fixedGrid();
  anchorZero.anchorMhz = 0;
  WF_CHECK(rejectedAsInvalid(anchorZero));

  ChannelGrid anchorHigh = fixedGrid();
  anchorHigh.anchorMhz = kMaxFrequencyMhz + 1;
  WF_CHECK(rejectedAsInvalid(anchorHigh));

  ChannelGrid widthZero = fixedGrid();
  widthZero.slotWidthMhz = 0;
  WF_CHECK(rejectedAsInvalid(widthZero));

  ChannelGrid widthHigh = fixedGrid();
  widthHigh.slotWidthMhz = kMaxSlotWidthMhz + 1;
  WF_CHECK(rejectedAsInvalid(widthHigh));

  ChannelGrid countZero = fixedGrid();
  countZero.slotCount = 0;
  WF_CHECK(rejectedAsInvalid(countZero));

  ChannelGrid countHigh = fixedGrid();
  countHigh.slotCount = kMaxGridSlots + 1;
  WF_CHECK(rejectedAsInvalid(countHigh));

  ChannelGrid endBeyondModel = fixedGrid();
  endBeyondModel.anchorMhz = 900'000'000;
  endBeyondModel.slotWidthMhz = 1'000'000;
  endBeyondModel.slotCount = 200;
  WF_CHECK(rejectedAsInvalid(endBeyondModel));

  ChannelGrid fixedMinTwo = fixedGrid();
  fixedMinTwo.minSlotsPerChannel = 2;
  WF_CHECK(rejectedAsInvalid(fixedMinTwo));

  ChannelGrid fixedMaxZero = fixedGrid();
  fixedMaxZero.maxSlotsPerChannel = 0;
  WF_CHECK(rejectedAsInvalid(fixedMaxZero));

  ChannelGrid fixedRange = fixedGrid();
  fixedRange.minSlotsPerChannel = 1;
  fixedRange.maxSlotsPerChannel = 2;
  WF_CHECK(rejectedAsInvalid(fixedRange));

  ChannelGrid flexMinZero = flexGrid();
  flexMinZero.minSlotsPerChannel = 0;
  WF_CHECK(rejectedAsInvalid(flexMinZero));

  ChannelGrid flexInverted = flexGrid();
  flexInverted.minSlotsPerChannel = 8;
  flexInverted.maxSlotsPerChannel = 4;
  WF_CHECK(rejectedAsInvalid(flexInverted));

  ChannelGrid flexMaxHigh = flexGrid();
  flexMaxHigh.maxSlotsPerChannel = kMaxSlotsPerChannel + 1;
  WF_CHECK(rejectedAsInvalid(flexMaxHigh));

  ChannelGrid label257 = fixedGrid();
  label257.label.assign(257, 'g');
  WF_CHECK(rejectedAsInvalid(label257));

  ChannelGrid zeroIdFirst = fixedGrid();
  zeroIdFirst.id = ChannelGridId(0);
  zeroIdFirst.kind = GridKind::Unknown;
  zeroIdFirst.anchorMhz = 0;
  WF_CHECK(rejectedAsInvalid(zeroIdFirst));  // the first failing field is reported
}

WF_TEST(rejected_grid_is_never_registered) {
  SpectrumRuntime runtime;

  ChannelGrid badAnchor = fixedGrid();
  badAnchor.anchorMhz = 0;
  const Status refused = runtime.registerGrid(badAnchor);
  WF_CHECK(refused.code == StatusCode::InvalidArgument);
  WF_CHECK(!runtime.grid(badAnchor.id).has_value());
  WF_CHECK_EQ(runtime.grids().size(), std::size_t{0});

  ChannelGrid ok = fixedGrid();
  WF_CHECK(runtime.registerGrid(ok).ok());
  WF_CHECK(runtime.grid(ok.id).has_value());
  WF_CHECK_EQ(runtime.grids().size(), std::size_t{1});

  WF_CHECK(runtime.registerGrid(ok).code == StatusCode::Duplicate);

  ChannelGrid older = fixedGrid();
  older.generation = GridGeneration(0);
  WF_CHECK(runtime.registerGrid(older).code == StatusCode::InvalidArgument);

  ChannelGrid newer = fixedGrid();
  newer.generation = GridGeneration(2);
  newer.label = "fixed-1-gen2";
  WF_CHECK(runtime.registerGrid(newer).ok());
  const std::optional<ChannelGrid> stored = runtime.grid(ok.id);
  WF_REQUIRE(stored.has_value());
  WF_CHECK_EQ(stored->generation.raw(), std::uint64_t{2});
  WF_CHECK_EQ(stored->label, std::string("fixed-1-gen2"));
  WF_CHECK_EQ(runtime.grids().size(), std::size_t{1});

  const Status beyondBound = runtime.registerGrid(flexGrid(ChannelGridId(2)));
  WF_CHECK(beyondBound.ok());
}

WF_TEST(slot_range_in_grid_boundaries) {
  const ChannelGrid grid = fixedGrid();  // 96 slots

  WF_CHECK(slotRangeInGrid(grid, SlotRange{0, 1}));
  WF_CHECK(slotRangeInGrid(grid, SlotRange{95, 1}));
  WF_CHECK(slotRangeInGrid(grid, SlotRange{0, 96}));
  WF_CHECK(!slotRangeInGrid(grid, SlotRange{96, 1}));
  WF_CHECK(!slotRangeInGrid(grid, SlotRange{95, 2}));
  WF_CHECK(!slotRangeInGrid(grid, SlotRange{0, 97}));
  WF_CHECK(!slotRangeInGrid(grid, SlotRange{0, 0}));
  WF_CHECK(!slotRangeInGrid(grid, SlotRange{5, 0}));
  WF_CHECK(!slotRangeInGrid(grid, SlotRange{0xFFFF'FFFFu, 1}));

  ChannelGrid maxGrid = fixedGrid();
  maxGrid.slotCount = kMaxGridSlots;
  maxGrid.slotWidthMhz = 1;
  maxGrid.anchorMhz = 1;
  WF_CHECK(slotRangeInGrid(maxGrid, SlotRange{0, kMaxGridSlots}));
  WF_CHECK(slotRangeInGrid(maxGrid, SlotRange{kMaxGridSlots - 1, 1}));
  // One past the largest representable count is refused before any arithmetic.
  WF_CHECK(!slotRangeInGrid(maxGrid, SlotRange{0, kMaxGridSlots + 1}));

  ChannelGrid noSlots = fixedGrid();
  noSlots.slotCount = 0;
  WF_CHECK(!slotRangeInGrid(noSlots, SlotRange{0, 1}));
}

WF_TEST(slot_frequency_mhz_boundaries) {
  const ChannelGrid fixed = fixedGrid();  // anchor 191'300'000, 50'000 per slot
  std::int64_t mhz = 0;
  WF_CHECK(slotFrequencyMhz(fixed, 0, mhz));
  WF_CHECK_EQ(mhz, std::int64_t{191'300'000});
  WF_CHECK(slotFrequencyMhz(fixed, 1, mhz));
  WF_CHECK_EQ(mhz, std::int64_t{191'350'000});
  WF_CHECK(slotFrequencyMhz(fixed, 95, mhz));
  WF_CHECK_EQ(mhz, std::int64_t{196'050'000});

  mhz = -12'345;
  WF_CHECK(!slotFrequencyMhz(fixed, 96, mhz));
  WF_CHECK_EQ(mhz, std::int64_t{-12'345});  // a refused query never writes the result

  const ChannelGrid flex = flexGrid();  // anchor 191'300'000, 12'500 per slot, 384 slots
  WF_CHECK(slotFrequencyMhz(flex, 0, mhz));
  WF_CHECK_EQ(mhz, std::int64_t{191'300'000});
  WF_CHECK(slotFrequencyMhz(flex, 383, mhz));
  WF_CHECK_EQ(mhz, std::int64_t{196'087'500});
  WF_CHECK(!slotFrequencyMhz(flex, 384, mhz));

  ChannelGrid unusable = fixedGrid();  // an unvalidated grid is never used for arithmetic
  unusable.anchorMhz = 0;
  WF_CHECK(!slotFrequencyMhz(unusable, 0, mhz));
  unusable = fixedGrid();
  unusable.slotWidthMhz = 0;
  WF_CHECK(!slotFrequencyMhz(unusable, 0, mhz));
  unusable = fixedGrid();
  unusable.slotCount = 0;
  WF_CHECK(!slotFrequencyMhz(unusable, 0, mhz));
  unusable = fixedGrid();
  unusable.kind = GridKind::Unknown;
  WF_CHECK(!slotFrequencyMhz(unusable, 0, mhz));
  unusable = fixedGrid();
  unusable.anchorMhz = kMaxFrequencyMhz;
  unusable.slotWidthMhz = 1'000'000;
  unusable.slotCount = 2;
  WF_CHECK(!slotFrequencyMhz(unusable, 1, mhz));
}

WF_TEST(frequency_of_slot_range_boundaries) {
  const ChannelGrid fixed = fixedGrid();
  FrequencyRange range;

  WF_CHECK(frequencyOfSlotRange(fixed, SlotRange{10, 1}, range));
  WF_CHECK_EQ(range.lowMhz, std::int64_t{191'800'000});
  WF_CHECK_EQ(range.highMhz, std::int64_t{191'850'000});

  WF_CHECK(frequencyOfSlotRange(fixed, SlotRange{95, 1}, range));
  WF_CHECK_EQ(range.lowMhz, std::int64_t{196'050'000});
  WF_CHECK_EQ(range.highMhz, std::int64_t{196'100'000});

  WF_CHECK(frequencyOfSlotRange(fixed, SlotRange{0, 96}, range));
  WF_CHECK_EQ(range.lowMhz, std::int64_t{191'300'000});
  WF_CHECK_EQ(range.highMhz, std::int64_t{196'100'000});
  WF_CHECK_EQ(range.highMhz, fixed.endMhz());

  const FrequencyRange sentinel{-1, -2};
  range = sentinel;
  WF_CHECK(!frequencyOfSlotRange(fixed, SlotRange{95, 2}, range));
  WF_CHECK(range == sentinel);
  WF_CHECK(!frequencyOfSlotRange(fixed, SlotRange{0, 0}, range));
  WF_CHECK(!frequencyOfSlotRange(fixed, SlotRange{96, 1}, range));
  WF_CHECK(!frequencyOfSlotRange(fixed, SlotRange{0xFFFF'FFFFu, 1}, range));

  const ChannelGrid flex = flexGrid();
  WF_CHECK(frequencyOfSlotRange(flex, SlotRange{0, 4}, range));
  WF_CHECK_EQ(range.lowMhz, std::int64_t{191'300'000});
  WF_CHECK_EQ(range.highMhz, std::int64_t{191'350'000});

  ChannelGrid unusable = fixedGrid();
  unusable.anchorMhz = 0;
  WF_CHECK(!frequencyOfSlotRange(unusable, SlotRange{0, 1}, range));
}

WF_TEST(slot_range_for_frequency_boundaries) {
  const ChannelGrid fixed = fixedGrid();  // 50'000 MHz slots, [191'300'000, 196'100'000)
  SlotRange slots;

  WF_CHECK(slotRangeForFrequency(fixed, FrequencyRange{191'300'000, 191'350'000}, slots));
  WF_CHECK_EQ(slots.first, std::uint32_t{0});
  WF_CHECK_EQ(slots.count, std::uint32_t{1});

  // A range that starts in the middle of a slot still needs that whole slot.
  WF_CHECK(slotRangeForFrequency(fixed, FrequencyRange{191'300'001, 191'300'002}, slots));
  WF_CHECK_EQ(slots.first, std::uint32_t{0});
  WF_CHECK_EQ(slots.count, std::uint32_t{1});

  WF_CHECK(slotRangeForFrequency(fixed, FrequencyRange{191'350'000, 191'425'000}, slots));
  WF_CHECK_EQ(slots.first, std::uint32_t{1});
  WF_CHECK_EQ(slots.count, std::uint32_t{2});

  WF_CHECK(slotRangeForFrequency(fixed, FrequencyRange{191'300'000, 191'300'001}, slots));
  WF_CHECK_EQ(slots.first, std::uint32_t{0});
  WF_CHECK_EQ(slots.count, std::uint32_t{1});

  WF_CHECK(slotRangeForFrequency(fixed, FrequencyRange{196'099'999, 196'100'000}, slots));
  WF_CHECK_EQ(slots.first, std::uint32_t{95});
  WF_CHECK_EQ(slots.count, std::uint32_t{1});

  WF_CHECK(slotRangeForFrequency(fixed, FrequencyRange{191'300'000, 196'100'000}, slots));
  WF_CHECK_EQ(slots.first, std::uint32_t{0});
  WF_CHECK_EQ(slots.count, std::uint32_t{96});

  // Outside the grid, empty, or spanning the grid edge: not representable.
  WF_CHECK(!slotRangeForFrequency(fixed, FrequencyRange{191'299'999, 191'350'000}, slots));
  WF_CHECK(!slotRangeForFrequency(fixed, FrequencyRange{191'300'000, 196'100'001}, slots));
  WF_CHECK(!slotRangeForFrequency(fixed, FrequencyRange{191'350'000, 191'350'000}, slots));
  WF_CHECK(!slotRangeForFrequency(fixed, FrequencyRange{191'400'000, 191'300'000}, slots));
  WF_CHECK(!slotRangeForFrequency(fixed, FrequencyRange{196'100'000, 196'100'001}, slots));

  const ChannelGrid flex = flexGrid();  // 12'500 MHz slots
  WF_CHECK(slotRangeForFrequency(flex, FrequencyRange{191'300'000, 191'312'500}, slots));
  WF_CHECK_EQ(slots.first, std::uint32_t{0});
  WF_CHECK_EQ(slots.count, std::uint32_t{1});
  WF_CHECK(slotRangeForFrequency(flex, FrequencyRange{191'300'000, 191'300'001}, slots));
  WF_CHECK_EQ(slots.count, std::uint32_t{1});
  WF_CHECK(slotRangeForFrequency(flex, FrequencyRange{191'300'000, 196'100'000}, slots));
  WF_CHECK_EQ(slots.count, std::uint32_t{384});

  ChannelGrid unusable = fixedGrid();
  unusable.anchorMhz = 0;
  WF_CHECK(!slotRangeForFrequency(unusable, FrequencyRange{0, 1}, slots));
}

WF_TEST(floor_and_ceil_division_used_by_grid_mapping) {
  WF_CHECK_EQ(floorDiv(7, 2), std::int64_t{3});
  WF_CHECK_EQ(floorDiv(-7, 2), std::int64_t{-4});
  WF_CHECK_EQ(floorDiv(7, -2), std::int64_t{-4});
  WF_CHECK_EQ(floorDiv(-1, 2), std::int64_t{-1});
  WF_CHECK_EQ(floorDiv(6, 2), std::int64_t{3});
  WF_CHECK_EQ(ceilDiv(7, 2), std::int64_t{4});
  WF_CHECK_EQ(ceilDiv(-7, 2), std::int64_t{-3});
  WF_CHECK_EQ(ceilDiv(-1, 2), std::int64_t{0});
  WF_CHECK_EQ(ceilDiv(6, 2), std::int64_t{3});
  WF_CHECK_EQ(floorDiv(0, 50'000), std::int64_t{0});
  WF_CHECK_EQ(ceilDiv(0, 50'000), std::int64_t{0});
}

WF_TEST(channel_width_supported_semantics) {
  const ChannelGrid fixed = fixedGrid();
  WF_CHECK(channelWidthSupported(fixed, 1));
  WF_CHECK(!channelWidthSupported(fixed, 0));
  WF_CHECK(!channelWidthSupported(fixed, 2));
  WF_CHECK(!channelWidthSupported(fixed, kMaxSlotsPerChannel));

  const ChannelGrid flex = flexGrid();  // [1, 32]
  WF_CHECK(channelWidthSupported(flex, 1));
  WF_CHECK(channelWidthSupported(flex, 32));
  WF_CHECK(!channelWidthSupported(flex, 0));
  WF_CHECK(!channelWidthSupported(flex, 33));

  const ChannelGrid narrow = flexGrid(ChannelGridId(3), GridGeneration(1), 191'300'000, 12'500, 384, 4, 8);
  WF_CHECK(!channelWidthSupported(narrow, 3));
  WF_CHECK(channelWidthSupported(narrow, 4));
  WF_CHECK(channelWidthSupported(narrow, 8));
  WF_CHECK(!channelWidthSupported(narrow, 9));

  const ChannelGrid single = flexGrid(ChannelGridId(4), GridGeneration(1), 191'300'000, 12'500, 384, 2, 2);
  WF_CHECK(!channelWidthSupported(single, 1));
  WF_CHECK(channelWidthSupported(single, 2));
  WF_CHECK(!channelWidthSupported(single, 3));

  ChannelGrid unknownKind = flexGrid();
  unknownKind.kind = GridKind::Unknown;
  WF_CHECK(!channelWidthSupported(unknownKind, 1));
  WF_CHECK(!channelWidthSupported(unknownKind, 16));
}

WF_TEST(slot_and_frequency_range_algebra) {
  const SlotRange base{3, 4};
  const SlotRange straddle{6, 2};
  const SlotRange adjacent{7, 2};
  const SlotRange emptySlots{3, 0};
  const SlotRange inside{4, 2};
  const SlotRange wider{3, 5};
  WF_CHECK_EQ(base.end(), std::uint32_t{7});
  WF_CHECK(base.overlaps(straddle));
  WF_CHECK(!base.overlaps(adjacent));
  WF_CHECK(!emptySlots.overlaps(base));
  WF_CHECK(emptySlots.empty());
  WF_CHECK(base.contains(inside));
  WF_CHECK(!base.contains(wider));
  WF_CHECK(base.contains(emptySlots));
  WF_CHECK(!emptySlots.touches(emptySlots));

  const FrequencyRange band{10, 20};
  const FrequencyRange same{10, 20};
  const FrequencyRange touching{20, 30};
  const FrequencyRange straddling{19, 21};
  const FrequencyRange zeroWidth{5, 5};
  const FrequencyRange inverted{20, 10};
  const FrequencyRange longer{10, 21};
  WF_CHECK(!zeroWidth.overlaps(FrequencyRange(10, 20)));
  WF_CHECK(band.overlaps(straddling));
  WF_CHECK(!band.overlaps(touching));
  WF_CHECK(band.contains(same));
  WF_CHECK(!band.contains(longer));
  WF_CHECK(zeroWidth.empty());
  WF_CHECK(inverted.empty());

  // Adjacent slot ranges meet exactly at a frequency boundary: the property the
  // guard-band arithmetic in the runtime relies on.
  const ChannelGrid grid = fixedGrid();
  FrequencyRange left;
  FrequencyRange right;
  WF_REQUIRE(frequencyOfSlotRange(grid, SlotRange{4, 1}, left));
  WF_REQUIRE(frequencyOfSlotRange(grid, SlotRange{5, 1}, right));
  WF_CHECK_EQ(left.highMhz, right.lowMhz);
  WF_CHECK(!left.overlaps(right));
}

WF_TEST_MAIN()
