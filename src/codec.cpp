#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "wavelength_fabric/serialization.hpp"

// Bounded binary codec shared by durable state and the transport protocol.
//
// Every enumerator encoded here is contiguous from zero, so an out-of-range tag
// is rejected with a single comparison. Slot counts, collection sizes and
// string lengths are bounded before anything is allocated or indexed.

namespace wavelength_fabric {

namespace {

constexpr std::size_t kMaxBufferBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxEncodedBytes = 16u * 1024u * 1024u;
constexpr std::uint32_t kMaxCollectionCount = 1u << 20;
constexpr std::uint32_t kMaxStatusMessageBytes = 4096;
constexpr std::uint32_t kMaxReasonCount = 256;
constexpr std::uint32_t kMaxConflictCount = 4096;
constexpr std::uint32_t kMaxDiagnosticCount = 4096;
constexpr std::uint32_t kMaxLabelBytes = 256;

constexpr std::array<std::uint32_t, 256> makeCrcTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1u) != 0u ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
    }
    table[index] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = makeCrcTable();

template <class Enum>
[[nodiscard]] bool decodeEnum(Decoder& in, Enum& value, std::uint8_t maxValue) {
  std::uint8_t raw = 0;
  if (!in.u8(raw)) return false;
  if (raw > maxValue) {
    in.fail();
    return false;
  }
  value = static_cast<Enum>(raw);
  return true;
}

template <class T, class EncodeFn>
void encodeVector(Encoder& out, const std::vector<T>& values, EncodeFn encodeOne) {
  if (values.size() > kMaxCollectionCount) {
    out.u32(0);
    return;
  }
  out.u32(static_cast<std::uint32_t>(values.size()));
  for (const T& value : values) encodeOne(out, value);
}

