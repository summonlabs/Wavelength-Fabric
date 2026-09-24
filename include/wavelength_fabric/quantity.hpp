#pragma once

#include <cstdint>

// Checked integer arithmetic and the numeric bounds of the spectrum model.
//
// Every quantity that originates outside the runtime (a request, a persisted
// record, a decoded frame) is range-checked before it is used to size an
// allocation or index a container. Silent wraparound is never acceptable in
// spectrum arithmetic, because a wrapped slot index would alias a different
// physical frequency range.

namespace wavelength_fabric {

// ---------------------------------------------------------------------------
// Checked arithmetic
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr bool addOverflow(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  constexpr std::int64_t kMax = 0x7FFF'FFFF'FFFF'FFFFll;
  constexpr std::int64_t kMin = -0x7FFF'FFFF'FFFF'FFFFll - 1;
  if (b > 0 && a > kMax - b) return true;
  if (b < 0 && a < kMin - b) return true;
  out = a + b;
  return false;
}

[[nodiscard]] constexpr bool subOverflow(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  constexpr std::int64_t kMax = 0x7FFF'FFFF'FFFF'FFFFll;
  constexpr std::int64_t kMin = -0x7FFF'FFFF'FFFF'FFFFll - 1;
  if (b > 0 && a < kMin + b) return true;
  if (b < 0 && a > kMax + b) return true;
  out = a - b;
  return false;
}

[[nodiscard]] constexpr bool mulOverflow(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if (a == 0 || b == 0) {
    out = 0;
    return false;
  }
  const bool negative = (a < 0) != (b < 0);
  using U = std::uint64_t;
  const U ua = a < 0 ? (U{0} - static_cast<U>(a)) : static_cast<U>(a);
  const U ub = b < 0 ? (U{0} - static_cast<U>(b)) : static_cast<U>(b);
  const U limit = negative ? (U{1} << 63) : 0x7FFF'FFFF'FFFF'FFFFull;
  if (ua > limit / ub) return true;
  const U magnitude = ua * ub;
  if (!negative) {
    out = static_cast<std::int64_t>(magnitude);
    return false;
  }
  if (magnitude == (U{1} << 63)) {
    out = -0x7FFF'FFFF'FFFF'FFFFll - 1;
    return false;
  }
  out = -static_cast<std::int64_t>(magnitude);
  return false;
}

// Narrowing with an explicit range check.
[[nodiscard]] constexpr bool fitsU32(std::int64_t value, std::uint32_t& out) noexcept {
  if (value < 0 || value > 0xFFFF'FFFFll) return false;
  out = static_cast<std::uint32_t>(value);
  return true;
}

[[nodiscard]] constexpr bool fitsU16(std::int64_t value, std::uint16_t& out) noexcept {
  if (value < 0 || value > 0xFFFFll) return false;
  out = static_cast<std::uint16_t>(value);
  return true;
}

// ---------------------------------------------------------------------------
// Spectrum bounds
// ---------------------------------------------------------------------------

// Abstract frequency limits. The runtime models frequency as an integer number
// of megahertz inside [1 MHz, 1000 THz]. These bounds are data-model bounds and
// do not assert anything about physical optical hardware.
inline constexpr std::int64_t kMinFrequencyMhz = 1;
inline constexpr std::int64_t kMaxFrequencyMhz = 1'000'000'000ll;

inline constexpr std::int64_t kMinSlotWidthMhz = 1;
inline constexpr std::int64_t kMaxSlotWidthMhz = 1'000'000ll;

inline constexpr std::uint32_t kMaxGridSlots = 1u << 20;
inline constexpr std::uint32_t kMaxSlotsPerChannel = 1u << 16;

inline constexpr std::int64_t kMaxGuardBandMhz = 1'000'000ll;

// Request-shape bounds. They bound every enumeration and every persisted
// record, so a hostile or buggy caller cannot force unbounded work.
inline constexpr std::uint32_t kMaxRequestDomains = 32;
inline constexpr std::uint32_t kMaxExcludedRanges = 64;
inline constexpr std::uint32_t kMaxFrequencyWindows = 64;
inline constexpr std::uint32_t kMaxExclusionDomainMembers = 4096;
inline constexpr std::uint32_t kMaxCandidatesPerRequest = 4096;

// A slot range is half-open: [first, first + count).
struct SlotRange {
  std::uint32_t first{0};
  std::uint32_t count{0};

  [[nodiscard]] constexpr bool empty() const noexcept { return count == 0; }

  // Exclusive end index. Callers must have validated that first + count does
  // not overflow; the constructor path in grid.hpp enforces that bound.
  [[nodiscard]] constexpr std::uint32_t end() const noexcept { return first + count; }

  [[nodiscard]] constexpr bool overlaps(const SlotRange& other) const noexcept {
    if (empty() || other.empty()) return false;
    return first < other.end() && other.first < end();
  }

  [[nodiscard]] constexpr bool touches(const SlotRange& other) const noexcept {
    if (empty() || other.empty()) return false;
    return first <= other.end() && other.first <= end();
  }

  [[nodiscard]] constexpr bool contains(const SlotRange& other) const noexcept {
    if (other.empty()) return true;
    if (empty()) return false;
    return first <= other.first && other.end() <= end();
  }

  friend constexpr bool operator==(const SlotRange&, const SlotRange&) noexcept = default;
};

// A frequency range is half-open: [lowMhz, highMhz).
struct FrequencyRange {
  std::int64_t lowMhz{0};
  std::int64_t highMhz{0};

  [[nodiscard]] constexpr bool empty() const noexcept { return highMhz <= lowMhz; }

  [[nodiscard]] constexpr bool overlaps(const FrequencyRange& other) const noexcept {
    if (empty() || other.empty()) return false;
    return lowMhz < other.highMhz && other.lowMhz < highMhz;
  }

  [[nodiscard]] constexpr bool contains(const FrequencyRange& other) const noexcept {
    if (other.empty()) return true;
    if (empty()) return false;
    return lowMhz <= other.lowMhz && other.highMhz <= highMhz;
  }

  friend constexpr bool operator==(const FrequencyRange&, const FrequencyRange&) noexcept = default;
};

}  // namespace wavelength_fabric
