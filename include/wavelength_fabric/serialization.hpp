#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "wavelength_fabric/audit.hpp"
#include "wavelength_fabric/capability.hpp"
#include "wavelength_fabric/decision.hpp"
#include "wavelength_fabric/error.hpp"
#include "wavelength_fabric/request.hpp"
#include "wavelength_fabric/reservation.hpp"
#include "wavelength_fabric/resource.hpp"
#include "wavelength_fabric/version.hpp"

// Bounded binary codec shared by durable state and the transport protocol.
//
// Every length is written explicitly and validated on read against a bound
// before anything is allocated. A decoder that sees a bad tag, an unknown
// enum, a length beyond its bound, or a truncated buffer fails once and stays
// failed, so partial input can never be mistaken for a valid record.

namespace wavelength_fabric {

[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept;

class Encoder {
 public:
  void u8(std::uint8_t value) { raw(&value, 1); }
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
  void boolean(bool value) { u8(value ? 1u : 0u); }
  void string(const std::string& value);
  void bytes(std::span<const std::uint8_t> value);

  [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
  [[nodiscard]] const std::vector<std::uint8_t>& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(buffer_); }
  void reserve(std::size_t bytes) { buffer_.reserve(bytes); }

 private:
  void raw(const void* data, std::size_t count);
  std::vector<std::uint8_t> buffer_;
  bool overflowed_{false};
};

class Decoder {
 public:
  explicit Decoder(std::span<const std::uint8_t> data) noexcept : data_(data) {}

  bool u8(std::uint8_t& value);
  bool u16(std::uint16_t& value);
  bool u32(std::uint32_t& value);
  bool u64(std::uint64_t& value);
  bool i64(std::int64_t& value);
  bool boolean(bool& value);
  bool string(std::string& value);
  bool bytes(std::vector<std::uint8_t>& value, std::size_t maxBytes);

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] bool exhausted() const noexcept { return offset_ == data_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  void fail() noexcept { ok_ = false; }

 private:
  std::span<const std::uint8_t> data_;
  std::size_t offset_{0};
  bool ok_{true};
};

// ---------------------------------------------------------------------------
// Primitive codecs
// ---------------------------------------------------------------------------
void encode(Encoder& out, const SlotRange& value);
bool decode(Decoder& in, SlotRange& value);
void encode(Encoder& out, const FrequencyRange& value);
bool decode(Decoder& in, FrequencyRange& value);
void encode(Encoder& out, const ControllerFence& value);
bool decode(Decoder& in, ControllerFence& value);
void encode(Encoder& out, const AuthorityState& value);
bool decode(Decoder& in, AuthorityState& value);
void encode(Encoder& out, const Status& value);
bool decode(Decoder& in, Status& value);
void encode(Encoder& out, const EligibilityAuthority& value);
bool decode(Decoder& in, EligibilityAuthority& value);
void encode(Encoder& out, const ReservationAuthority& value);
bool decode(Decoder& in, ReservationAuthority& value);
void encode(Encoder& out, const ActivationAuthority& value);
bool decode(Decoder& in, ActivationAuthority& value);
void encode(Encoder& out, const ReleaseAuthority& value);
bool decode(Decoder& in, ReleaseAuthority& value);

// ---------------------------------------------------------------------------
// Domain codecs
// ---------------------------------------------------------------------------
void encode(Encoder& out, const ChannelGrid& value);
bool decode(Decoder& in, ChannelGrid& value);
void encode(Encoder& out, const Span& value);
bool decode(Decoder& in, Span& value);
void encode(Encoder& out, const OpticalPort& value);
bool decode(Decoder& in, OpticalPort& value);
void encode(Encoder& out, const SpectrumDomain& value);
bool decode(Decoder& in, SpectrumDomain& value);
void encode(Encoder& out, const ExclusionDomain& value);
bool decode(Decoder& in, ExclusionDomain& value);
void encode(Encoder& out, const CapabilityEvidence& value);
bool decode(Decoder& in, CapabilityEvidence& value);
void encode(Encoder& out, const SpectrumCapability& value);
bool decode(Decoder& in, SpectrumCapability& value);

// ---------------------------------------------------------------------------
// Request / decision codecs
// ---------------------------------------------------------------------------
void encode(Encoder& out, const SpectrumConstraints& value);
bool decode(Decoder& in, SpectrumConstraints& value);
void encode(Encoder& out, const SpectrumRequest& value);
bool decode(Decoder& in, SpectrumRequest& value);

void encode(Encoder& out, const SpectrumCandidate& value);
bool decode(Decoder& in, SpectrumCandidate& value);
void encode(Encoder& out, const CandidateSet& value);
bool decode(Decoder& in, CandidateSet& value);
void encode(Encoder& out, const DecisionExplanation& value);
bool decode(Decoder& in, DecisionExplanation& value);
void encode(Encoder& out, const AllocationDecision& value);
bool decode(Decoder& in, AllocationDecision& value);
void encode(Encoder& out, const ReclaimReport& value);
bool decode(Decoder& in, ReclaimReport& value);
void encode(Encoder& out, const RecoveryReport& value);
bool decode(Decoder& in, RecoveryReport& value);

// ---------------------------------------------------------------------------
// Reservation / audit codecs
// ---------------------------------------------------------------------------
void encode(Encoder& out, const Lease& value);
bool decode(Decoder& in, Lease& value);
void encode(Encoder& out, const SpectrumReservation& value);
bool decode(Decoder& in, SpectrumReservation& value);
void encode(Encoder& out, const SpectrumUsage& value);
bool decode(Decoder& in, SpectrumUsage& value);
void encode(Encoder& out, const AuditRecord& value);
bool decode(Decoder& in, AuditRecord& value);

}  // namespace wavelength_fabric
