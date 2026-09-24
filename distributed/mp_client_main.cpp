// wf_mp_client - a real independent client process used by the multiprocess
// proof. It connects over loopback TCP, optionally waits for a wall-clock start
// instant so that several processes attempt the same allocation genuinely
// simultaneously, and writes its typed result to a file so the orchestrator can
// read it without parsing interleaved output.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <wavelength_fabric/protocol.hpp>
#include <wavelength_fabric/runtime.hpp>
#include <wavelength_fabric/text.hpp>

namespace {

struct Options {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::string out;
  std::uint64_t requestId{1};
  std::uint64_t requestGeneration{1};
  std::uint64_t owner{1};
  std::uint64_t ownerGeneration{1};
  std::uint64_t domainGeneration{1};
  std::uint64_t grid{1};
  std::uint64_t gridGeneration{1};
  std::uint32_t slots{1};
  std::int64_t startAt{0};
  std::int64_t giveUpAt{0};
  std::int64_t requestedAt{0};
  std::int64_t leaseSeconds{600};
  std::int64_t windowLow{-1};
  std::int64_t windowHigh{-1};
  std::uint64_t eligibilityGeneration{1};
  std::uint64_t reservationGeneration{1};
  std::uint64_t fenceEpoch{0};
  std::uint64_t fenceIncarnation{0};
  std::vector<std::uint64_t> domains;
  bool abandon{false};
};

[[nodiscard]] bool parseSigned(const char* text, std::int64_t& value) {
  if (text == nullptr || *text == '\0') return false;
  bool negative = false;
  const char* cursor = text;
  if (*cursor == '-') {
    negative = true;
    ++cursor;
  }
  if (*cursor == '\0') return false;
  std::uint64_t result = 0;
  for (; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(*cursor - '0');
    if (result > (0x7FFF'FFFF'FFFF'FFFFull - digit) / 10ull) return false;
    result = result * 10ull + digit;
  }
  value = negative ? -static_cast<std::int64_t>(result) : static_cast<std::int64_t>(result);
  return true;
}

// Parses the full unsigned 64-bit range: controller incarnations are hashed
// values that routinely exceed the signed range, and a CLI that cannot express
// them could not name the fence it wants to test.
[[nodiscard]] bool parseUnsigned(const char* text, std::uint64_t& value) {
  if (text == nullptr || *text == '\0') return false;
  std::uint64_t result = 0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(*cursor - '0');
    if (result > (0xFFFF'FFFF'FFFF'FFFFull - digit) / 10ull) return false;
    result = result * 10ull + digit;
  }
  value = result;
  return true;
}

[[nodiscard]] bool parseDomains(const char* text, std::vector<std::uint64_t>& domains) {
  std::string current;
  const std::string value = text;
  for (std::size_t index = 0; index <= value.size(); ++index) {
    if (index == value.size() || value[index] == ',') {
      if (current.empty()) return false;
      std::uint64_t parsed = 0;
      if (!parseUnsigned(current.c_str(), parsed)) return false;
      domains.push_back(parsed);
      current.clear();
    } else {
      current.push_back(value[index]);
    }
  }
  return !domains.empty();
}

void writeResult(const std::string& path, const std::string& text) {
  std::FILE* handle = std::fopen(path.c_str(), "wb");
  if (handle == nullptr) return;
  std::fwrite(text.data(), 1, text.size(), handle);
  std::fclose(handle);
}

[[nodiscard]] std::string sanitise(std::string value) {
  for (char& character : value) {
    if (character == '\n' || character == '\r') character = ' ';
  }
  if (value.size() > 400) value.resize(400);
  return value;
}

[[nodiscard]] bool parseOptions(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto next = [&](const char*& out) -> bool {
      if (index + 1 >= argc) return false;
      out = argv[++index];
      return true;
    };
    const char* value = nullptr;
    std::uint64_t parsed = 0;
    std::int64_t signedValue = 0;
    if (argument == "--host") {
      if (!next(value)) return false;
      options.host = value;
    } else if (argument == "--port") {
      if (!next(value) || !parseUnsigned(value, parsed) || parsed > 65535ull) return false;
      options.port = static_cast<std::uint16_t>(parsed);
    } else if (argument == "--out") {
      if (!next(value)) return false;
      options.out = value;
    } else if (argument == "--request-id") {
      if (!next(value) || !parseUnsigned(value, parsed)) return false;
      options.requestId = parsed;
    } else if (argument == "--request-generation") {
      if (!next(value) || !parseUnsigned(value, parsed)) return false;
      options.requestGeneration = parsed;
    } else if (argument == "--owner") {
      if (!next(value) || !parseUnsigned(value, parsed)) return false;
      options.owner = parsed;
    } else if (argument == "--domain-generation") {
      if (!next(value) || !parseUnsigned(value, parsed)) return false;
      options.domainGeneration = parsed;
    } else if (argument == "--grid") {
      if (!next(value) || !parseUnsigned(value, parsed)) return false;
      options.grid = parsed;
    } else if (argument == "--grid-generation") {
      if (!next(value) || !parseUnsigned(value, parsed)) return false;
      options.gridGeneration = parsed;
    } else if (argument == "--slots") {
      if (!next(value) || !parseUnsigned(value, parsed) || parsed == 0) return false;
      options.slots = static_cast<std::uint32_t>(parsed);
    } else if (argument == "--start-at") {
      if (!next(value) || !parseSigned(value, signedValue)) return false;
      options.startAt = signedValue;
    } else if (argument == "--give-up-at") {
      if (!next(value) || !parseSigned(value, signedValue)) return false;
      options.giveUpAt = signedValue;
    } else if (argument == "--requested-at") {
      if (!next(value) || !parseSigned(value, signedValue)) return false;
      options.requestedAt = signedValue;
    } else if (argument == "--lease-seconds") {
      if (!next(value) || !parseSigned(value, signedValue) || signedValue <= 0) return false;
      options.leaseSeconds = signedValue;
    } else if (argument == "--window-low") {
      if (!next(value) || !parseSigned(value, signedValue)) return false;
      options.windowLow = signedValue;
    } else if (argument == "--window-high") {
      if (!next(value) || !parseSigned(value, signedValue)) return false;
      options.windowHigh = signedValue;
    } else if (argument == "--eligibility-generation") {
      if (!next(value) || !parseUnsigned(value, parsed)) return false;
      options.eligibilityGeneration = parsed;
    } else if (argument == "--reservation-generation") {
      if (!next(value) || !parseUnsigned(value, parsed)) return false;
      options.reservationGeneration = parsed;
    } else if (argument == "--fence-epoch") {
      if (!next(value) || !parseUnsigned(value, parsed)) return false;
      options.fenceEpoch = parsed;
    } else if (argument == "--fence-incarnation") {
      if (!next(value) || !parseUnsigned(value, parsed)) return false;
      options.fenceIncarnation = parsed;
    } else if (argument == "--domains") {
      if (!next(value) || !parseDomains(value, options.domains)) return false;
    } else if (argument == "--abandon") {
      options.abandon = true;
    } else {
      std::cerr << "unrecognised argument: " << argument << "\n";
      return false;
    }
  }
  return options.port != 0 && !options.out.empty() && !options.domains.empty();
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parseOptions(argc, argv, options)) {
    std::cerr << "usage: wf_mp_client --port <port> --out <path> --domains <a,b> [options]\n";
    return 2;
  }

  wavelength_fabric::SpectrumClient client;
  const wavelength_fabric::Status connected =
      client.connect(options.host, options.port, wavelength_fabric::kMaxFramePayloadBytes);
  if (!connected.ok()) {
    writeResult(options.out, "transport=connect-failed reason=" + sanitise(connected.message) + "\n");
    return 3;
  }

  wavelength_fabric::ServerDescription description;
  const wavelength_fabric::Status described = client.describe(description);
  if (!described.ok()) {
    writeResult(options.out, "transport=describe-failed reason=" + sanitise(described.message) + "\n");
    return 3;
  }

  wavelength_fabric::ControllerFence fence = description.fence;
  if (options.fenceEpoch != 0) fence.epoch = wavelength_fabric::ControllerEpoch(options.fenceEpoch);
  if (options.fenceIncarnation != 0) {
    fence.incarnation = wavelength_fabric::ControllerIncarnation(options.fenceIncarnation);
  }

  wavelength_fabric::SpectrumRequest request;
  request.requestId = wavelength_fabric::AllocationRequestId(options.requestId);
  request.requestGeneration =
      wavelength_fabric::AllocationRequestGeneration(options.requestGeneration);
  request.owner = wavelength_fabric::OwnerId(options.owner);
  request.ownerGeneration = wavelength_fabric::OwnerGeneration(options.ownerGeneration);
  for (const std::uint64_t domain : options.domains) {
    request.domains.push_back(wavelength_fabric::SpectrumDomainId(domain));
    request.domainGenerations.push_back(
        wavelength_fabric::SpectrumDomainGeneration(options.domainGeneration));
  }
  request.grid = wavelength_fabric::ChannelGridId(options.grid);
  request.gridGeneration = wavelength_fabric::GridGeneration(options.gridGeneration);
  request.slots = options.slots;
  request.contiguity = options.slots > 1 ? wavelength_fabric::ContiguityRequirement::Required
                                         : wavelength_fabric::ContiguityRequirement::Unspecified;
  request.continuity = request.domains.size() > 1
                           ? wavelength_fabric::ContinuityRequirement::Required
                           : wavelength_fabric::ContinuityRequirement::Unspecified;
  request.leaseDuration = wavelength_fabric::Duration::seconds(options.leaseSeconds);
  request.requestedAt = wavelength_fabric::Instant::fromNanos(options.requestedAt);
  request.policyGeneration = wavelength_fabric::PolicyGeneration(1);
  request.priorityGeneration = wavelength_fabric::PriorityGeneration(1);
  if (options.windowLow >= 0 && options.windowHigh > options.windowLow) {
    request.frequencyWindows.push_back(
        wavelength_fabric::FrequencyRange{options.windowLow, options.windowHigh});
  }
  request.eligibilityAuthority.generation =
      wavelength_fabric::EligibilityAuthorityGeneration(options.eligibilityGeneration);
  request.eligibilityAuthority.fence = fence;
  request.reservationAuthority.generation =
      wavelength_fabric::ReservationAuthorityGeneration(options.reservationGeneration);
  request.reservationAuthority.fence = fence;

  if (options.abandon) {
    // Send the allocation and terminate immediately without reading the reply:
    // this is the deterministic form of "the process died after the commit but
    // before the acknowledgement reached the caller". A raw socket is used so
    // nothing can read or buffer the reply on our behalf.
    const wavelength_fabric::Status socketSubsystem = wavelength_fabric::initSockets();
    if (!socketSubsystem.ok()) {
      writeResult(options.out, "transport=sockets-failed reason=" + sanitise(socketSubsystem.message) + "\n");
      return 3;
    }
    wavelength_fabric::Socket raw;
    const wavelength_fabric::Status rawConnect =
        wavelength_fabric::connectTcp(options.host, options.port, raw);
    if (!rawConnect.ok()) {
      writeResult(options.out,
                  "transport=connect-failed reason=" + sanitise(rawConnect.message) + "\n");
      return 3;
    }
    wavelength_fabric::Encoder encoder;
    wavelength_fabric::encode(encoder, request);
    wavelength_fabric::Frame frame;
    frame.op = wavelength_fabric::OpCode::Allocate;
    frame.requestId = 1;
    frame.payload = encoder.take();
    const wavelength_fabric::Status written =
        wavelength_fabric::writeFrame(raw, frame, wavelength_fabric::kMaxFramePayloadBytes);
    if (!written.ok()) {
      writeResult(options.out,
                  "transport=write-failed reason=" + sanitise(written.message) + "\n");
      return 3;
    }
    std::cout << "ABANDONED\n";
    std::cout.flush();
    std::_Exit(7);
  }

  const wavelength_fabric::Status synchronised =
      wavelength_fabric::waitUntil(options.startAt, options.giveUpAt);
  if (!synchronised.ok()) {
    writeResult(options.out, "transport=sync-failed reason=" + sanitise(synchronised.message) + "\n");
    return 3;
  }

  wavelength_fabric::AllocationDecision decision;
  const wavelength_fabric::Status status = client.allocate(request, decision);

  std::string text;
  text += "transport=" + std::string(wavelength_fabric::toToken(status.code)) + "\n";
  text += "outcome=" + std::string(wavelength_fabric::toToken(decision.outcome)) + "\n";
  text += "reservation=" + std::to_string(decision.reservation.raw()) + "\n";
  text += "generation=" + std::to_string(decision.generation.raw()) + "\n";
  text += "request=" + std::to_string(decision.explanation.reservation.raw()) + "\n";
  text += "slots_first=" + std::to_string(decision.explanation.selected.slots.first) + "\n";
  text += "slots_count=" + std::to_string(decision.explanation.selected.slots.count) + "\n";
  text += "epoch=" + std::to_string(description.fence.epoch.raw()) + "\n";
  text += "incarnation=" + std::to_string(description.fence.incarnation.raw()) + "\n";
  if (!decision.explanation.reasons.empty()) {
    text += "reason=" + sanitise(decision.explanation.reasons.front()) + "\n";
  }
  writeResult(options.out, text);
  return 0;
}
