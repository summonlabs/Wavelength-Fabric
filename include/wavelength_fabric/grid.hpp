#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "wavelength_fabric/error.hpp"
#include "wavelength_fabric/identity.hpp"
#include "wavelength_fabric/quantity.hpp"

// Abstract channel grids.
//
// A grid is a purely arithmetic, vendor-neutral description of an ordered set
// of equal-width frequency slots. Wavelength Fabric does not claim conformance
// to any standard grid definition; a fixed grid here means "one slot per
// channel", and a flex grid here means "a channel occupies one or more
// contiguous slots". Nothing in this header asserts physical optical behavior.

namespace wavelength_fabric {

enum class GridKind : std::uint8_t {
  Unknown = 0,  // rejected by validateGrid
  Fixed = 1,    // one slot per channel; slot width is the channel spacing
  Flex = 2,     // a channel occupies [minSlotsPerChannel, maxSlotsPerChannel] contiguous slots
};

[[nodiscard]] std::string_view toToken(GridKind kind) noexcept;
[[nodiscard]] GridKind gridKindFromToken(std::string_view token) noexcept;

struct ChannelGrid {
  ChannelGridId id{};
  GridGeneration generation{};
  GridKind kind{GridKind::Fixed};

  // Lower edge of slot 0.
  std::int64_t anchorMhz{0};
  // Width of one slot.
  std::int64_t slotWidthMhz{0};
  // Number of slots in the grid.
  std::uint32_t slotCount{0};

  std::uint32_t minSlotsPerChannel{1};
  std::uint32_t maxSlotsPerChannel{1};

  std::string label;

  [[nodiscard]] std::int64_t endMhz() const noexcept { return anchorMhz + slotWidthMhz * static_cast<std::int64_t>(slotCount); }
};

// Validates a grid definition. Every field is range-checked and every derived
// quantity uses checked arithmetic; an invalid grid is never registered.
[[nodiscard]] Status validateGrid(const ChannelGrid& grid);

[[nodiscard]] constexpr std::int64_t floorDiv(std::int64_t a, std::int64_t b) noexcept {
  const std::int64_t q = a / b;
  const std::int64_t r = a % b;
  return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

[[nodiscard]] constexpr std::int64_t ceilDiv(std::int64_t a, std::int64_t b) noexcept {
  const std::int64_t q = a / b;
  const std::int64_t r = a % b;
  return (r != 0 && ((r < 0) == (b < 0))) ? q + 1 : q;
}

// Returns true when the range lies inside the grid.
[[nodiscard]] bool slotRangeInGrid(const ChannelGrid& grid, SlotRange range) noexcept;

// Lower edge frequency of a slot index. Returns false when the result would
// leave the representable frequency bounds.
[[nodiscard]] bool slotFrequencyMhz(const ChannelGrid& grid, std::uint32_t slot, std::int64_t& outMhz) noexcept;

// Frequency range covered by a slot range.
[[nodiscard]] bool frequencyOfSlotRange(const ChannelGrid& grid, SlotRange range, FrequencyRange& out) noexcept;

// Smallest slot range fully covering a frequency range on this grid. Returns
// false when the frequency range cannot be represented on the grid at all.
[[nodiscard]] bool slotRangeForFrequency(const ChannelGrid& grid, FrequencyRange range, SlotRange& out) noexcept;

// True when the slot range is aligned such that a channel of that width is
// representable on the grid (fixed grids: exactly one slot; flex grids: within
// the per-channel slot bounds).
[[nodiscard]] bool channelWidthSupported(const ChannelGrid& grid, std::uint32_t slots) noexcept;

[[nodiscard]] std::string describeGrid(const ChannelGrid& grid);
[[nodiscard]] std::string describeSlotRange(const ChannelGrid& grid, SlotRange range);

}  // namespace wavelength_fabric
