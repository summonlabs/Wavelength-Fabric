#include "wavelength_fabric/grid.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Channel grid validation and slot arithmetic.
//
// A grid is pure arithmetic over equal-width slots, so every derived quantity is
// computed with the checked helpers from quantity.hpp and every bound is
// re-tested here rather than trusted: a slot index that wrapped would silently
// name a different frequency range, and that is the one error the spectrum model
// must never make.

namespace wavelength_fabric {
namespace {

// Upper bound on the human-readable label of a registered definition, in bytes.
constexpr std::size_t kMaxLabelBytes = 256;

// Exclusive upper edge of a grid: anchorMhz + slotCount * slotWidthMhz.
[[nodiscard]] bool gridEndMhz(const ChannelGrid& grid, std::int64_t& outMhz) noexcept {
  std::int64_t span = 0;
  const std::int64_t count = static_cast<std::int64_t>(grid.slotCount);
  if (mulOverflow(count, grid.slotWidthMhz, span)) return false;
  return !addOverflow(grid.anchorMhz, span, outMhz);
}

// A grid is usable for slot arithmetic only when it satisfies every structural
// bound validateGrid enforces. The helpers below accept a grid the caller may
// never have registered, so each re-tests the bounds it depends on instead of
// trusting them.
[[nodiscard]] bool gridUsable(const ChannelGrid& grid) noexcept {
  if (grid.kind != GridKind::Fixed && grid.kind != GridKind::Flex) return false;
  if (grid.anchorMhz < kMinFrequencyMhz || grid.anchorMhz > kMaxFrequencyMhz) return false;
  if (grid.slotWidthMhz < kMinSlotWidthMhz || grid.slotWidthMhz > kMaxSlotWidthMhz) return false;
  if (grid.slotCount == 0 || grid.slotCount > kMaxGridSlots) return false;
  std::int64_t endMhz = 0;
  if (!gridEndMhz(grid, endMhz)) return false;
  return endMhz <= kMaxFrequencyMhz;
}

}  // namespace

std::string_view toToken(GridKind kind) noexcept {
  switch (kind) {
    case GridKind::Fixed:
      return "fixed";
    case GridKind::Flex:
      return "flex";
    case GridKind::Unknown:
      break;
  }
  return "unknown";
}

GridKind gridKindFromToken(std::string_view token) noexcept {
  if (token == "fixed") return GridKind::Fixed;
  if (token == "flex") return GridKind::Flex;
  if (token == "unknown") return GridKind::Unknown;
  return GridKind::Unknown;
}

Status validateGrid(const ChannelGrid& grid) {
  if (!grid.id.valid()) return fail(StatusCode::InvalidArgument, "grid id must be non-zero");
  if (!grid.generation.valid())
    return fail(StatusCode::InvalidArgument, "grid generation must be non-zero");
  if (grid.kind != GridKind::Fixed && grid.kind != GridKind::Flex)
    return fail(StatusCode::InvalidArgument,
                "grid kind must be fixed or flex, not " + std::string(toToken(grid.kind)));
  if (grid.anchorMhz < kMinFrequencyMhz || grid.anchorMhz > kMaxFrequencyMhz)
    return fail(StatusCode::InvalidArgument,
                "grid anchor " + std::to_string(grid.anchorMhz) +
                    " MHz is outside the supported range [" + std::to_string(kMinFrequencyMhz) +
                    ", " + std::to_string(kMaxFrequencyMhz) + "] MHz");
  if (grid.slotWidthMhz < kMinSlotWidthMhz || grid.slotWidthMhz > kMaxSlotWidthMhz)
    return fail(StatusCode::InvalidArgument,
                "grid slot width " + std::to_string(grid.slotWidthMhz) +
                    " MHz is outside the supported range [" + std::to_string(kMinSlotWidthMhz) +
                    ", " + std::to_string(kMaxSlotWidthMhz) + "] MHz");
  if (grid.slotCount == 0)
    return fail(StatusCode::InvalidArgument, "grid slot count must be at least 1");
  if (grid.slotCount > kMaxGridSlots)
    return fail(StatusCode::InvalidArgument,
                "grid slot count " + std::to_string(grid.slotCount) + " exceeds the maximum of " +
                    std::to_string(kMaxGridSlots) + " slots");
  std::int64_t span = 0;
  if (mulOverflow(static_cast<std::int64_t>(grid.slotCount), grid.slotWidthMhz, span))
    return fail(StatusCode::InvalidArgument,
                "grid span of " + std::to_string(grid.slotCount) + " slots at " +
                    std::to_string(grid.slotWidthMhz) +
                    " MHz per slot overflows the frequency range");
  std::int64_t endMhz = 0;
  if (addOverflow(grid.anchorMhz, span, endMhz))
    return fail(StatusCode::InvalidArgument,
                "grid end frequency overflows the supported frequency range");
  if (endMhz > kMaxFrequencyMhz)
    return fail(StatusCode::InvalidArgument,
                "grid end frequency " + std::to_string(endMhz) + " MHz exceeds the maximum of " +
                    std::to_string(kMaxFrequencyMhz) + " MHz");
  if (grid.kind == GridKind::Fixed) {
    if (grid.minSlotsPerChannel != 1 || grid.maxSlotsPerChannel != 1)
      return fail(StatusCode::InvalidArgument,
                  "fixed grid must allow exactly one slot per channel, but allows [" +
                      std::to_string(grid.minSlotsPerChannel) + ", " +
                      std::to_string(grid.maxSlotsPerChannel) + "]");
  } else {
    if (grid.minSlotsPerChannel == 0)
      return fail(StatusCode::InvalidArgument,
                  "flex grid minimum slots per channel must be at least 1");
    if (grid.maxSlotsPerChannel < grid.minSlotsPerChannel)
      return fail(StatusCode::InvalidArgument,
                  "flex grid maximum slots per channel " +
                      std::to_string(grid.maxSlotsPerChannel) + " is below the minimum of " +
                      std::to_string(grid.minSlotsPerChannel));
    if (grid.maxSlotsPerChannel > kMaxSlotsPerChannel)
      return fail(StatusCode::InvalidArgument,
                  "flex grid maximum slots per channel " +
                      std::to_string(grid.maxSlotsPerChannel) + " exceeds the maximum of " +
                      std::to_string(kMaxSlotsPerChannel));
  }
  if (grid.label.size() > kMaxLabelBytes)
    return fail(StatusCode::InvalidArgument,
                "grid label of " + std::to_string(grid.label.size()) +
                    " bytes exceeds the maximum of " + std::to_string(kMaxLabelBytes) + " bytes");
  return Status::success();
}

bool slotRangeInGrid(const ChannelGrid& grid, SlotRange range) noexcept {
  if (range.count == 0) return false;
  if (range.count > kMaxGridSlots) return false;
  const std::int64_t first = static_cast<std::int64_t>(range.first);
  const std::int64_t count = static_cast<std::int64_t>(range.count);
  std::int64_t end = 0;
  if (addOverflow(first, count, end)) return false;
  return end <= static_cast<std::int64_t>(grid.slotCount);
}

bool slotFrequencyMhz(const ChannelGrid& grid, std::uint32_t slot, std::int64_t& outMhz) noexcept {
  if (!gridUsable(grid)) return false;
  if (slot >= grid.slotCount) return false;
  std::int64_t offset = 0;
  if (mulOverflow(static_cast<std::int64_t>(slot), grid.slotWidthMhz, offset)) return false;
  std::int64_t value = 0;
  if (addOverflow(grid.anchorMhz, offset, value)) return false;
  outMhz = value;
  return true;
}

bool frequencyOfSlotRange(const ChannelGrid& grid, SlotRange range, FrequencyRange& out) noexcept {
  if (!slotRangeInGrid(grid, range)) return false;
  std::int64_t lowMhz = 0;
  if (!slotFrequencyMhz(grid, range.first, lowMhz)) return false;
  const std::int64_t first = static_cast<std::int64_t>(range.first);
  const std::int64_t count = static_cast<std::int64_t>(range.count);
  std::int64_t endIndex = 0;
  if (addOverflow(first, count, endIndex)) return false;
  std::int64_t offset = 0;
  if (mulOverflow(endIndex, grid.slotWidthMhz, offset)) return false;
  std::int64_t highMhz = 0;
  if (addOverflow(grid.anchorMhz, offset, highMhz)) return false;
  out.lowMhz = lowMhz;
  out.highMhz = highMhz;
  return true;
}

bool slotRangeForFrequency(const ChannelGrid& grid, FrequencyRange range, SlotRange& out) noexcept {
  if (range.empty()) return false;
  if (!gridUsable(grid)) return false;
  std::int64_t gridEnd = 0;
  if (!gridEndMhz(grid, gridEnd)) return false;
  if (range.lowMhz < grid.anchorMhz || range.highMhz > gridEnd) return false;
  std::int64_t lowDelta = 0;
  if (subOverflow(range.lowMhz, grid.anchorMhz, lowDelta)) return false;
  std::int64_t highDelta = 0;
  if (subOverflow(range.highMhz, grid.anchorMhz, highDelta)) return false;
  // The lower edge never precedes the anchor, so the clamp is a guard against a
  // signed division surprise rather than an expected case.
  const std::int64_t rawFirst = floorDiv(lowDelta, grid.slotWidthMhz);
  const std::int64_t first = rawFirst < 0 ? 0 : rawFirst;
  const std::int64_t end = ceilDiv(highDelta, grid.slotWidthMhz);
  if (end <= first) return false;
  if (end > static_cast<std::int64_t>(grid.slotCount)) return false;
  std::uint32_t firstSlot = 0;
  std::uint32_t endSlot = 0;
  if (!fitsU32(first, firstSlot)) return false;
  if (!fitsU32(end, endSlot)) return false;
  out.first = firstSlot;
  out.count = endSlot - firstSlot;
  return true;
}

bool channelWidthSupported(const ChannelGrid& grid, std::uint32_t slots) noexcept {
  if (slots == 0) return false;
  switch (grid.kind) {
    case GridKind::Fixed:
      return slots == 1;
    case GridKind::Flex:
      return slots >= grid.minSlotsPerChannel && slots <= grid.maxSlotsPerChannel;
    case GridKind::Unknown:
      break;
  }
  return false;
}

}  // namespace wavelength_fabric
