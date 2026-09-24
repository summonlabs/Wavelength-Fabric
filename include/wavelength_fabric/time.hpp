#pragma once

#include <chrono>
#include <compare>
#include <cstdint>

// Abstract time for spectrum leasing.
//
// Instant is an integer nanosecond count relative to the Unix epoch as
// reported by the host system clock. It is deliberately an integer: lease
// arithmetic is exact, comparable across processes, and never uses floating
// point. Overflow saturates at the representable bound and is documented
// rather than undefined.

namespace wavelength_fabric {

namespace detail {

inline constexpr std::int64_t kTimeMax = 0x7FFF'FFFF'FFFF'FFFFll;
inline constexpr std::int64_t kTimeMin = -0x7FFF'FFFF'FFFF'FFFFll - 1;

[[nodiscard]] constexpr std::int64_t satAdd(std::int64_t a, std::int64_t b) noexcept {
  if (b > 0 && a > kTimeMax - b) return kTimeMax;
  if (b < 0 && a < kTimeMin - b) return kTimeMin;
  return a + b;
}

[[nodiscard]] constexpr std::int64_t satMul(std::int64_t a, std::int64_t b) noexcept {
  if (a == 0 || b == 0) return 0;
  const bool negative = (a < 0) != (b < 0);
  using U = std::uint64_t;
  const U ua = a < 0 ? (U{0} - static_cast<U>(a)) : static_cast<U>(a);
  const U ub = b < 0 ? (U{0} - static_cast<U>(b)) : static_cast<U>(b);
  const U limit = negative ? (U{1} << 63) : static_cast<U>(kTimeMax);
  if (ua > limit / ub) return negative ? kTimeMin : kTimeMax;
  const U magnitude = ua * ub;
  if (!negative) return static_cast<std::int64_t>(magnitude);
  if (magnitude == (U{1} << 63)) return kTimeMin;
  return -static_cast<std::int64_t>(magnitude);
}

}  // namespace detail

class Duration {
 public:
  constexpr Duration() noexcept = default;

  [[nodiscard]] static constexpr Duration nanos(std::int64_t value) noexcept {
    return Duration(value);
  }
  [[nodiscard]] static constexpr Duration micros(std::int64_t value) noexcept {
    return Duration(detail::satMul(value, 1000));
  }
  [[nodiscard]] static constexpr Duration millis(std::int64_t value) noexcept {
    return Duration(detail::satMul(value, 1'000'000));
  }
  [[nodiscard]] static constexpr Duration seconds(std::int64_t value) noexcept {
    return Duration(detail::satMul(value, 1'000'000'000));
  }
  [[nodiscard]] static constexpr Duration minutes(std::int64_t value) noexcept {
    return Duration(detail::satMul(value, 60'000'000'000ll));
  }
  [[nodiscard]] static constexpr Duration hours(std::int64_t value) noexcept {
    return Duration(detail::satMul(value, 3'600'000'000'000ll));
  }
  [[nodiscard]] static constexpr Duration days(std::int64_t value) noexcept {
    return Duration(detail::satMul(value, 86'400'000'000'000ll));
  }

  [[nodiscard]] static constexpr Duration max() noexcept { return Duration(detail::kTimeMax); }
  [[nodiscard]] static constexpr Duration zero() noexcept { return Duration(0); }

  [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return value_; }
  [[nodiscard]] constexpr bool isZero() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr bool isNegative() const noexcept { return value_ < 0; }
  [[nodiscard]] constexpr bool isPositive() const noexcept { return value_ > 0; }

  friend constexpr bool operator==(Duration, Duration) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(Duration, Duration) noexcept = default;

 private:
  constexpr explicit Duration(std::int64_t value) noexcept : value_(value) {}
  std::int64_t value_{0};
};

class Instant {
 public:
  constexpr Instant() noexcept = default;

  [[nodiscard]] static constexpr Instant fromNanos(std::int64_t value) noexcept {
    return Instant(value);
  }
  [[nodiscard]] static constexpr Instant fromMicros(std::int64_t value) noexcept {
    return Instant(detail::satMul(value, 1000));
  }
  [[nodiscard]] static constexpr Instant fromMillis(std::int64_t value) noexcept {
    return Instant(detail::satMul(value, 1'000'000));
  }
  [[nodiscard]] static constexpr Instant fromSeconds(std::int64_t value) noexcept {
    return Instant(detail::satMul(value, 1'000'000'000));
  }
  [[nodiscard]] static constexpr Instant max() noexcept { return Instant(detail::kTimeMax); }

  [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return value_; }
  [[nodiscard]] constexpr bool isZero() const noexcept { return value_ == 0; }

  friend constexpr bool operator==(Instant, Instant) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(Instant, Instant) noexcept = default;

  [[nodiscard]] constexpr Instant operator+(Duration d) const noexcept {
    return Instant(detail::satAdd(value_, d.nanos()));
  }
  [[nodiscard]] constexpr Instant operator-(Duration d) const noexcept {
    return Instant(detail::satAdd(value_, -d.nanos()));
  }
  [[nodiscard]] constexpr Duration operator-(Instant other) const noexcept {
    return Duration::nanos(detail::satAdd(value_, -other.value_));
  }

 private:
  constexpr explicit Instant(std::int64_t value) noexcept : value_(value) {}
  std::int64_t value_{0};
};

// Host wall-clock reading, in nanoseconds since the Unix epoch. Callers that
// require determinism supply explicit instants instead of calling this.
[[nodiscard]] Instant systemNow() noexcept;

}  // namespace wavelength_fabric