template <class T, class DecodeFn>
[[nodiscard]] bool decodeVector(Decoder& in, std::vector<T>& values, std::uint32_t maxCount,
                                DecodeFn decodeOne) {
  std::uint32_t count = 0;
  if (!in.u32(count)) return false;
  if (count > maxCount) {
    in.fail();
    return false;
  }
  values.clear();
  values.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    T value{};
    if (!decodeOne(in, value)) {
      values.clear();
      return false;
    }
    values.push_back(std::move(value));
  }
  return true;
}

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept {
  std::uint32_t crc = 0xFFFF'FFFFu;
  for (const std::uint8_t byte : data) {
    crc = kCrcTable[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFF'FFFFu;
}

void Encoder::raw(const void* data, std::size_t count) {
  if (overflowed_ || count == 0) return;
  if (count > kMaxBufferBytes - buffer_.size()) {
    overflowed_ = true;
    return;
  }
  const auto* first = static_cast<const std::uint8_t*>(data);
  buffer_.insert(buffer_.end(), first, first + count);
}

void Encoder::u16(std::uint16_t value) {
  u8(static_cast<std::uint8_t>(value & 0xFFu));
  u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void Encoder::u32(std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void Encoder::u64(std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void Encoder::string(const std::string& value) {
  if (value.size() > kMaxEncodedStringBytes) {
    overflowed_ = true;
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  raw(value.data(), value.size());
}

void Encoder::bytes(std::span<const std::uint8_t> value) {
  if (value.size() > kMaxEncodedBytes) {
    overflowed_ = true;
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  raw(value.data(), value.size());
}

bool Decoder::u8(std::uint8_t& value) {
  if (!ok_) return false;
  if (offset_ >= data_.size()) {
    ok_ = false;
    return false;
  }
  value = data_[offset_];
  offset_ += 1;
  return true;
}

bool Decoder::u16(std::uint16_t& value) {
  std::uint8_t low = 0;
  std::uint8_t high = 0;
  if (!u8(low) || !u8(high)) return false;
  value = static_cast<std::uint16_t>(static_cast<std::uint16_t>(low) |
                                     static_cast<std::uint16_t>(high) << 8);
  return true;
}

bool Decoder::u32(std::uint32_t& value) {
  std::uint32_t result = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    std::uint8_t byte = 0;
    if (!u8(byte)) return false;
    result |= static_cast<std::uint32_t>(byte) << shift;
  }
  value = result;
  return true;
}

bool Decoder::u64(std::uint64_t& value) {
  std::uint64_t result = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    std::uint8_t byte = 0;
    if (!u8(byte)) return false;
    result |= static_cast<std::uint64_t>(byte) << shift;
  }
  value = result;
  return true;
}

bool Decoder::i64(std::int64_t& value) {
  std::uint64_t raw = 0;
  if (!u64(raw)) return false;
  value = static_cast<std::int64_t>(raw);
  return true;
}

bool Decoder::boolean(bool& value) {
  std::uint8_t raw = 0;
  if (!u8(raw)) return false;
  if (raw > 1) {
    ok_ = false;
    return false;
  }
  value = raw == 1;
  return true;
}

bool Decoder::string(std::string& value) {
  std::uint32_t length = 0;
  if (!u32(length)) return false;
  if (length > kMaxEncodedStringBytes || length > remaining()) {
    ok_ = false;
    return false;
  }
  value.assign(reinterpret_cast<const char*>(data_.data() + offset_), length);
  offset_ += length;
  return true;
}

bool Decoder::bytes(std::vector<std::uint8_t>& value, std::size_t maxBytes) {
  std::uint32_t length = 0;
  if (!u32(length)) return false;
  if (length > maxBytes || length > remaining()) {
    ok_ = false;
    return false;
  }
  value.assign(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
               data_.begin() + static_cast<std::ptrdiff_t>(offset_ + length));
  offset_ += length;
  return true;
}

void encode(Encoder& out, const SlotRange& value) {
  out.u32(value.first);
  out.u32(value.count);
}

bool decode(Decoder& in, SlotRange& value) {
  std::uint32_t first = 0;
  std::uint32_t count = 0;
  if (!in.u32(first) || !in.u32(count)) return false;
  if (count > kMaxGridSlots) {
    in.fail();
    return false;
  }
  if (count > 0 && first > 0xFFFF'FFFFu - count) {
    in.fail();
    return false;
  }
  value.first = first;
  value.count = count;
  return true;
}

void encode(Encoder& out, const FrequencyRange& value) {
  out.i64(value.lowMhz);
  out.i64(value.highMhz);
}

bool decode(Decoder& in, FrequencyRange& value) {
  std::int64_t low = 0;
  std::int64_t high = 0;
  if (!in.i64(low) || !in.i64(high)) return false;
  if (low < 0 || high < low || high > kMaxFrequencyMhz) {
    in.fail();
    return false;
  }
  value.lowMhz = low;
  value.highMhz = high;
  return true;
}

void encode(Encoder& out, const ControllerFence& value) {
  out.u64(value.epoch.raw());
  out.u64(value.incarnation.raw());
}

bool decode(Decoder& in, ControllerFence& value) {
  std::uint64_t epoch = 0;
  std::uint64_t incarnation = 0;
  if (!in.u64(epoch) || !in.u64(incarnation)) return false;
  if (epoch > kMaxFenceValue || incarnation > kMaxFenceValue) {
    in.fail();
    return false;
  }
  value.epoch = ControllerEpoch(epoch);
  value.incarnation = ControllerIncarnation(incarnation);
  return true;
}

void encode(Encoder& out, const AuthorityState& value) {
  out.u64(value.eligibilityGeneration.raw());
  out.u64(value.reservationGeneration.raw());
  out.u64(value.activationGeneration.raw());
  out.u64(value.releaseGeneration.raw());
  encode(out, value.fence);
}

bool decode(Decoder& in, AuthorityState& value) {
  std::uint64_t eligibility = 0;
  std::uint64_t reservation = 0;
  std::uint64_t activation = 0;
  std::uint64_t release = 0;
  ControllerFence fence;
  if (!in.u64(eligibility) || !in.u64(reservation) || !in.u64(activation) || !in.u64(release) ||
      !decode(in, fence)) {
    return false;
  }
  value.eligibilityGeneration = EligibilityAuthorityGeneration(eligibility);
  value.reservationGeneration = ReservationAuthorityGeneration(reservation);
  value.activationGeneration = ActivationAuthorityGeneration(activation);
  value.releaseGeneration = ReleaseAuthorityGeneration(release);
  value.fence = fence;
  return true;
}

void encode(Encoder& out, const Status& value) {
  out.u8(static_cast<std::uint8_t>(value.code));
  out.string(value.message);
}

bool decode(Decoder& in, Status& value) {
  StatusCode code = StatusCode::Ok;
  if (!decodeEnum(in, code, 20)) return false;
  std::string message;
  if (!in.string(message)) return false;
  if (message.size() > kMaxStatusMessageBytes) {
    in.fail();
    return false;
  }
  value.code = code;
  value.message = std::move(message);
  return true;
}

namespace {

template <class Token>
void encodeAuthority(Encoder& out, const Token& token) {
  out.u64(token.generation.raw());
  encode(out, token.fence);
}

template <class Token, class Generation>
[[nodiscard]] bool decodeAuthority(Decoder& in, Token& token) {
  std::uint64_t generation = 0;
  ControllerFence fence;
  if (!in.u64(generation) || !decode(in, fence)) return false;
  if (generation > kMaxFenceValue) {
    in.fail();
    return false;
  }
  token.generation = Generation(generation);
  token.fence = fence;
  return true;
}

}  // namespace

void encode(Encoder& out, const EligibilityAuthority& value) { encodeAuthority(out, value); }
bool decode(Decoder& in, EligibilityAuthority& value) {
  return decodeAuthority<EligibilityAuthority, EligibilityAuthorityGeneration>(in, value);
}
void encode(Encoder& out, const ReservationAuthority& value) { encodeAuthority(out, value); }
bool decode(Decoder& in, ReservationAuthority& value) {
  return decodeAuthority<ReservationAuthority, ReservationAuthorityGeneration>(in, value);
}
void encode(Encoder& out, const ActivationAuthority& value) { encodeAuthority(out, value); }
bool decode(Decoder& in, ActivationAuthority& value) {
  return decodeAuthority<ActivationAuthority, ActivationAuthorityGeneration>(in, value);
}
void encode(Encoder& out, const ReleaseAuthority& value) { encodeAuthority(out, value); }
bool decode(Decoder& in, ReleaseAuthority& value) {
  return decodeAuthority<ReleaseAuthority, ReleaseAuthorityGeneration>(in, value);
}

void encode(Encoder& out, const ChannelGrid& value) {
  out.u64(value.id.raw());
  out.u64(value.generation.raw());
  out.u8(static_cast<std::uint8_t>(value.kind));
  out.i64(value.anchorMhz);
  out.i64(value.slotWidthMhz);
  out.u32(value.slotCount);
  out.u32(value.minSlotsPerChannel);
  out.u32(value.maxSlotsPerChannel);
  out.string(value.label);
}

bool decode(Decoder& in, ChannelGrid& value) {
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  GridKind kind = GridKind::Unknown;
  std::int64_t anchor = 0;
  std::int64_t width = 0;
  std::uint32_t slotCount = 0;
  std::uint32_t minSlots = 0;
  std::uint32_t maxSlots = 0;
  std::string label;
  if (!in.u64(id) || !in.u64(generation) || !decodeEnum(in, kind, 2) || !in.i64(anchor) ||
      !in.i64(width) || !in.u32(slotCount) || !in.u32(minSlots) || !in.u32(maxSlots) ||
      !in.string(label)) {
    return false;
  }
  if (anchor < 0 || width < 0 || slotCount > kMaxGridSlots || minSlots > kMaxSlotsPerChannel ||
      maxSlots > kMaxSlotsPerChannel || label.size() > kMaxLabelBytes) {
    in.fail();
    return false;
  }
  value.id = ChannelGridId(id);
  value.generation = GridGeneration(generation);
  value.kind = kind;
  value.anchorMhz = anchor;
  value.slotWidthMhz = width;
  value.slotCount = slotCount;
  value.minSlotsPerChannel = minSlots;
  value.maxSlotsPerChannel = maxSlots;
  value.label = std::move(label);
  return true;
}

void encode(Encoder& out, const Span& value) {
  out.u64(value.id.raw());
  out.u64(value.generation.raw());
  out.string(value.label);
}

bool decode(Decoder& in, Span& value) {
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  std::string label;
  if (!in.u64(id) || !in.u64(generation) || !in.string(label)) return false;
  if (label.size() > kMaxLabelBytes) {
    in.fail();
    return false;
  }
  value.id = SpanId(id);
  value.generation = SpanGeneration(generation);
  value.label = std::move(label);
  return true;
}

void encode(Encoder& out, const OpticalPort& value) {
  out.u64(value.id.raw());
  out.u64(value.generation.raw());
  out.string(value.label);
}

bool decode(Decoder& in, OpticalPort& value) {
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  std::string label;
  if (!in.u64(id) || !in.u64(generation) || !in.string(label)) return false;
  if (label.size() > kMaxLabelBytes) {
    in.fail();
    return false;
  }
  value.id = PortId(id);
  value.generation = PortGeneration(generation);
  value.label = std::move(label);
  return true;
}

void encode(Encoder& out, const SpectrumDomain& value) {
  out.u64(value.id.raw());
  out.u64(value.generation.raw());
  out.u8(static_cast<std::uint8_t>(value.klass));
  out.u64(value.grid.raw());
  out.u64(value.gridGeneration.raw());
  out.u64(value.span.raw());
  out.u64(value.spanGeneration.raw());
  out.u64(value.portA.raw());
  out.u64(value.portAGeneration.raw());
  out.u64(value.portB.raw());
  out.u64(value.portBGeneration.raw());
  out.boolean(value.requiresContiguity);
  out.string(value.label);
}

bool decode(Decoder& in, SpectrumDomain& value) {
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  ResourceClass klass = ResourceClass::Unknown;
  std::uint64_t grid = 0;
  std::uint64_t gridGeneration = 0;
  std::uint64_t span = 0;
  std::uint64_t spanGeneration = 0;
  std::uint64_t portA = 0;
  std::uint64_t portAGeneration = 0;
  std::uint64_t portB = 0;
  std::uint64_t portBGeneration = 0;
  bool requiresContiguity = true;
  std::string label;
  if (!in.u64(id) || !in.u64(generation) || !decodeEnum(in, klass, 5) || !in.u64(grid) ||
      !in.u64(gridGeneration) || !in.u64(span) || !in.u64(spanGeneration) || !in.u64(portA) ||
      !in.u64(portAGeneration) || !in.u64(portB) || !in.u64(portBGeneration) ||
      !in.boolean(requiresContiguity) || !in.string(label)) {
    return false;
  }
  if (label.size() > kMaxLabelBytes) {
    in.fail();
    return false;
  }
  value.id = SpectrumDomainId(id);
  value.generation = SpectrumDomainGeneration(generation);
  value.klass = klass;
  value.grid = ChannelGridId(grid);
  value.gridGeneration = GridGeneration(gridGeneration);
  value.span = SpanId(span);
  value.spanGeneration = SpanGeneration(spanGeneration);
  value.portA = PortId(portA);
  value.portAGeneration = PortGeneration(portAGeneration);
  value.portB = PortId(portB);
  value.portBGeneration = PortGeneration(portBGeneration);
  value.requiresContiguity = requiresContiguity;
  value.label = std::move(label);
  return true;
}

namespace {

template <class Strong>
[[nodiscard]] bool decodeStrong(Decoder& in, Strong& value) {
  std::uint64_t raw = 0;
  if (!in.u64(raw)) return false;
  value = Strong(raw);
  return true;
}
template <class Id>
[[nodiscard]] bool decodeId(Decoder& in, Id& value) {
  std::uint64_t raw = 0;
  if (!in.u64(raw)) return false;
  value = Id(raw);
  return true;
}

[[nodiscard]] bool decodeString(Decoder& in, std::string& value) { return in.string(value); }

}  // namespace

void encode(Encoder& out, const ExclusionDomain& value) {
  out.u64(value.id.raw());
  out.u64(value.generation.raw());
  out.string(value.label);
  encodeVector(out, value.members,
               [](Encoder& target, const SpectrumDomainId& member) { target.u64(member.raw()); });
  out.i64(value.guardBandMhz);
}

bool decode(Decoder& in, ExclusionDomain& value) {
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  std::string label;
  if (!in.u64(id) || !in.u64(generation) || !in.string(label)) return false;
  if (label.size() > kMaxLabelBytes) {
    in.fail();
    return false;
  }
  std::vector<SpectrumDomainId> members;
  std::int64_t guard = 0;
  if (!decodeVector(in, members, kMaxExclusionDomainMembers, decodeId<SpectrumDomainId>) ||
      !in.i64(guard)) {
    return false;
  }
  if (guard < 0 || guard > kMaxGuardBandMhz) {
    in.fail();
    return false;
  }
  value.id = ExclusionDomainId(id);
  value.generation = ExclusionDomainGeneration(generation);
  value.label = std::move(label);
  value.members = std::move(members);
  value.guardBandMhz = guard;
  return true;
}

void encode(Encoder& out, const CapabilityEvidence& value) {
  out.boolean(value.present);
  out.u64(value.digest);
  out.string(value.source);
}

bool decode(Decoder& in, CapabilityEvidence& value) {
  bool present = false;
  std::uint64_t digest = 0;
  std::string source;
  if (!in.boolean(present) || !in.u64(digest) || !in.string(source)) return false;
  if (source.size() > kMaxLabelBytes) {
    in.fail();
    return false;
  }
  value.present = present;
  value.digest = digest;
  value.source = std::move(source);
  return true;
}

void encode(Encoder& out, const SpectrumCapability& value) {
  out.u64(value.domain.raw());
  out.u64(value.domainGeneration.raw());
  out.u64(value.grid.raw());
  out.u64(value.gridGeneration.raw());
  out.u8(static_cast<std::uint8_t>(value.support));
  out.u32(value.firstAllocatableSlot);
  out.u32(value.allocatableSlots);
  out.i64(value.minTunableMhz);
  out.i64(value.maxTunableMhz);
  out.boolean(value.contiguityEnforced);
  out.boolean(value.conversionSupported);
  encode(out, value.conversionEvidence);
  encode(out, value.presenceEvidence);
  out.u64(value.generation.raw());
  out.u64(value.publisher.raw());
  encode(out, value.fence);
  out.i64(value.publishedAt.nanos());
  out.string(value.detail);
}

bool decode(Decoder& in, SpectrumCapability& value) {
  std::uint64_t domain = 0;
  std::uint64_t domainGeneration = 0;
  std::uint64_t grid = 0;
  std::uint64_t gridGeneration = 0;
  SpectrumSupport support = SpectrumSupport::Unknown;
  std::uint32_t firstAllocatable = 0;
  std::uint32_t allocatableSlots = 0;
  std::int64_t minTunable = 0;
  std::int64_t maxTunable = 0;
  bool contiguityEnforced = true;
  bool conversionSupported = false;
  CapabilityEvidence conversionEvidence;
  CapabilityEvidence presenceEvidence;
  std::uint64_t generation = 0;
  std::uint64_t publisher = 0;
  ControllerFence fence;
  std::int64_t publishedAt = 0;
  std::string detail;
  if (!in.u64(domain) || !in.u64(domainGeneration) || !in.u64(grid) || !in.u64(gridGeneration) ||
      !decodeEnum(in, support, 2) || !in.u32(firstAllocatable) || !in.u32(allocatableSlots) ||
      !in.i64(minTunable) || !in.i64(maxTunable) || !in.boolean(contiguityEnforced) ||
      !in.boolean(conversionSupported) || !decode(in, conversionEvidence) ||
      !decode(in, presenceEvidence) || !in.u64(generation) || !in.u64(publisher) ||
      !decode(in, fence) || !in.i64(publishedAt) || !in.string(detail)) {
    return false;
  }
  if (publishedAt < 0 || detail.size() > 4096 || firstAllocatable > kMaxGridSlots ||
      allocatableSlots > kMaxGridSlots) {
    in.fail();
    return false;
  }
  value.domain = SpectrumDomainId(domain);
  value.domainGeneration = SpectrumDomainGeneration(domainGeneration);
  value.grid = ChannelGridId(grid);
  value.gridGeneration = GridGeneration(gridGeneration);
  value.support = support;
  value.firstAllocatableSlot = firstAllocatable;
  value.allocatableSlots = allocatableSlots;
  value.minTunableMhz = minTunable;
  value.maxTunableMhz = maxTunable;
  value.contiguityEnforced = contiguityEnforced;
  value.conversionSupported = conversionSupported;
  value.conversionEvidence = conversionEvidence;
  value.presenceEvidence = presenceEvidence;
  value.generation = CapabilityGeneration(generation);
  value.publisher = ControllerId(publisher);
  value.fence = fence;
  value.publishedAt = Instant::fromNanos(publishedAt);
  value.detail = std::move(detail);
  return true;
}

void encode(Encoder& out, const SpectrumConstraints& value) {
  encodeVector(out, value.excludedSlots,
               [](Encoder& target, const SlotRange& range) { encode(target, range); });
  encodeVector(out, value.excludedFrequencies,
               [](Encoder& target, const FrequencyRange& range) { encode(target, range); });
  encodeVector(out, value.mustNotConflictWith,
               [](Encoder& target, const ReservationId& id) { target.u64(id.raw()); });
}

bool decode(Decoder& in, SpectrumConstraints& value) {
  std::vector<SlotRange> excludedSlots;
  std::vector<FrequencyRange> excludedFrequencies;
  std::vector<ReservationId> mustNotConflictWith;
  if (!decodeVector(in, excludedSlots, kMaxExcludedRanges,
                    [](Decoder& source, SlotRange& range) { return decode(source, range); }) ||
      !decodeVector(in, excludedFrequencies, kMaxExcludedRanges,
                    [](Decoder& source, FrequencyRange& range) { return decode(source, range); }) ||
      !decodeVector(in, mustNotConflictWith, kMaxExcludedRanges, decodeId<ReservationId>)) {
    return false;
  }
  value.excludedSlots = std::move(excludedSlots);
  value.excludedFrequencies = std::move(excludedFrequencies);
  value.mustNotConflictWith = std::move(mustNotConflictWith);
  return true;
}

void encode(Encoder& out, const SpectrumRequest& value) {
  out.u64(value.requestId.raw());
  out.u64(value.requestGeneration.raw());
  out.u64(value.owner.raw());
  out.u64(value.ownerGeneration.raw());
  encodeVector(out, value.domains,
               [](Encoder& target, const SpectrumDomainId& id) { target.u64(id.raw()); });
  encodeVector(out, value.domainGenerations,
               [](Encoder& target, const SpectrumDomainGeneration& id) { target.u64(id.raw()); });
  out.u64(value.grid.raw());
  out.u64(value.gridGeneration.raw());
  out.u32(value.slots);
  out.u8(static_cast<std::uint8_t>(value.contiguity));
  out.u8(static_cast<std::uint8_t>(value.continuity));
  encodeVector(out, value.frequencyWindows,
               [](Encoder& target, const FrequencyRange& range) { encode(target, range); });
  out.i64(value.guardBandMhz);
  out.i64(value.leaseDuration.nanos());
  out.u32(value.maxRenewals);
  out.i64(value.requestedAt.nanos());
  out.i64(value.notBefore.nanos());
  out.u64(value.policyGeneration.raw());
  out.u64(value.priorityGeneration.raw());
  encode(out, value.constraints);
  encode(out, value.eligibilityAuthority);
  encode(out, value.reservationAuthority);
}

bool decode(Decoder& in, SpectrumRequest& value) {
  SpectrumRequest decoded;
  std::uint8_t contiguity = 0;
  std::uint8_t continuity = 0;
  std::int64_t leaseNanos = 0;
  std::int64_t guardBand = 0;
  std::int64_t requestedAt = 0;
  std::int64_t notBefore = 0;
  std::vector<SpectrumDomainId> domains;
  std::vector<SpectrumDomainGeneration> domainGenerations;
  std::vector<FrequencyRange> frequencyWindows;
  if (!decodeStrong(in, decoded.requestId)) return false;
  if (!decodeStrong(in, decoded.requestGeneration) || !decodeStrong(in, decoded.owner) ||
      !decodeStrong(in, decoded.ownerGeneration) ||
      !decodeVector(in, domains, kMaxRequestDomains, decodeId<SpectrumDomainId>) ||
      !decodeVector(in, domainGenerations, kMaxRequestDomains,
                    decodeId<SpectrumDomainGeneration>) ||
      !decodeStrong(in, decoded.grid) || !decodeStrong(in, decoded.gridGeneration) || !in.u32(decoded.slots) ||
      !decodeEnum(in, contiguity, 2) || !decodeEnum(in, continuity, 2) ||
      !decodeVector(in, frequencyWindows, kMaxFrequencyWindows,
                    [](Decoder& source, FrequencyRange& range) { return decode(source, range); }) ||
      !in.i64(guardBand) || !in.i64(leaseNanos) || !in.u32(decoded.maxRenewals) ||
      !in.i64(requestedAt) || !in.i64(notBefore) || !decodeStrong(in, decoded.policyGeneration) ||
      !decodeStrong(in, decoded.priorityGeneration) || !decode(in, decoded.constraints) ||
      !decode(in, decoded.eligibilityAuthority) || !decode(in, decoded.reservationAuthority)) {
    return false;
  }
  if (decoded.slots > kMaxSlotsPerChannel || leaseNanos <= 0 || requestedAt < 0 || notBefore < 0 ||
      guardBand < 0 || guardBand > kMaxGuardBandMhz || domains.size() != domainGenerations.size()) {
    in.fail();
    return false;
  }
  decoded.contiguity = static_cast<ContiguityRequirement>(contiguity);
  decoded.continuity = static_cast<ContinuityRequirement>(continuity);
  decoded.domains = std::move(domains);
  decoded.domainGenerations = std::move(domainGenerations);
  decoded.frequencyWindows = std::move(frequencyWindows);
  decoded.guardBandMhz = guardBand;
  decoded.leaseDuration = Duration::nanos(leaseNanos);
  decoded.requestedAt = Instant::fromNanos(requestedAt);
  decoded.notBefore = Instant::fromNanos(notBefore);
  value = std::move(decoded);
  return true;
}

void encode(Encoder& out, const SpectrumCandidate& value) {
  out.u32(static_cast<std::uint32_t>(value.ordinal & 0xFFFF'FFFFu));
  out.u64(value.anchorDomain.raw());
  encode(out, value.slots);
  encode(out, value.frequency);
  out.u8(static_cast<std::uint8_t>(value.eligibility));
  encodeVector(out, value.conflicts,
               [](Encoder& target, const ReservationId& id) { target.u64(id.raw()); });
  encodeVector(out, value.perDomainSlots,
               [](Encoder& target, const SlotRange& range) { encode(target, range); });
  out.boolean(value.crossGrid);
  out.string(value.detail);
}

bool decode(Decoder& in, SpectrumCandidate& value) {
  SpectrumCandidate decoded;
  std::uint32_t ordinal = 0;
  std::uint64_t anchorDomain = 0;
  std::vector<ReservationId> conflicts;
  std::vector<SlotRange> perDomainSlots;
  if (!in.u32(ordinal) || !in.u64(anchorDomain) || !decode(in, decoded.slots) ||
      !decode(in, decoded.frequency) || !decodeEnum(in, decoded.eligibility, 14) ||
      !decodeVector(in, conflicts, kMaxConflictCount, decodeId<ReservationId>) ||
      !decodeVector(in, perDomainSlots, kMaxRequestDomains,
                    [](Decoder& source, SlotRange& range) { return decode(source, range); }) ||
      !in.boolean(decoded.crossGrid) || !in.string(decoded.detail)) {
    return false;
  }
  if (ordinal > kMaxCandidatesPerRequest || decoded.detail.size() > 4096) {
    in.fail();
    return false;
  }
  decoded.ordinal = ordinal;
  decoded.anchorDomain = SpectrumDomainId(anchorDomain);
  decoded.conflicts = std::move(conflicts);
  decoded.perDomainSlots = std::move(perDomainSlots);
  value = std::move(decoded);
  return true;
}

void encode(Encoder& out, const CandidateSet& value) {
  encode(out, value.status);
  encodeVector(out, value.candidates,
               [](Encoder& target, const SpectrumCandidate& candidate) {
                 encode(target, candidate);
               });
  out.u32(static_cast<std::uint32_t>(value.eligibleCount & 0xFFFF'FFFFu));
  out.u32(static_cast<std::uint32_t>(value.omitted & 0xFFFF'FFFFu));
  out.boolean(value.complete);
  out.string(value.summary);
}

bool decode(Decoder& in, CandidateSet& value) {
  CandidateSet decoded;
  std::uint32_t eligibleCount = 0;
  std::uint32_t omitted = 0;
  if (!decode(in, decoded.status) ||
      !decodeVector(in, decoded.candidates, kMaxCandidatesPerRequest,
                    [](Decoder& source, SpectrumCandidate& candidate) {
                      return decode(source, candidate);
                    }) ||
      !in.u32(eligibleCount) || !in.u32(omitted) || !in.boolean(decoded.complete) ||
      !in.string(decoded.summary)) {
    return false;
  }
  if (decoded.summary.size() > kMaxStatusMessageBytes ||
      eligibleCount > decoded.candidates.size()) {
    in.fail();
    return false;
  }
  decoded.eligibleCount = eligibleCount;
  decoded.omitted = omitted;
  value = std::move(decoded);
  return true;
}

void encode(Encoder& out, const DecisionExplanation& value) {
  out.u8(static_cast<std::uint8_t>(value.outcome));
  out.u64(value.reservation.raw());
  out.u64(value.generation.raw());
  encode(out, value.fence);
  out.u32(static_cast<std::uint32_t>(value.candidatesEnumerated & 0xFFFF'FFFFu));
  out.u32(static_cast<std::uint32_t>(value.candidatesRejected & 0xFFFF'FFFFu));
  out.u32(static_cast<std::uint32_t>(value.candidatesOmitted & 0xFFFF'FFFFu));
  out.u32(static_cast<std::uint32_t>(value.candidateOrdinal & 0xFFFF'FFFFu));
  encode(out, value.selected);
  encodeVector(out, value.rejected, [](Encoder& target, const SpectrumCandidate& candidate) {
    encode(target, candidate);
  });
  encodeVector(out, value.conflicts,
               [](Encoder& target, const ReservationId& id) { target.u64(id.raw()); });
  encodeVector(out, value.reasons,
               [](Encoder& target, const std::string& reason) { target.string(reason); });
}

bool decode(Decoder& in, DecisionExplanation& value) {
  DecisionExplanation decoded;
  std::uint8_t outcome = 0;
  std::uint32_t enumerated = 0;
  std::uint32_t rejectedCount = 0;
  std::uint32_t omitted = 0;
  std::uint32_t ordinal = 0;
  std::vector<SpectrumCandidate> rejected;
  std::vector<ReservationId> conflicts;
  std::vector<std::string> reasons;
  if (!decodeEnum(in, outcome, 22) || !decodeStrong(in, decoded.reservation) ||
      !decodeStrong(in, decoded.generation) || !decode(in, decoded.fence) || !in.u32(enumerated) ||
      !in.u32(rejectedCount) || !in.u32(omitted) || !in.u32(ordinal) ||
      !decode(in, decoded.selected) ||
      !decodeVector(in, rejected, kMaxCandidatesPerRequest,
                    [](Decoder& source, SpectrumCandidate& candidate) {
                      return decode(source, candidate);
                    }) ||
      !decodeVector(in, conflicts, kMaxConflictCount, decodeId<ReservationId>) ||
      !decodeVector(in, reasons, kMaxReasonCount, decodeString)) {
    return false;
  }
  if (ordinal > kMaxCandidatesPerRequest) {
    in.fail();
    return false;
  }
  decoded.outcome = static_cast<AllocationOutcome>(outcome);
  decoded.candidatesEnumerated = enumerated;
  decoded.candidatesRejected = rejectedCount;
  decoded.candidatesOmitted = omitted;
  decoded.candidateOrdinal = ordinal;
  decoded.rejected = std::move(rejected);
  decoded.conflicts = std::move(conflicts);
  decoded.reasons = std::move(reasons);
  value = std::move(decoded);
  return true;
}

void encode(Encoder& out, const AllocationDecision& value) {
  encode(out, value.status);
  out.u8(static_cast<std::uint8_t>(value.outcome));
  out.u64(value.reservation.raw());
  out.u64(value.generation.raw());
  encode(out, value.explanation);
}

bool decode(Decoder& in, AllocationDecision& value) {
  AllocationDecision decoded;
  std::uint8_t outcome = 0;
  if (!decode(in, decoded.status) || !decodeEnum(in, outcome, 22) ||
      !decodeStrong(in, decoded.reservation) || !decodeStrong(in, decoded.generation) ||
      !decode(in, decoded.explanation)) {
    return false;
  }
  decoded.outcome = static_cast<AllocationOutcome>(outcome);
  value = std::move(decoded);
  return true;
}

void encode(Encoder& out, const ReclaimReport& value) {
  encode(out, value.status);
  out.i64(value.evaluatedAt.nanos());
  encode(out, value.fence);
  out.u32(static_cast<std::uint32_t>(value.scanned & 0xFFFF'FFFFu));
  out.u32(static_cast<std::uint32_t>(value.expired & 0xFFFF'FFFFu));
  encodeVector(out, value.reclaimed,
               [](Encoder& target, const ReservationId& id) { target.u64(id.raw()); });
  encodeVector(out, value.lapsed,
               [](Encoder& target, const ReservationId& id) { target.u64(id.raw()); });
}

bool decode(Decoder& in, ReclaimReport& value) {
  ReclaimReport decoded;
  std::int64_t evaluatedAt = 0;
  std::uint32_t scanned = 0;
  std::uint32_t expired = 0;
  std::vector<ReservationId> reclaimed;
  std::vector<ReservationId> lapsed;
  if (!decode(in, decoded.status) || !in.i64(evaluatedAt) || !decode(in, decoded.fence) ||
      !in.u32(scanned) || !in.u32(expired) ||
      !decodeVector(in, reclaimed, kMaxCollectionCount, decodeId<ReservationId>) ||
      !decodeVector(in, lapsed, kMaxCollectionCount, decodeId<ReservationId>)) {
    return false;
  }
  if (evaluatedAt < 0) {
    in.fail();
    return false;
  }
  decoded.evaluatedAt = Instant::fromNanos(evaluatedAt);
  decoded.scanned = scanned;
  decoded.expired = expired;
  decoded.reclaimed = std::move(reclaimed);
  decoded.lapsed = std::move(lapsed);
  value = std::move(decoded);
  return true;
}

void encode(Encoder& out, const RecoveryReport& value) {
  encode(out, value.status);
  out.boolean(value.recovered);
  out.string(value.source);
  encode(out, value.fence);
  out.u64(value.runtimeGeneration.raw());
  out.u64(value.recoveryGeneration.raw());
  out.u64(value.recordsRead);
  out.u64(value.recordsAccepted);
  out.u64(value.recordsRejected);
  out.u32(static_cast<std::uint32_t>(value.gridsRestored & 0xFFFF'FFFFu));
  out.u32(static_cast<std::uint32_t>(value.domainsRestored & 0xFFFF'FFFFu));
  out.u32(static_cast<std::uint32_t>(value.exclusionDomainsRestored & 0xFFFF'FFFFu));
  out.u32(static_cast<std::uint32_t>(value.capabilitiesRestored & 0xFFFF'FFFFu));
  out.u32(static_cast<std::uint32_t>(value.reservationsRestored & 0xFFFF'FFFFu));
  out.u32(static_cast<std::uint32_t>(value.auditsRestored & 0xFFFF'FFFFu));
  out.u32(static_cast<std::uint32_t>(value.demotedFromActive & 0xFFFF'FFFFu));
  encodeVector(out, value.diagnostics,
               [](Encoder& target, const std::string& text) { target.string(text); });
}

bool decode(Decoder& in, RecoveryReport& value) {
  RecoveryReport decoded;
  std::uint32_t counters[7] = {0, 0, 0, 0, 0, 0, 0};
  std::vector<std::string> diagnostics;
  if (!decode(in, decoded.status) || !in.boolean(decoded.recovered) || !in.string(decoded.source) ||
      !decode(in, decoded.fence) || !decodeStrong(in, decoded.runtimeGeneration) ||
      !decodeStrong(in, decoded.recoveryGeneration) || !in.u64(decoded.recordsRead) ||
      !in.u64(decoded.recordsAccepted) || !in.u64(decoded.recordsRejected)) {
    return false;
  }
  for (std::uint32_t& counter : counters) {
    if (!in.u32(counter)) return false;
  }
  if (!decodeVector(in, diagnostics, kMaxDiagnosticCount, decodeString)) return false;
  if (decoded.source.size() > kMaxStatusMessageBytes) {
    in.fail();
    return false;
  }
  decoded.gridsRestored = counters[0];
  decoded.domainsRestored = counters[1];
  decoded.exclusionDomainsRestored = counters[2];
  decoded.capabilitiesRestored = counters[3];
  decoded.reservationsRestored = counters[4];
  decoded.auditsRestored = counters[5];
  decoded.demotedFromActive = counters[6];
  decoded.diagnostics = std::move(diagnostics);
  value = std::move(decoded);
  return true;
}

void encode(Encoder& out, const Lease& value) {
  out.u64(value.generation.raw());
  out.i64(value.grantedAt.nanos());
  out.i64(value.expiresAt.nanos());
  out.u32(value.renewalCount);
  out.u32(value.maxRenewals);
}

bool decode(Decoder& in, Lease& value) {
  Lease decoded;
  std::int64_t grantedAt = 0;
  std::int64_t expiresAt = 0;
  if (!decodeStrong(in, decoded.generation) || !in.i64(grantedAt) || !in.i64(expiresAt) ||
      !in.u32(decoded.renewalCount) || !in.u32(decoded.maxRenewals)) {
    return false;
  }
  if (grantedAt < 0 || expiresAt < grantedAt) {
    in.fail();
    return false;
  }
  decoded.grantedAt = Instant::fromNanos(grantedAt);
  decoded.expiresAt = Instant::fromNanos(expiresAt);
  value = decoded;
  return true;
}

void encode(Encoder& out, const SpectrumReservation& value) {
  out.u64(value.id.raw());
  out.u64(value.generation.raw());
  out.u64(value.requestId.raw());
  out.u64(value.requestGeneration.raw());
  out.u64(value.owner.raw());
  out.u64(value.ownerGeneration.raw());
  out.u8(static_cast<std::uint8_t>(value.state));
  encodeVector(out, value.domains,
               [](Encoder& target, const SpectrumDomainId& id) { target.u64(id.raw()); });
  encodeVector(out, value.domainGenerations,
               [](Encoder& target, const SpectrumDomainGeneration& id) { target.u64(id.raw()); });
  out.u64(value.anchorDomain.raw());
  out.u64(value.grid.raw());
  out.u64(value.gridGeneration.raw());
  encode(out, value.slots);
  encode(out, value.frequency);
  encodeVector(out, value.perDomainSlots,
               [](Encoder& target, const SlotRange& range) { encode(target, range); });
  out.boolean(value.crossGrid);
  out.boolean(value.contiguityRequired);
  out.boolean(value.continuityRequired);
  out.i64(value.guardBandMhz);
  out.u64(value.exclusionDomain.raw());
  encode(out, value.lease);
  out.i64(value.createdAt.nanos());
  out.i64(value.updatedAt.nanos());
  out.i64(value.activatedAt.nanos());
  out.i64(value.deactivatedAt.nanos());
  out.i64(value.releasedAt.nanos());
  out.i64(value.reclaimedAt.nanos());
  out.u64(value.capabilityGeneration.raw());
  encode(out, value.commitFence);
  out.u64(value.lastOperation.raw());
  out.boolean(value.needsRevalidation);
  out.string(value.detail);
}

bool decode(Decoder& in, SpectrumReservation& value) {
  SpectrumReservation decoded;
  std::uint8_t state = 0;
  std::int64_t guardBand = 0;
  std::int64_t createdAt = 0;
  std::int64_t updatedAt = 0;
  std::int64_t activatedAt = 0;
  std::int64_t deactivatedAt = 0;
  std::int64_t releasedAt = 0;
  std::int64_t reclaimedAt = 0;
  std::vector<SpectrumDomainId> domains;
  std::vector<SpectrumDomainGeneration> domainGenerations;
  std::vector<SlotRange> perDomainSlots;
  if (!decodeStrong(in, decoded.id) || !decodeStrong(in, decoded.generation) || !decodeStrong(in, decoded.requestId) ||
      !decodeStrong(in, decoded.requestGeneration) || !decodeStrong(in, decoded.owner) ||
      !decodeStrong(in, decoded.ownerGeneration) || !decodeEnum(in, state, 17) ||
      !decodeVector(in, domains, kMaxRequestDomains, decodeId<SpectrumDomainId>) ||
      !decodeVector(in, domainGenerations, kMaxRequestDomains,
                    decodeId<SpectrumDomainGeneration>) ||
      !decodeStrong(in, decoded.anchorDomain) || !decodeStrong(in, decoded.grid) ||
      !decodeStrong(in, decoded.gridGeneration) || !decode(in, decoded.slots) ||
      !decode(in, decoded.frequency) ||
      !decodeVector(in, perDomainSlots, kMaxRequestDomains,
                    [](Decoder& source, SlotRange& range) { return decode(source, range); }) ||
      !in.boolean(decoded.crossGrid) || !in.boolean(decoded.contiguityRequired) ||
      !in.boolean(decoded.continuityRequired) || !in.i64(guardBand) ||
      !decodeStrong(in, decoded.exclusionDomain) || !decode(in, decoded.lease) || !in.i64(createdAt) ||
      !in.i64(updatedAt) || !in.i64(activatedAt) || !in.i64(deactivatedAt) ||
      !in.i64(releasedAt) || !in.i64(reclaimedAt) || !decodeStrong(in, decoded.capabilityGeneration) ||
      !decode(in, decoded.commitFence) || !decodeStrong(in, decoded.lastOperation) ||
      !in.boolean(decoded.needsRevalidation) || !in.string(decoded.detail)) {
    return false;
  }
  if (guardBand < 0 || guardBand > kMaxGuardBandMhz || createdAt < 0 || updatedAt < 0 ||
      activatedAt < 0 || deactivatedAt < 0 || releasedAt < 0 || reclaimedAt < 0 ||
      domains.size() != domainGenerations.size() || domains.size() != perDomainSlots.size() ||
      decoded.detail.size() > 4096) {
    in.fail();
    return false;
  }
  decoded.state = static_cast<ReservationState>(state);
  decoded.domains = std::move(domains);
  decoded.domainGenerations = std::move(domainGenerations);
  decoded.perDomainSlots = std::move(perDomainSlots);
  decoded.guardBandMhz = guardBand;
  decoded.createdAt = Instant::fromNanos(createdAt);
  decoded.updatedAt = Instant::fromNanos(updatedAt);
  decoded.activatedAt = Instant::fromNanos(activatedAt);
  decoded.deactivatedAt = Instant::fromNanos(deactivatedAt);
  decoded.releasedAt = Instant::fromNanos(releasedAt);
  decoded.reclaimedAt = Instant::fromNanos(reclaimedAt);
  value = std::move(decoded);
  return true;
}

void encode(Encoder& out, const SpectrumUsage& value) {
  out.u64(value.domain.raw());
  out.u64(value.domainGeneration.raw());
  out.u64(value.grid.raw());
  out.u64(value.gridGeneration.raw());
  out.u32(value.totalSlots);
  out.u32(value.allocatableSlots);
  out.u32(value.liveSlots);
  out.u32(value.activeSlots);
  out.u32(value.reservedSlots);
  out.u32(value.freeSlots);
  out.u32(value.lapsedSlots);
  out.u32(value.liveReservations);
  out.u32(value.activeReservations);
  out.u32(value.lapsedReservations);
  out.u32(value.reclaimedReservations);
  out.u32(value.releasedReservations);
  encodeVector(out, value.freeRuns,
               [](Encoder& target, const SlotRange& range) { encode(target, range); });
}

bool decode(Decoder& in, SpectrumUsage& value) {
  SpectrumUsage decoded;
  std::vector<SlotRange> freeRuns;
  if (!decodeStrong(in, decoded.domain) || !decodeStrong(in, decoded.domainGeneration) || !decodeStrong(in, decoded.grid) ||
      !decodeStrong(in, decoded.gridGeneration) || !in.u32(decoded.totalSlots) ||
      !in.u32(decoded.allocatableSlots) || !in.u32(decoded.liveSlots) ||
      !in.u32(decoded.activeSlots) || !in.u32(decoded.reservedSlots) ||
      !in.u32(decoded.freeSlots) || !in.u32(decoded.lapsedSlots) ||
      !in.u32(decoded.liveReservations) || !in.u32(decoded.activeReservations) ||
      !in.u32(decoded.lapsedReservations) || !in.u32(decoded.reclaimedReservations) ||
      !in.u32(decoded.releasedReservations) ||
      !decodeVector(in, freeRuns, kMaxGridSlots,
                    [](Decoder& source, SlotRange& range) { return decode(source, range); })) {
    return false;
  }
  decoded.freeRuns = std::move(freeRuns);
  value = std::move(decoded);
  return true;
}

void encode(Encoder& out, const AuditRecord& value) {
  out.u64(value.sequence.raw());
  out.i64(value.at.nanos());
  out.u8(static_cast<std::uint8_t>(value.kind));
  encode(out, value.fence);
  out.u8(static_cast<std::uint8_t>(value.outcome));
  out.u64(value.reservation.raw());
  out.u64(value.reservationGeneration.raw());
  out.u64(value.domain.raw());
  encode(out, value.slots);
  encode(out, value.frequency);
  out.string(value.detail);
}

bool decode(Decoder& in, AuditRecord& value) {
  AuditRecord decoded;
  std::int64_t at = 0;
  std::uint8_t kind = 0;
  std::uint8_t outcome = 0;
  if (!decodeStrong(in, decoded.sequence) || !in.i64(at) || !decodeEnum(in, kind, 24) ||
      !decode(in, decoded.fence) || !decodeEnum(in, outcome, 22) ||
      !decodeStrong(in, decoded.reservation) || !decodeStrong(in, decoded.reservationGeneration) ||
      !decodeStrong(in, decoded.domain) || !decode(in, decoded.slots) || !decode(in, decoded.frequency) ||
      !in.string(decoded.detail)) {
    return false;
  }
  if (at < 0 || decoded.detail.size() > 4096) {
    in.fail();
    return false;
  }
  decoded.at = Instant::fromNanos(at);
  decoded.kind = static_cast<AuditKind>(kind);
  decoded.outcome = static_cast<AllocationOutcome>(outcome);
  value = std::move(decoded);
  return true;
}

}  // namespace wavelength_fabric
