#pragma once

#include <cstdint>
#include <string_view>

// Wavelength Fabric version and wire/format identifiers.
//
// The persistence format and the transport protocol carry their own explicit
// version numbers so that a reader always rejects a format it does not
// understand instead of guessing.

namespace wavelength_fabric {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";

// Version of the durable state container written by SpectrumRuntime::save().
inline constexpr std::uint16_t kPersistenceFormatVersion = 1;

// Version of the framed loopback transport protocol.
inline constexpr std::uint16_t kProtocolVersion = 2;

// Upper bound on any single encoded frame payload, in bytes.
inline constexpr std::uint32_t kMaxFramePayloadBytes = 4u * 1024u * 1024u;

// Upper bound on any single encoded string field, in bytes.
inline constexpr std::uint32_t kMaxEncodedStringBytes = 64u * 1024u;

}  // namespace wavelength_fabric
