// wfcli: inspection and control utility for the Wavelength Fabric runtime.
//
//   wfcli local  [--state <path>] <command> [arguments] [@<nanos>]
//   wfcli server --host <host> --port <port> <command> [arguments] [@<nanos>]
//
// "local" drives an in-process SpectrumRuntime. "server" drives a runtime that
// is owned by another process, through the framed loopback TCP client. Both
// modes print the same structured output, so a local inspection and a remote
// inspection of the same state are directly comparable.
//
// Every command takes an optional trailing @<nanos> instant. Commands that need
// a time use that instant, the command's own positional instant, or the host
// wall clock, in that order. The runtime never reads a clock itself, so passing
// an explicit instant makes every decision reproducible.
//
// Exit codes:
//   0  the command completed and the runtime accepted it
//   1  the runtime refused the command, or the operation failed
//   2  the command line was not usable
//
// Every decision prints the runtime's typed token. A refusal is never printed
// as success.

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "wavelength_fabric/protocol.hpp"
#include "wavelength_fabric/quantity.hpp"
#include "wavelength_fabric/runtime.hpp"
#include "wavelength_fabric/text.hpp"

namespace wf = wavelength_fabric;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitRefused = 1;
constexpr int kExitUsage = 2;

// A lease may not exceed the runtime's structural ceiling of 3650 days.
constexpr std::int64_t kLeaseSecondsCeiling = 3650ll * 24ll * 60ll * 60ll;

// Display bounds. They limit printed lines only; the runtime bounds the work.
constexpr std::size_t kMaxPrintedCandidates = 32;
constexpr std::size_t kMaxPrintedReservations = 256;
constexpr std::size_t kMaxPrintedAuditRecords = 256;

struct Command {
  std::string name;
  std::vector<std::string_view> args;
  std::optional<wf::Instant> at;
};

// ---------------------------------------------------------------------------
// Scalar parsing
//
// Every number that reaches the runtime is parsed here and range-checked with
// the checked helpers from quantity.hpp. A value that does not fit is a usage
// error, never a wrapped or truncated quantity.
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  for (;;) {
    const std::size_t position = text.find(separator, start);
    if (position == std::string_view::npos) {
      fields.push_back(text.substr(start));
      return fields;
    }
    fields.push_back(text.substr(start, position - start));
    start = position + 1;
  }
}

[[nodiscard]] bool parseI64(std::string_view text, std::int64_t& out) noexcept {
  if (text.empty()) return false;
  const char* const first = text.data();
  const char* const last = text.data() + text.size();
  const std::from_chars_result result = std::from_chars(first, last, out);
  return result.ec == std::errc{} && result.ptr == last;
}

[[nodiscard]] bool parseU64(std::string_view text, std::uint64_t& out) noexcept {
  if (text.empty()) return false;
  const char* const first = text.data();
  const char* const last = text.data() + text.size();
  const std::from_chars_result result = std::from_chars(first, last, out);
  return result.ec == std::errc{} && result.ptr == last;
}

[[nodiscard]] bool parseU32(std::string_view text, std::uint32_t& out) noexcept {
  std::int64_t value = 0;
  if (!parseI64(text, value)) return false;
  return wf::fitsU32(value, out);
}

[[nodiscard]] bool parseU16(std::string_view text, std::uint16_t& out) noexcept {
  std::int64_t value = 0;
  if (!parseI64(text, value)) return false;
  return wf::fitsU16(value, out);
}

// Identities and generations are unsigned and never zero.
[[nodiscard]] bool parseIdentity(std::string_view text, std::uint64_t& out) noexcept {
  return parseU64(text, out) && out != 0;
}

[[nodiscard]] bool parseInstant(std::string_view text, wf::Instant& out) noexcept {
  std::int64_t nanos = 0;
  if (!parseI64(text, nanos) || nanos < 0) return false;
  out = wf::Instant::fromNanos(nanos);
  return true;
}

[[nodiscard]] bool parseLease(std::string_view text, wf::Duration& out, std::string& error) {
  std::int64_t seconds = 0;
  if (!parseI64(text, seconds) || seconds <= 0) {
    error = "lease seconds must be a positive integer";
    return false;
  }
  if (seconds > kLeaseSecondsCeiling) {
    error = "lease seconds above " + std::to_string(kLeaseSecondsCeiling) +
            " exceed the runtime's lease ceiling";
    return false;
  }
  std::int64_t nanos = 0;
  if (wf::mulOverflow(seconds, 1'000'000'000ll, nanos)) {
    error = "lease seconds are not representable as a nanosecond duration";
    return false;
  }
  out = wf::Duration::nanos(nanos);
  return true;
}

[[nodiscard]] bool parseContiguity(std::string_view text, wf::ContiguityRequirement& out,
                                   std::string& error) {
  if (text == "required") {
    out = wf::ContiguityRequirement::Required;
    return true;
  }
  if (text == "not-required") {
    out = wf::ContiguityRequirement::NotRequired;
    return true;
  }
  if (text == "unspecified") {
    out = wf::ContiguityRequirement::Unspecified;
    return true;
  }
  error = "contiguity must be required, not-required or unspecified";
  return false;
}

[[nodiscard]] bool parseContinuity(std::string_view text, wf::ContinuityRequirement& out,
                                   std::string& error) {
  if (text == "required") {
    out = wf::ContinuityRequirement::Required;
    return true;
  }
  if (text == "not-required") {
    out = wf::ContinuityRequirement::NotRequired;
    return true;
  }
  if (text == "unspecified") {
    out = wf::ContinuityRequirement::Unspecified;
    return true;
  }
  error = "continuity must be required, not-required or unspecified";
  return false;
}

// ---------------------------------------------------------------------------
// Session: the single interface both modes implement
// ---------------------------------------------------------------------------

struct SessionInfo {
  bool remote{false};
  wf::ControllerFence fence{};
  wf::RuntimeGeneration runtimeGeneration{};
  wf::RecoveryGeneration recoveryGeneration{};
  wf::AuthorityState authority{};
  // False when the authority generations were reconstructed from the reported
  // controller fence instead of being read from the runtime itself.
  bool authorityKnown{true};
  std::size_t grids{0};
  std::size_t domains{0};
  std::size_t exclusionDomains{0};
  std::size_t capabilities{0};
  std::size_t reservations{0};
  std::size_t auditRecords{0};
  wf::AuditSequence lastAudit{};
  wf::RuntimeStats stats{};
};

class Session {
 public:
  Session() = default;
  virtual ~Session() = default;

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  [[nodiscard]] virtual bool remote() const noexcept = 0;
  // True when state outlives this process, so a reservation that is active now
  // comes back demoted when the next process recovers the image.
  [[nodiscard]] virtual bool durableState() const noexcept = 0;
  [[nodiscard]] virtual wf::Status info(SessionInfo& out) = 0;

  [[nodiscard]] virtual wf::Status registerGrid(const wf::ChannelGrid& grid) = 0;
  [[nodiscard]] virtual wf::Status registerDomain(const wf::SpectrumDomain& domain) = 0;
  [[nodiscard]] virtual wf::Status registerExclusionDomain(const wf::ExclusionDomain& exclusion) = 0;
  [[nodiscard]] virtual wf::Status publishCapability(const wf::SpectrumCapability& capability) = 0;

  // Resolves the generation of a registered domain. Only an in-process runtime
  // can answer this; the transport exposes no domain lookup.
  [[nodiscard]] virtual wf::Status lookupDomainGeneration(wf::SpectrumDomainId id,
                                                          wf::SpectrumDomainGeneration& out) = 0;

  [[nodiscard]] virtual wf::Status enumerate(const wf::SpectrumRequest& request,
                                             wf::CandidateSet& out) = 0;
  // Reports what a request would do without creating ownership.
  [[nodiscard]] virtual wf::Status explain(const wf::SpectrumRequest& request,
                                           wf::DecisionExplanation& out) = 0;
  [[nodiscard]] virtual wf::Status allocate(const wf::SpectrumRequest& request,
                                            wf::AllocationDecision& out) = 0;
  [[nodiscard]] virtual wf::Status renew(const wf::RenewArgs& args, wf::SpectrumReservation& out) = 0;
  [[nodiscard]] virtual wf::Status activate(const wf::ActivateArgs& args,
                                            wf::SpectrumReservation& out) = 0;
  [[nodiscard]] virtual wf::Status deactivate(const wf::ActivateArgs& args,
                                              wf::SpectrumReservation& out) = 0;
  [[nodiscard]] virtual wf::Status release(const wf::ReleaseArgs& args,
                                           wf::SpectrumReservation& out) = 0;
  [[nodiscard]] virtual wf::Status expireLeases(const wf::SweepArgs& args,
                                                wf::ReclaimReport& out) = 0;
  [[nodiscard]] virtual wf::Status reclaimExpired(const wf::SweepArgs& args,
                                                  wf::ReclaimReport& out) = 0;

  [[nodiscard]] virtual wf::Status reservation(wf::ReservationId id,
                                               wf::SpectrumReservation& out) = 0;
  [[nodiscard]] virtual wf::Status reservations(std::vector<wf::SpectrumReservation>& out) = 0;
  [[nodiscard]] virtual wf::Status usageAll(wf::Instant now, std::vector<wf::SpectrumUsage>& out) = 0;
  [[nodiscard]] virtual wf::Status audit(std::uint64_t since, std::uint32_t limit,
                                         std::vector<wf::AuditRecord>& out) = 0;

  [[nodiscard]] virtual wf::Status save() = 0;
  [[nodiscard]] virtual wf::Status reset() = 0;
  [[nodiscard]] virtual wf::Status recover(wf::RecoveryReport& out) = 0;
};

// ---------------------------------------------------------------------------
// Local session
// ---------------------------------------------------------------------------

class LocalSession final : public Session {
 public:
  explicit LocalSession(const std::string& statePath) : runtime_(makeConfig(statePath)) {}

  // Loads durable state when a state path is configured. A missing image is the
  // first run of a durable runtime, not a failure; a rejected image is.
  [[nodiscard]] wf::Status load(wf::RecoveryReport& report) {
    report = wf::RecoveryReport{};
    if (runtime_.config().statePath.empty()) return wf::okStatus();
    report = runtime_.recover();
    if (report.status.code == wf::StatusCode::NotFound) report.status = wf::okStatus();
    return report.status;
  }

  [[nodiscard]] bool remote() const noexcept override { return false; }

  [[nodiscard]] bool durableState() const noexcept override {
    return !runtime_.config().statePath.empty();
  }

  [[nodiscard]] wf::Status info(SessionInfo& out) override {
    out = SessionInfo{};
    out.remote = false;
    out.fence = runtime_.fence();
    out.runtimeGeneration = runtime_.runtimeGeneration();
    out.recoveryGeneration = runtime_.recoveryGeneration();
    out.authority = runtime_.authorityState();
    out.authorityKnown = true;
    const std::vector<wf::SpectrumDomain> domains = runtime_.domains();
    out.grids = runtime_.grids().size();
    out.domains = domains.size();
    out.exclusionDomains = runtime_.exclusionDomains().size();
    out.reservations = runtime_.reservations().size();
    out.auditRecords = runtime_.auditSize();
    out.lastAudit = runtime_.lastAuditSequence();
    out.stats = runtime_.stats();
    for (const wf::SpectrumDomain& domain : domains) {
      if (runtime_.capability(domain.id).has_value()) out.capabilities += 1;
    }
    return wf::okStatus();
  }

  [[nodiscard]] wf::Status registerGrid(const wf::ChannelGrid& grid) override {
    return runtime_.registerGrid(grid);
  }

  [[nodiscard]] wf::Status registerDomain(const wf::SpectrumDomain& domain) override {
    return runtime_.registerDomain(domain);
  }

  [[nodiscard]] wf::Status registerExclusionDomain(const wf::ExclusionDomain& exclusion) override {
    return runtime_.registerExclusionDomain(exclusion);
  }

  [[nodiscard]] wf::Status publishCapability(const wf::SpectrumCapability& capability) override {
    return runtime_.publishCapability(capability);
  }

  [[nodiscard]] wf::Status lookupDomainGeneration(wf::SpectrumDomainId id,
                                                  wf::SpectrumDomainGeneration& out) override {
    const std::optional<wf::SpectrumDomain> found = runtime_.domain(id);
    if (!found.has_value()) {
      return wf::fail(wf::StatusCode::NotFound,
                      "domain " + wf::typedToken("domain", id) + " is not registered");
    }
    out = found->generation;
    return wf::okStatus();
  }

  [[nodiscard]] wf::Status enumerate(const wf::SpectrumRequest& request,
                                     wf::CandidateSet& out) override {
    out = runtime_.enumerateCandidates(request);
    return out.status;
  }

  [[nodiscard]] wf::Status explain(const wf::SpectrumRequest& request,
                                   wf::DecisionExplanation& out) override {
    out = runtime_.explain(request);
    return wf::okStatus();
  }

  [[nodiscard]] wf::Status allocate(const wf::SpectrumRequest& request,
                                    wf::AllocationDecision& out) override {
    out = runtime_.allocate(request);
    return out.status;
  }

  [[nodiscard]] wf::Status renew(const wf::RenewArgs& args, wf::SpectrumReservation& out) override {
    out = wf::SpectrumReservation{};
    const wf::Status status = runtime_.renew(args.id, args.generation, args.extension,
                                             args.authority, args.now, args.maxRenewals);
    if (!status.ok()) return status;
    return reservation(args.id, out);
  }

  [[nodiscard]] wf::Status activate(const wf::ActivateArgs& args,
                                    wf::SpectrumReservation& out) override {
    out = wf::SpectrumReservation{};
    const wf::Status status = runtime_.activate(args.id, args.generation, args.authority, args.now);
    if (!status.ok()) return status;
    return reservation(args.id, out);
  }

  [[nodiscard]] wf::Status deactivate(const wf::ActivateArgs& args,
                                      wf::SpectrumReservation& out) override {
    out = wf::SpectrumReservation{};
    const wf::Status status = runtime_.deactivate(args.id, args.generation, args.authority,
                                                  args.now);
    if (!status.ok()) return status;
    return reservation(args.id, out);
  }

  [[nodiscard]] wf::Status release(const wf::ReleaseArgs& args,
                                   wf::SpectrumReservation& out) override {
    out = wf::SpectrumReservation{};
    const wf::Status status = runtime_.release(args.id, args.generation, args.authority, args.now);
    if (!status.ok()) return status;
    return reservation(args.id, out);
  }

  [[nodiscard]] wf::Status expireLeases(const wf::SweepArgs& args,
                                        wf::ReclaimReport& out) override {
    out = runtime_.expireLeases(args.now);
    return out.status;
  }

  [[nodiscard]] wf::Status reclaimExpired(const wf::SweepArgs& args,
                                          wf::ReclaimReport& out) override {
    out = runtime_.reclaimExpired(args.now);
    return out.status;
  }

  [[nodiscard]] wf::Status reservation(wf::ReservationId id,
                                       wf::SpectrumReservation& out) override {
    const std::optional<wf::SpectrumReservation> found = runtime_.reservation(id);
    if (!found.has_value()) {
      return wf::fail(wf::StatusCode::NotFound, "reservation " +
                                                    wf::typedToken("reservation", id) +
                                                    " is not known to this runtime");
    }
    out = *found;
    return wf::okStatus();
  }

  [[nodiscard]] wf::Status reservations(std::vector<wf::SpectrumReservation>& out) override {
    out = runtime_.reservations();
    return wf::okStatus();
  }

  [[nodiscard]] wf::Status usageAll(wf::Instant now,
                                    std::vector<wf::SpectrumUsage>& out) override {
    out = runtime_.usageAll(now);
    return wf::okStatus();
  }

  [[nodiscard]] wf::Status audit(std::uint64_t since, std::uint32_t limit,
                                 std::vector<wf::AuditRecord>& out) override {
    out = runtime_.audit(wf::AuditSequence(since), limit);
    return wf::okStatus();
  }

  [[nodiscard]] wf::Status save() override { return runtime_.save(); }

  [[nodiscard]] wf::Status reset() override { return runtime_.reset(); }

  [[nodiscard]] wf::Status recover(wf::RecoveryReport& out) override {
    out = runtime_.recover();
    return out.status;
  }

 private:
  [[nodiscard]] static wf::RuntimeConfig makeConfig(const std::string& statePath) {
    wf::RuntimeConfig config;
    config.statePath = statePath;
    // With a state path every committed allocation crosses the durable boundary
    // before the allocating call returns. Every other change is persisted by
    // the "save" command.
    config.durableCommits = !statePath.empty();
    return config;
  }

  wf::SpectrumRuntime runtime_;
};

// ---------------------------------------------------------------------------
// Remote session
// ---------------------------------------------------------------------------

class RemoteSession final : public Session {
 public:
  RemoteSession(std::string host, std::uint16_t port) : host_(std::move(host)), port_(port) {}

  [[nodiscard]] wf::Status connect() { return client_.connect(host_, port_); }

  [[nodiscard]] bool remote() const noexcept override { return true; }

  // The server owns its durable image; a client process never holds one.
  [[nodiscard]] bool durableState() const noexcept override { return false; }

  [[nodiscard]] wf::Status info(SessionInfo& out) override {
    wf::ServerDescription description;
    const wf::Status status = client_.describe(description);
    if (!status.ok()) return status;
    out = SessionInfo{};
    out.remote = true;
    out.fence = description.fence;
    out.runtimeGeneration = description.runtimeGeneration;
    out.recoveryGeneration = description.recoveryGeneration;
    out.authority = reconstructedAuthority(description.fence);
    out.authorityKnown = false;
    out.grids = description.grids;
    out.domains = description.domains;
    out.exclusionDomains = description.exclusionDomains;
    out.capabilities = description.capabilities;
    out.reservations = description.reservations;
    out.auditRecords = description.auditRecords;
    out.lastAudit = description.lastAudit;
    out.stats = description.stats;
    return wf::okStatus();
  }

  [[nodiscard]] wf::Status registerGrid(const wf::ChannelGrid& grid) override {
    return client_.registerGrid(grid);
  }

  [[nodiscard]] wf::Status registerDomain(const wf::SpectrumDomain& domain) override {
    return client_.registerDomain(domain);
  }

  [[nodiscard]] wf::Status registerExclusionDomain(const wf::ExclusionDomain& exclusion) override {
    return client_.registerExclusionDomain(exclusion);
  }

  [[nodiscard]] wf::Status publishCapability(const wf::SpectrumCapability& capability) override {
    return client_.publishCapability(capability);
  }

  [[nodiscard]] wf::Status lookupDomainGeneration(wf::SpectrumDomainId id,
                                                  wf::SpectrumDomainGeneration& out) override {
    (void)id;
    out = wf::SpectrumDomainGeneration{};
    return wf::fail(wf::StatusCode::Unsupported,
                    "the transport exposes no domain lookup; name the generation explicitly as "
                    "<domainId>@<domainGeneration>");
  }

  [[nodiscard]] wf::Status enumerate(const wf::SpectrumRequest& request,
                                     wf::CandidateSet& out) override {
    out = wf::CandidateSet{};
    const wf::Status status = client_.enumerate(request, out);
    if (status.code != wf::StatusCode::Corruption) return status;
    // A refusal status comes back before any decode is attempted, so a
    // corruption here means the reply carried no candidate set at all. The
    // reference server reports the enumeration status without the encoded
    // candidate set, which leaves the reply payload empty; say so instead of
    // reporting a generic decode failure.
    return wf::fail(wf::StatusCode::Unavailable,
                    "the server returned the enumeration status without an encoded candidate "
                    "set, so no candidate list is available over the transport; inspect "
                    "candidates in local mode");
  }

  [[nodiscard]] wf::Status explain(const wf::SpectrumRequest& request,
                                   wf::DecisionExplanation& out) override {
    out = wf::DecisionExplanation{};
    return client_.explain(request, out);
  }

  [[nodiscard]] wf::Status allocate(const wf::SpectrumRequest& request,
                                    wf::AllocationDecision& out) override {
    out = wf::AllocationDecision{};
    return client_.allocate(request, out);
  }

  [[nodiscard]] wf::Status renew(const wf::RenewArgs& args, wf::SpectrumReservation& out) override {
    out = wf::SpectrumReservation{};
    return client_.renew(args, out);
  }

  [[nodiscard]] wf::Status activate(const wf::ActivateArgs& args,
                                    wf::SpectrumReservation& out) override {
    out = wf::SpectrumReservation{};
    return client_.activate(args, out);
  }

  [[nodiscard]] wf::Status deactivate(const wf::ActivateArgs& args,
                                      wf::SpectrumReservation& out) override {
    out = wf::SpectrumReservation{};
    return client_.deactivate(args, out);
  }

  [[nodiscard]] wf::Status release(const wf::ReleaseArgs& args,
                                   wf::SpectrumReservation& out) override {
    out = wf::SpectrumReservation{};
    return client_.release(args, out);
  }

  [[nodiscard]] wf::Status expireLeases(const wf::SweepArgs& args,
                                        wf::ReclaimReport& out) override {
    out = wf::ReclaimReport{};
    return client_.expireLeases(args, out);
  }

  [[nodiscard]] wf::Status reclaimExpired(const wf::SweepArgs& args,
                                          wf::ReclaimReport& out) override {
    out = wf::ReclaimReport{};
    return client_.reclaimExpired(args, out);
  }

  [[nodiscard]] wf::Status reservation(wf::ReservationId id,
                                       wf::SpectrumReservation& out) override {
    out = wf::SpectrumReservation{};
    return client_.queryReservation(id, out);
  }

  [[nodiscard]] wf::Status reservations(std::vector<wf::SpectrumReservation>& out) override {
    out.clear();
    return client_.queryReservations(out);
  }

  [[nodiscard]] wf::Status usageAll(wf::Instant now,
                                    std::vector<wf::SpectrumUsage>& out) override {
    out.clear();
    return client_.queryUsageAll(now, out);
  }

  [[nodiscard]] wf::Status audit(std::uint64_t since, std::uint32_t limit,
                                 std::vector<wf::AuditRecord>& out) override {
    wf::AuditArgs args;
    args.since = wf::AuditSequence(since);
    args.limit = limit;
    out.clear();
    return client_.queryAudit(args, out);
  }

  [[nodiscard]] wf::Status save() override { return client_.save(); }

  [[nodiscard]] wf::Status reset() override {
    std::vector<std::uint8_t> reply;
    return client_.call(wf::OpCode::Reset, {}, reply);
  }

  [[nodiscard]] wf::Status recover(wf::RecoveryReport& out) override {
    out = wf::RecoveryReport{};
    return wf::fail(wf::StatusCode::NotPermitted,
                    "recover is not exposed by the transport protocol; the server owns its "
                    "durable state");
  }

 private:
  // The framed protocol reports the controller fence but not the authority
  // generations. Tokens minted here therefore carry the fence the server just
  // reported and generation 1, which is the generation a runtime starts at. A
  // server that has advanced its authority refuses those tokens with a typed
  // stale-authority refusal instead of accepting them.
  [[nodiscard]] static wf::AuthorityState reconstructedAuthority(
      const wf::ControllerFence& fence) noexcept {
    wf::AuthorityState authority;
    authority.eligibilityGeneration = wf::EligibilityAuthorityGeneration(1);
    authority.reservationGeneration = wf::ReservationAuthorityGeneration(1);
    authority.activationGeneration = wf::ActivationAuthorityGeneration(1);
    authority.releaseGeneration = wf::ReleaseAuthorityGeneration(1);
    authority.fence = fence;
    return authority;
  }

  std::string host_;
  std::uint16_t port_{0};
  wf::SpectrumClient client_;
};

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void printStatus(const std::string& what, const wf::Status& status) {
  std::cout << what << ": status=" << wf::toToken(status.code)
            << " message=" << (status.message.empty() ? std::string("-") : status.message) << '\n';
}

int usageError(const std::string& message) {
  std::cout << "usage-error: " << message << '\n';
  std::cout << "run 'wfcli help' for the command grammar\n";
  return kExitUsage;
}

void printLease(const wf::Lease& lease) {
  std::cout << "lease generation=" << lease.generation.raw()
            << " grantedAt=" << wf::renderInstant(lease.grantedAt)
            << " expiresAt=" << wf::renderInstant(lease.expiresAt)
            << " renewals=" << lease.renewalCount << '/' << lease.maxRenewals << '\n';
}

void printReservation(const wf::SpectrumReservation& reservation) {
  std::cout << "reservation id=" << reservation.id.raw()
            << " generation=" << reservation.generation.raw()
            << " state=" << wf::toToken(reservation.state)
            << " request=" << reservation.requestId.raw() << '@'
            << reservation.requestGeneration.raw()
            << " owner=" << reservation.owner.raw() << '@' << reservation.ownerGeneration.raw()
            << '\n';
  std::cout << "reservation id=" << reservation.id.raw() << " grid=" << reservation.grid.raw()
            << '@' << reservation.gridGeneration.raw() << ' ' << wf::renderSlotRange(reservation.slots)
            << ' ' << wf::renderFrequencyRange(reservation.frequency)
            << " crossGrid=" << (reservation.crossGrid ? "true" : "false")
            << " contiguityRequired=" << (reservation.contiguityRequired ? "true" : "false")
            << " continuityRequired=" << (reservation.continuityRequired ? "true" : "false")
            << " guardBandMhz=" << reservation.guardBandMhz << '\n';
  std::cout << "reservation id=" << reservation.id.raw() << " domains=[";
  for (std::size_t index = 0; index < reservation.domains.size(); ++index) {
    if (index != 0) std::cout << ',';
    std::cout << reservation.domains[index].raw();
    if (index < reservation.domainGenerations.size()) {
      std::cout << '@' << reservation.domainGenerations[index].raw();
    }
    if (index < reservation.perDomainSlots.size()) {
      std::cout << ' ' << wf::renderSlotRange(reservation.perDomainSlots[index]);
    }
  }
  std::cout << "]\n";
  printLease(reservation.lease);
}

void printUsageRecord(const wf::SpectrumUsage& usage) {
  std::cout << "usage domain=" << usage.domain.raw() << '@' << usage.domainGeneration.raw()
            << " grid=" << usage.grid.raw() << '@' << usage.gridGeneration.raw()
            << " totalSlots=" << usage.totalSlots
            << " allocatableSlots=" << usage.allocatableSlots
            << " liveSlots=" << usage.liveSlots
            << " activeSlots=" << usage.activeSlots
            << " reservedSlots=" << usage.reservedSlots
            << " lapsedSlots=" << usage.lapsedSlots
            << " freeSlots=" << usage.freeSlots << '\n';
  std::cout << "usage domain=" << usage.domain.raw()
            << " liveReservations=" << usage.liveReservations
            << " activeReservations=" << usage.activeReservations
            << " lapsedReservations=" << usage.lapsedReservations
            << " reclaimedReservations=" << usage.reclaimedReservations
            << " releasedReservations=" << usage.releasedReservations
            << " freeRuns=" << usage.freeRuns.size() << '\n';
  for (const wf::SlotRange& run : usage.freeRuns) {
    std::cout << "usage domain=" << usage.domain.raw() << " freeRun " << wf::renderSlotRange(run)
              << '\n';
  }
}

void printCandidates(const wf::CandidateSet& candidates) {
  const std::size_t printed = candidates.candidates.size() < kMaxPrintedCandidates
                                  ? candidates.candidates.size()
                                  : kMaxPrintedCandidates;
  for (std::size_t index = 0; index < printed; ++index) {
    std::cout << "candidate " << candidates.candidates[index].describe() << '\n';
  }
  if (candidates.candidates.size() > printed) {
    std::cout << "candidate ... " << (candidates.candidates.size() - printed)
              << " further candidate(s) not printed\n";
  }
}

void printReasons(const std::vector<std::string>& reasons) {
  for (std::size_t index = 0; index < reasons.size(); ++index) {
    std::cout << "reason[" << index << "]=" << reasons[index] << '\n';
  }
}

// ---------------------------------------------------------------------------
// Request construction
// ---------------------------------------------------------------------------

struct RequestSpec {
  std::vector<wf::SpectrumDomainId> domains;
  std::vector<wf::SpectrumDomainGeneration> domainGenerations;
  wf::ChannelGridId grid{};
  wf::GridGeneration gridGeneration{};
  std::uint32_t slots{0};
  wf::ContiguityRequirement contiguity{wf::ContiguityRequirement::Unspecified};
  wf::ContinuityRequirement continuity{wf::ContinuityRequirement::Unspecified};
  wf::Duration lease{};
};

// A domain entry is either "<id>", whose generation is resolved in-process, or
// "<id>@<generation>", the form the runtime itself uses when it renders a
// request.
[[nodiscard]] bool parseDomainEntry(std::string_view entry, Session& session,
                                    wf::SpectrumDomainId& id,
                                    wf::SpectrumDomainGeneration& generation, std::string& error) {
  const std::vector<std::string_view> parts = split(entry, '@');
  if (parts.size() > 2) {
    error = "domain entry '" + std::string(entry) + "' must be <id> or <id>@<generation>";
    return false;
  }
  std::uint64_t raw = 0;
  if (!parseIdentity(parts[0], raw)) {
    error = "domain id in '" + std::string(entry) + "' must be a non-zero integer";
    return false;
  }
  id = wf::SpectrumDomainId(raw);
  if (parts.size() == 2) {
    std::uint64_t rawGeneration = 0;
    if (!parseIdentity(parts[1], rawGeneration)) {
      error = "domain generation in '" + std::string(entry) + "' must be a non-zero integer";
      return false;
    }
    generation = wf::SpectrumDomainGeneration(rawGeneration);
    return true;
  }
  const wf::Status status = session.lookupDomainGeneration(id, generation);
  if (!status.ok()) {
    error = status.message;
    return false;
  }
  return true;
}

[[nodiscard]] bool parseRequestSpec(std::string_view spec, Session& session, RequestSpec& out,
                                    std::string& error) {
  const std::vector<std::string_view> fields = split(spec, ':');
  if (fields.size() != 7) {
    error =
        "a request spec is <domainList>:<grid>:<gridGen>:<slots>:<contiguity>:<continuity>:"
        "<leaseSeconds>";
    return false;
  }
  const std::vector<std::string_view> domainFields = split(fields[0], ',');
  if (domainFields.size() > wf::kMaxRequestDomains) {
    error = "a request may name at most " + std::to_string(wf::kMaxRequestDomains) + " domains";
    return false;
  }
  out = RequestSpec{};
  for (const std::string_view entry : domainFields) {
    wf::SpectrumDomainId id;
    wf::SpectrumDomainGeneration generation;
    if (!parseDomainEntry(entry, session, id, generation, error)) return false;
    if (!out.domains.empty() && !(out.domains.back() < id)) {
      error = "request domains must be strictly ascending, but " + std::to_string(id.raw()) +
              " does not follow " + std::to_string(out.domains.back().raw());
      return false;
    }
    out.domains.push_back(id);
    out.domainGenerations.push_back(generation);
  }
  std::uint64_t gridId = 0;
  std::uint64_t gridGeneration = 0;
  if (!parseIdentity(fields[1], gridId)) {
    error = "grid id must be a non-zero integer";
    return false;
  }
  if (!parseIdentity(fields[2], gridGeneration)) {
    error = "grid generation must be a non-zero integer";
    return false;
  }
  out.grid = wf::ChannelGridId(gridId);
  out.gridGeneration = wf::GridGeneration(gridGeneration);
  if (!parseU32(fields[3], out.slots) || out.slots == 0 || out.slots > wf::kMaxSlotsPerChannel) {
    error = "slots must be in [1, " + std::to_string(wf::kMaxSlotsPerChannel) + "]";
    return false;
  }
  if (!parseContiguity(fields[4], out.contiguity, error)) return false;
  if (!parseContinuity(fields[5], out.continuity, error)) return false;
  return parseLease(fields[6], out.lease, error);
}

// The next unused request identity. Reusing a committed identity would be an
// idempotent replay or a supersede, which is not what a fresh command-line
// allocation means.
[[nodiscard]] wf::Status nextRequestId(Session& session, std::uint64_t& out) {
  std::vector<wf::SpectrumReservation> existing;
  const wf::Status status = session.reservations(existing);
  if (!status.ok()) return status;
  std::uint64_t maximum = 0;
  for (const wf::SpectrumReservation& reservation : existing) {
    maximum = std::max(maximum, reservation.requestId.raw());
  }
  if (maximum == std::numeric_limits<std::uint64_t>::max()) {
    return wf::fail(wf::StatusCode::LimitExceeded, "the request identity space is exhausted");
  }
  out = maximum + 1;
  return wf::okStatus();
}

// Builds the request the runtime expects: identities, generations, the grid
// generation, the instant, and authority tokens minted under the fence the
// runtime currently recognises.
[[nodiscard]] wf::Status buildRequest(Session& session, const RequestSpec& spec, wf::Instant at,
                                      wf::SpectrumRequest& out, std::string& detail) {
  SessionInfo info;
  wf::Status status = session.info(info);
  if (!status.ok()) return status;
  std::uint64_t requestId = 0;
  status = nextRequestId(session, requestId);
  if (!status.ok()) return status;

  out = wf::SpectrumRequest{};
  out.requestId = wf::AllocationRequestId(requestId);
  out.requestGeneration = wf::AllocationRequestGeneration(1);
  out.owner = wf::OwnerId(1);
  out.ownerGeneration = wf::OwnerGeneration(1);
  out.domains = spec.domains;
  out.domainGenerations = spec.domainGenerations;
  out.grid = spec.grid;
  out.gridGeneration = spec.gridGeneration;
  out.slots = spec.slots;
  out.contiguity = spec.contiguity;
  out.continuity = spec.continuity;
  out.leaseDuration = spec.lease;
  out.requestedAt = at;
  out.notBefore = wf::Instant{};
  out.eligibilityAuthority =
      wf::EligibilityAuthority{info.authority.eligibilityGeneration, info.fence};
  out.reservationAuthority =
      wf::ReservationAuthority{info.authority.reservationGeneration, info.fence};
  detail = "request id=" + std::to_string(requestId) +
           " authorityKnown=" + (info.authorityKnown ? "true" : "false");
  return wf::okStatus();
}

[[nodiscard]] wf::Instant commandInstant(const Command& command) noexcept {
  return command.at.has_value() ? *command.at : wf::systemNow();
}

// ---------------------------------------------------------------------------
// Commands: registration
// ---------------------------------------------------------------------------

int commandRegisterGrid(Session& session, const Command& command) {
  if (command.args.size() != 1) {
    return usageError("register-grid takes one spec: "
                      "fixed:<id>:<gen>:<anchorMhz>:<slotWidthMhz>:<slots> or "
                      "flex:<id>:<gen>:<anchorMhz>:<slotWidthMhz>:<slots>:<min>:<max>");
  }
  const std::vector<std::string_view> fields = split(command.args[0], ':');
  if (fields.size() != 6 && fields.size() != 8) {
    return usageError("a grid spec has 6 fields (fixed) or 8 fields (flex), not " +
                      std::to_string(fields.size()));
  }
  wf::ChannelGrid grid;
  if (fields[0] == "fixed") {
    if (fields.size() != 6) return usageError("a fixed grid spec has exactly 6 fields");
    grid.kind = wf::GridKind::Fixed;
    grid.minSlotsPerChannel = 1;
    grid.maxSlotsPerChannel = 1;
  } else if (fields[0] == "flex") {
    if (fields.size() != 8) return usageError("a flex grid spec has exactly 8 fields");
    grid.kind = wf::GridKind::Flex;
  } else {
    return usageError("grid kind must be fixed or flex, not '" + std::string(fields[0]) + "'");
  }

  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  std::int64_t anchorMhz = 0;
  std::int64_t slotWidthMhz = 0;
  if (!parseIdentity(fields[1], id)) return usageError("grid id must be a non-zero integer");
  if (!parseIdentity(fields[2], generation)) {
    return usageError("grid generation must be a non-zero integer");
  }
  if (!parseI64(fields[3], anchorMhz) || anchorMhz < wf::kMinFrequencyMhz ||
      anchorMhz > wf::kMaxFrequencyMhz) {
    return usageError("anchorMhz must be in [" + std::to_string(wf::kMinFrequencyMhz) + ", " +
                      std::to_string(wf::kMaxFrequencyMhz) + "]");
  }
  if (!parseI64(fields[4], slotWidthMhz) || slotWidthMhz < wf::kMinSlotWidthMhz ||
      slotWidthMhz > wf::kMaxSlotWidthMhz) {
    return usageError("slotWidthMhz must be in [" + std::to_string(wf::kMinSlotWidthMhz) + ", " +
                      std::to_string(wf::kMaxSlotWidthMhz) + "]");
  }
  if (!parseU32(fields[5], grid.slotCount) || grid.slotCount == 0 ||
      grid.slotCount > wf::kMaxGridSlots) {
    return usageError("slots must be in [1, " + std::to_string(wf::kMaxGridSlots) + "]");
  }
  std::int64_t span = 0;
  std::int64_t endMhz = 0;
  if (wf::mulOverflow(static_cast<std::int64_t>(grid.slotCount), slotWidthMhz, span) ||
      wf::addOverflow(anchorMhz, span, endMhz)) {
    return usageError("the grid span is not representable");
  }
  if (endMhz > wf::kMaxFrequencyMhz) {
    return usageError("the grid ends at " + std::to_string(endMhz) + " MHz, above the maximum of " +
                      std::to_string(wf::kMaxFrequencyMhz) + " MHz");
  }
  grid.id = wf::ChannelGridId(id);
  grid.generation = wf::GridGeneration(generation);
  grid.anchorMhz = anchorMhz;
  grid.slotWidthMhz = slotWidthMhz;
  if (grid.kind == wf::GridKind::Flex) {
    std::uint32_t minimum = 0;
    std::uint32_t maximum = 0;
    if (!parseU32(fields[6], minimum) || minimum == 0 ||
        minimum > wf::kMaxSlotsPerChannel) {
      return usageError("min slots per channel must be in [1, " +
                        std::to_string(wf::kMaxSlotsPerChannel) + "]");
    }
    if (!parseU32(fields[7], maximum) || maximum < minimum ||
        maximum > wf::kMaxSlotsPerChannel) {
      return usageError("max slots per channel must be in [min, " +
                        std::to_string(wf::kMaxSlotsPerChannel) + "]");
    }
    grid.minSlotsPerChannel = minimum;
    grid.maxSlotsPerChannel = maximum;
  }

  const wf::Status status = session.registerGrid(grid);
  printStatus("register-grid", status);
  if (!status.ok()) return kExitRefused;
  std::cout << "registered " << wf::describeGrid(grid) << '\n';
  return kExitOk;
}

int commandRegisterDomain(Session& session, const Command& command) {
  if (command.args.size() != 1) {
    return usageError(
        "register-domain takes one spec: <id>:<generation>:<gridId>:<gridGen>[:contiguous|"
        "fragmented]");
  }
  const std::vector<std::string_view> fields = split(command.args[0], ':');
  if (fields.size() != 4 && fields.size() != 5) {
    return usageError("a domain spec has 4 fields plus an optional contiguity token");
  }
  wf::SpectrumDomain domain;
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  std::uint64_t gridId = 0;
  std::uint64_t gridGeneration = 0;
  if (!parseIdentity(fields[0], id)) return usageError("domain id must be a non-zero integer");
  if (!parseIdentity(fields[1], generation)) {
    return usageError("domain generation must be a non-zero integer");
  }
  if (!parseIdentity(fields[2], gridId)) return usageError("grid id must be a non-zero integer");
  if (!parseIdentity(fields[3], gridGeneration)) {
    return usageError("grid generation must be a non-zero integer");
  }
  domain.id = wf::SpectrumDomainId(id);
  domain.generation = wf::SpectrumDomainGeneration(generation);
  domain.klass = wf::ResourceClass::AbstractDomain;
  domain.grid = wf::ChannelGridId(gridId);
  domain.gridGeneration = wf::GridGeneration(gridGeneration);
  if (fields.size() == 5) {
    if (fields[4] == "contiguous") {
      domain.requiresContiguity = true;
    } else if (fields[4] == "fragmented") {
      domain.requiresContiguity = false;
    } else {
      return usageError("the contiguity token must be contiguous or fragmented");
    }
  }

  const wf::Status status = session.registerDomain(domain);
  printStatus("register-domain", status);
  if (!status.ok()) return kExitRefused;
  std::cout << "registered domain id=" << domain.id.raw()
            << " generation=" << domain.generation.raw()
            << " class=" << wf::toToken(domain.klass) << " grid=" << domain.grid.raw() << '@'
            << domain.gridGeneration.raw()
            << " requiresContiguity=" << (domain.requiresContiguity ? "true" : "false") << '\n';
  return kExitOk;
}

int commandRegisterExclusion(Session& session, const Command& command) {
  if (command.args.size() != 1) {
    return usageError("register-exclusion takes one spec: "
                      "<id>:<generation>:<guardMhz>:<domainId,...>");
  }
  const std::vector<std::string_view> fields = split(command.args[0], ':');
  if (fields.size() != 4) return usageError("an exclusion spec has exactly 4 fields");
  wf::ExclusionDomain exclusion;
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  std::int64_t guardMhz = 0;
  if (!parseIdentity(fields[0], id)) {
    return usageError("exclusion domain id must be a non-zero integer");
  }
  if (!parseIdentity(fields[1], generation)) {
    return usageError("exclusion domain generation must be a non-zero integer");
  }
  if (!parseI64(fields[2], guardMhz) || guardMhz < 0 || guardMhz > wf::kMaxGuardBandMhz) {
    return usageError("guardMhz must be in [0, " + std::to_string(wf::kMaxGuardBandMhz) + "]");
  }
  const std::vector<std::string_view> memberFields = split(fields[3], ',');
  if (memberFields.size() > wf::kMaxExclusionDomainMembers) {
    return usageError("an exclusion domain may name at most " +
                      std::to_string(wf::kMaxExclusionDomainMembers) + " members");
  }
  for (const std::string_view entry : memberFields) {
    std::uint64_t member = 0;
    if (!parseIdentity(entry, member)) {
      return usageError("exclusion member '" + std::string(entry) +
                        "' must be a non-zero domain id");
    }
    if (!exclusion.members.empty() && !(exclusion.members.back() < wf::SpectrumDomainId(member))) {
      return usageError("exclusion members must be strictly ascending, but " +
                        std::to_string(member) + " does not follow " +
                        std::to_string(exclusion.members.back().raw()));
    }
    exclusion.members.push_back(wf::SpectrumDomainId(member));
  }
  exclusion.id = wf::ExclusionDomainId(id);
  exclusion.generation = wf::ExclusionDomainGeneration(generation);
  exclusion.guardBandMhz = guardMhz;

  const wf::Status status = session.registerExclusionDomain(exclusion);
  printStatus("register-exclusion", status);
  if (!status.ok()) return kExitRefused;
  std::cout << "registered exclusion id=" << exclusion.id.raw()
            << " generation=" << exclusion.generation.raw()
            << " guardBandMhz=" << exclusion.guardBandMhz << " members=[";
  for (std::size_t index = 0; index < exclusion.members.size(); ++index) {
    if (index != 0) std::cout << ',';
    std::cout << exclusion.members[index].raw();
  }
  std::cout << "]\n";
  return kExitOk;
}

int commandPublishCapability(Session& session, const Command& command) {
  if (command.args.size() != 1) {
    return usageError("publish-capability takes one spec: <domain>:<domainGen>:<grid>:<gridGen>:"
                      "<supported|unsupported|unknown>:<firstSlot>:<slots>:<minTunableMhz>:"
                      "<maxTunableMhz>");
  }
  const std::vector<std::string_view> fields = split(command.args[0], ':');
  if (fields.size() != 9) return usageError("a capability spec has exactly 9 fields");

  SessionInfo info;
  const wf::Status infoStatus = session.info(info);
  if (!infoStatus.ok()) {
    printStatus("publish-capability", infoStatus);
    return kExitRefused;
  }
  const wf::Instant at = commandInstant(command);
  if (at.nanos() <= 0) return usageError("the publication instant must be positive");

  wf::SpectrumCapability capability;
  std::uint64_t domain = 0;
  std::uint64_t domainGeneration = 0;
  std::uint64_t grid = 0;
  std::uint64_t gridGeneration = 0;
  if (!parseIdentity(fields[0], domain)) return usageError("domain id must be a non-zero integer");
  if (!parseIdentity(fields[1], domainGeneration)) {
    return usageError("domain generation must be a non-zero integer");
  }
  if (!parseIdentity(fields[2], grid)) return usageError("grid id must be a non-zero integer");
  if (!parseIdentity(fields[3], gridGeneration)) {
    return usageError("grid generation must be a non-zero integer");
  }
  capability.domain = wf::SpectrumDomainId(domain);
  capability.domainGeneration = wf::SpectrumDomainGeneration(domainGeneration);
  capability.grid = wf::ChannelGridId(grid);
  capability.gridGeneration = wf::GridGeneration(gridGeneration);
  if (fields[4] == "supported") {
    capability.support = wf::SpectrumSupport::Supported;
  } else if (fields[4] == "unsupported") {
    capability.support = wf::SpectrumSupport::Unsupported;
  } else if (fields[4] == "unknown") {
    capability.support = wf::SpectrumSupport::Unknown;
  } else {
    return usageError("support must be supported, unsupported or unknown");
  }
  if (!parseU32(fields[5], capability.firstAllocatableSlot)) {
    return usageError("firstSlot must be an integer in [0, " +
                      std::to_string(wf::kMaxGridSlots) + "]");
  }
  if (!parseU32(fields[6], capability.allocatableSlots)) {
    return usageError("slots must be an integer in [0, " + std::to_string(wf::kMaxGridSlots) + "]");
  }
  std::int64_t windowEnd = 0;
  if (wf::addOverflow(static_cast<std::int64_t>(capability.firstAllocatableSlot),
                      static_cast<std::int64_t>(capability.allocatableSlots), windowEnd)) {
    return usageError("the allocatable window overflows the slot index space");
  }
  if (!parseI64(fields[7], capability.minTunableMhz) ||
      capability.minTunableMhz < wf::kMinFrequencyMhz ||
      capability.minTunableMhz > wf::kMaxFrequencyMhz) {
    return usageError("minTunableMhz must be in [" + std::to_string(wf::kMinFrequencyMhz) + ", " +
                      std::to_string(wf::kMaxFrequencyMhz) + "]");
  }
  if (!parseI64(fields[8], capability.maxTunableMhz) ||
      capability.maxTunableMhz < wf::kMinFrequencyMhz ||
      capability.maxTunableMhz > wf::kMaxFrequencyMhz) {
    return usageError("maxTunableMhz must be in [" + std::to_string(wf::kMinFrequencyMhz) + ", " +
                      std::to_string(wf::kMaxFrequencyMhz) + "]");
  }
  if (capability.support == wf::SpectrumSupport::Supported &&
      capability.maxTunableMhz <= capability.minTunableMhz) {
    return usageError("a supported capability needs a non-empty tunable range");
  }

  // The evidence digest is derived from the spec text the caller supplied, so
  // it is reproducible and never zero. The runtime refuses a supported claim
  // that arrives without usable evidence.
  const std::string specText(command.args[0]);
  const std::vector<std::uint8_t> specBytes(specText.begin(), specText.end());
  std::uint64_t digest = wf::crc32(specBytes);
  if (digest == 0) digest = 1;
  capability.presenceEvidence.present = true;
  capability.presenceEvidence.digest = digest;
  capability.presenceEvidence.source = "wfcli";
  capability.generation = wf::CapabilityGeneration(1);
  capability.publisher = wf::ControllerId(1);
  capability.fence = info.fence;
  capability.publishedAt = at;

  const wf::Status status = session.publishCapability(capability);
  printStatus("publish-capability", status);
  if (!status.ok()) return kExitRefused;
  std::cout << "published " << wf::describeCapability(capability) << '\n';
  return kExitOk;
}

// ---------------------------------------------------------------------------
// Commands: enumeration and allocation
// ---------------------------------------------------------------------------

int commandEnumerate(Session& session, const Command& command) {
  if (command.args.size() != 1) {
    return usageError("enumerate takes one spec: "
                      "<domainList>:<grid>:<gridGen>:<slots>:<contiguity>:<continuity>:"
                      "<leaseSeconds>");
  }
  RequestSpec spec;
  std::string error;
  if (!parseRequestSpec(command.args[0], session, spec, error)) return usageError(error);

  const wf::Instant at = commandInstant(command);
  if (at.nanos() <= 0) return usageError("the enumeration instant must be positive");

  wf::SpectrumRequest request;
  std::string detail;
  const wf::Status built = buildRequest(session, spec, at, request, detail);
  if (!built.ok()) {
    printStatus("enumerate", built);
    return kExitRefused;
  }

  wf::CandidateSet candidates;
  const wf::Status status = session.enumerate(request, candidates);
  std::cout << "enumerate: " << detail << " status=" << wf::toToken(status.code)
            << " message=" << (status.message.empty() ? std::string("-") : status.message) << '\n';
  if (!status.ok()) {
    printStatus("enumerate", status);
    return kExitRefused;
  }
  std::cout << "enumerate: eligible=" << candidates.eligibleCount
            << " listed=" << candidates.candidates.size() << " omitted=" << candidates.omitted
            << " complete=" << (candidates.complete ? "true" : "false") << '\n';
  std::cout << "enumerate: summary=" << candidates.summary << '\n';
  printCandidates(candidates);
  return kExitOk;
}

// Reports the typed outcome a request would produce, without creating
// ownership. The exit code matches what "allocate" would return, so it can be
// used as a dry run.
int commandExplain(Session& session, const Command& command) {
  if (command.args.size() != 1) {
    return usageError("explain takes one spec: "
                      "<domainList>:<grid>:<gridGen>:<slots>:<contiguity>:<continuity>:"
                      "<leaseSeconds>");
  }
  RequestSpec spec;
  std::string error;
  if (!parseRequestSpec(command.args[0], session, spec, error)) return usageError(error);

  const wf::Instant at = commandInstant(command);
  if (at.nanos() <= 0) return usageError("the explanation instant must be positive");

  wf::SpectrumRequest request;
  std::string detail;
  const wf::Status built = buildRequest(session, spec, at, request, detail);
  if (!built.ok()) {
    printStatus("explain", built);
    return kExitRefused;
  }

  wf::DecisionExplanation explanation;
  const wf::Status status = session.explain(request, explanation);
  std::cout << "explain: " << detail << " status=" << wf::toToken(status.code)
            << " message=" << (status.message.empty() ? std::string("-") : status.message) << '\n';
  if (!status.ok()) {
    printStatus("explain", status);
    return kExitRefused;
  }
  std::cout << "explain " << explanation.summary() << '\n';
  if (explanation.outcome == wf::AllocationOutcome::Allocated) {
    std::cout << "explain: would allocate " << wf::renderSlotRange(explanation.selected.slots) << ' '
              << wf::renderFrequencyRange(explanation.selected.frequency) << '\n';
  }
  printReasons(explanation.reasons);
  std::cout << "explain: no ownership was created; the outcome token is what allocate would "
               "return\n";
  return explanation.outcome == wf::AllocationOutcome::Allocated ? kExitOk : kExitRefused;
}

int commandAllocate(Session& session, const Command& command) {
  if (command.args.size() != 1) {
    return usageError("allocate takes one spec: "
                      "<domainList>:<grid>:<gridGen>:<slots>:<contiguity>:<continuity>:"
                      "<leaseSeconds>");
  }
  RequestSpec spec;
  std::string error;
  if (!parseRequestSpec(command.args[0], session, spec, error)) return usageError(error);

  const wf::Instant at = commandInstant(command);
  if (at.nanos() <= 0) return usageError("the allocation instant must be positive");

  wf::SpectrumRequest request;
  std::string detail;
  const wf::Status built = buildRequest(session, spec, at, request, detail);
  if (!built.ok()) {
    printStatus("allocate", built);
    return kExitRefused;
  }

  wf::AllocationDecision decision;
  const wf::Status status = session.allocate(request, decision);
  std::cout << "allocate: " << detail << '\n';
  std::cout << "allocate decision " << decision.describe() << '\n';
  printReasons(decision.explanation.reasons);
  if (!decision.allocated()) {
    if (decision.explanation.conflicts.empty()) {
      std::cout << "allocate: no conflicting reservation\n";
    } else {
      std::cout << "allocate: conflicts=[";
      for (std::size_t index = 0; index < decision.explanation.conflicts.size(); ++index) {
        if (index != 0) std::cout << ',';
        std::cout << decision.explanation.conflicts[index].raw();
      }
      std::cout << "]\n";
    }
    if (decision.outcome == wf::AllocationOutcome::Unknown) {
      std::cout << "allocate: status=" << wf::toToken(status.code)
                << " message=" << (status.message.empty() ? std::string("-") : status.message)
                << '\n';
    }
    return kExitRefused;
  }

  wf::SpectrumReservation reservation;
  const wf::Status fetched = session.reservation(decision.reservation, reservation);
  if (fetched.ok()) {
    printReservation(reservation);
  } else {
    const wf::SpectrumCandidate& selected = decision.explanation.selected;
    std::cout << "allocate: reservation=" << decision.reservation.raw()
              << " generation=" << decision.generation.raw() << ' '
              << wf::renderSlotRange(selected.slots) << ' '
              << wf::renderFrequencyRange(selected.frequency) << '\n';
  }
  return kExitOk;
}

// ---------------------------------------------------------------------------
// Commands: reservation lifecycle
// ---------------------------------------------------------------------------

int commandRenew(Session& session, const Command& command) {
  if (command.args.size() != 1) {
    return usageError("renew takes one spec: <reservationId>:<generation>:<extensionSeconds>");
  }
  const std::vector<std::string_view> fields = split(command.args[0], ':');
  if (fields.size() != 3) return usageError("a renewal spec has exactly 3 fields");
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  if (!parseIdentity(fields[0], id)) {
    return usageError("reservation id must be a non-zero integer");
  }
  if (!parseIdentity(fields[1], generation)) {
    return usageError("reservation generation must be a non-zero integer");
  }
  wf::Duration extension;
  std::string error;
  if (!parseLease(fields[2], extension, error)) return usageError(error);

  SessionInfo info;
  const wf::Status infoStatus = session.info(info);
  if (!infoStatus.ok()) {
    printStatus("renew", infoStatus);
    return kExitRefused;
  }
  wf::RenewArgs args;
  args.id = wf::ReservationId(id);
  args.generation = wf::ReservationGeneration(generation);
  args.extension = extension;
  args.authority = wf::ReservationAuthority{info.authority.reservationGeneration, info.fence};
  args.now = commandInstant(command);
  args.maxRenewals = 0;

  wf::SpectrumReservation reservation;
  const wf::Status status = session.renew(args, reservation);
  printStatus("renew", status);
  if (!status.ok()) return kExitRefused;
  printReservation(reservation);
  return kExitOk;
}

int commandActivation(Session& session, const Command& command, bool activating) {
  const char* const verb = activating ? "activate" : "deactivate";
  if (command.args.size() != 1) {
    return usageError(std::string(verb) + " takes one spec: <reservationId>:<generation>");
  }
  const std::vector<std::string_view> fields = split(command.args[0], ':');
  if (fields.size() != 2) return usageError("an activation spec has exactly 2 fields");
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  if (!parseIdentity(fields[0], id)) {
    return usageError("reservation id must be a non-zero integer");
  }
  if (!parseIdentity(fields[1], generation)) {
    return usageError("reservation generation must be a non-zero integer");
  }

  SessionInfo info;
  const wf::Status infoStatus = session.info(info);
  if (!infoStatus.ok()) {
    printStatus(verb, infoStatus);
    return kExitRefused;
  }
  wf::ActivateArgs args;
  args.id = wf::ReservationId(id);
  args.generation = wf::ReservationGeneration(generation);
  args.authority = wf::ActivationAuthority{info.authority.activationGeneration, info.fence};
  args.now = commandInstant(command);

  wf::SpectrumReservation reservation;
  const wf::Status status =
      activating ? session.activate(args, reservation) : session.deactivate(args, reservation);
  printStatus(verb, status);
  if (!status.ok()) return kExitRefused;
  printReservation(reservation);
  if (activating && session.durableState()) {
    std::cout << "activate: activation evidence is not durable, so this reservation returns as "
                 "reserved with a fresh generation when the image is loaded again; deactivate or "
                 "release it in this process, or drive a long-lived runtime in server mode\n";
  }
  return kExitOk;
}

int commandRelease(Session& session, const Command& command) {
  if (command.args.size() != 1) {
    return usageError("release takes one spec: <reservationId>:<generation>");
  }
  const std::vector<std::string_view> fields = split(command.args[0], ':');
  if (fields.size() != 2) return usageError("a release spec has exactly 2 fields");
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  if (!parseIdentity(fields[0], id)) {
    return usageError("reservation id must be a non-zero integer");
  }
  if (!parseIdentity(fields[1], generation)) {
    return usageError("reservation generation must be a non-zero integer");
  }

  SessionInfo info;
  const wf::Status infoStatus = session.info(info);
  if (!infoStatus.ok()) {
    printStatus("release", infoStatus);
    return kExitRefused;
  }
  wf::ReleaseArgs args;
  args.id = wf::ReservationId(id);
  args.generation = wf::ReservationGeneration(generation);
  args.authority = wf::ReleaseAuthority{info.authority.releaseGeneration, info.fence};
  args.now = commandInstant(command);

  wf::SpectrumReservation reservation;
  const wf::Status status = session.release(args, reservation);
  printStatus("release", status);
  if (!status.ok()) return kExitRefused;
  printReservation(reservation);
  return kExitOk;
}

// Parses the instant a sweep runs at: the trailing @<nanos> instant wins over
// the positional instant, which wins over the host wall clock.
[[nodiscard]] bool sweepInstant(const Command& command, wf::Instant& out, std::string& error) {
  if (command.at.has_value()) {
    out = *command.at;
    return true;
  }
  if (command.args.empty()) {
    out = wf::systemNow();
    return true;
  }
  if (command.args.size() > 1) {
    error = "a sweep takes at most one positional instant";
    return false;
  }
  if (!parseInstant(command.args[0], out)) {
    error = "the sweep instant must be a non-negative integer number of nanoseconds";
    return false;
  }
  return true;
}

int commandSweep(Session& session, const Command& command, bool reclaiming) {
  const char* const verb = reclaiming ? "reclaim" : "expire";
  wf::Instant now;
  std::string error;
  if (!sweepInstant(command, now, error)) return usageError(error);

  wf::SweepArgs args;
  args.now = now;
  wf::ReclaimReport report;
  const wf::Status status =
      reclaiming ? session.reclaimExpired(args, report) : session.expireLeases(args, report);
  printStatus(verb, status);
  std::cout << verb << ' ' << report.describe() << '\n';
  if (!status.ok()) return kExitRefused;
  return kExitOk;
}

// ---------------------------------------------------------------------------
// Commands: inspection
// ---------------------------------------------------------------------------

int commandShow(Session& session, const Command& command) {
  if (command.args.size() != 1) {
    return usageError("show takes one subject: reservations, usage, audit, stats or fence");
  }
  const std::string_view subject = command.args[0];
  const wf::Instant at = commandInstant(command);

  if (subject == "reservations") {
    std::vector<wf::SpectrumReservation> all;
    const wf::Status status = session.reservations(all);
    if (!status.ok()) {
      printStatus("show reservations", status);
      return kExitRefused;
    }
    std::cout << "show reservations: total=" << all.size() << '\n';
    const std::size_t printed =
        all.size() < kMaxPrintedReservations ? all.size() : kMaxPrintedReservations;
    for (std::size_t index = 0; index < printed; ++index) printReservation(all[index]);
    if (all.size() > printed) {
      std::cout << "show reservations: " << (all.size() - printed)
                << " further reservation(s) not printed\n";
    }
    return kExitOk;
  }

  if (subject == "usage") {
    std::vector<wf::SpectrumUsage> usage;
    const wf::Status status = session.usageAll(at, usage);
    if (!status.ok()) {
      printStatus("show usage", status);
      return kExitRefused;
    }
    std::cout << "show usage: at=" << wf::renderInstant(at) << " domains=" << usage.size() << '\n';
    for (const wf::SpectrumUsage& record : usage) printUsageRecord(record);
    return kExitOk;
  }

  if (subject == "audit") {
    SessionInfo info;
    const wf::Status infoStatus = session.info(info);
    if (!infoStatus.ok()) {
      printStatus("show audit", infoStatus);
      return kExitRefused;
    }
    const std::uint64_t last = info.lastAudit.raw();
    const std::uint64_t window = kMaxPrintedAuditRecords;
    const std::uint64_t since = last > window ? last - window : 0;
    std::vector<wf::AuditRecord> records;
    const wf::Status status = session.audit(since, 0, records);
    if (!status.ok()) {
      printStatus("show audit", status);
      return kExitRefused;
    }
    std::cout << "show audit: lastSequence=" << last << " since=" << since
              << " records=" << records.size() << " retained=" << info.auditRecords << '\n';
    for (const wf::AuditRecord& record : records) {
      std::cout << "audit " << record.describe() << '\n';
    }
    return kExitOk;
  }

  if (subject == "stats") {
    SessionInfo info;
    const wf::Status status = session.info(info);
    if (!status.ok()) {
      printStatus("show stats", status);
      return kExitRefused;
    }
    std::cout << "show stats: " << wf::describeRuntime(info.stats) << '\n';
    std::cout << "show stats: counters describe the runtime that answered this command; an "
                 "in-process runtime that loaded a durable image starts them at zero\n";
    return kExitOk;
  }

  if (subject == "fence") {
    SessionInfo info;
    const wf::Status status = session.info(info);
    if (!status.ok()) {
      printStatus("show fence", status);
      return kExitRefused;
    }
    std::cout << "show fence: " << wf::renderFence(info.fence) << '\n';
    std::cout << "show fence: epoch=" << info.fence.epoch.raw()
              << " incarnation=" << info.fence.incarnation.raw()
              << " runtimeGeneration=" << info.runtimeGeneration.raw()
              << " recoveryGeneration=" << info.recoveryGeneration.raw() << '\n';
    if (info.authorityKnown) {
      std::cout << "show fence: authority eligibility="
                << info.authority.eligibilityGeneration.raw()
                << " reservation=" << info.authority.reservationGeneration.raw()
                << " activation=" << info.authority.activationGeneration.raw()
                << " release=" << info.authority.releaseGeneration.raw() << '\n';
    } else {
      std::cout << "show fence: authority generations are not exposed by the transport "
                   "protocol; tokens carry generation 1 under the fence above\n";
    }
    std::cout << "show fence: grids=" << info.grids << " domains=" << info.domains
              << " exclusionDomains=" << info.exclusionDomains
              << " capabilities=" << info.capabilities
              << " reservations=" << info.reservations
              << " auditRecords=" << info.auditRecords
              << " lastAudit=" << info.lastAudit.raw() << '\n';
    return kExitOk;
  }

  return usageError("show subject '" + std::string(subject) +
                    "' is not reservations, usage, audit, stats or fence");
}

int commandSave(Session& session, const Command& command) {
  if (!command.args.empty()) return usageError("save takes no arguments");
  const wf::Status status = session.save();
  printStatus("save", status);
  if (!status.ok()) return kExitRefused;
  std::cout << "save: the durable image is current at " << wf::renderInstant(commandInstant(command))
            << '\n';
  return kExitOk;
}

int commandRecover(Session& session, const Command& command) {
  if (!command.args.empty()) return usageError("recover takes no arguments");
  if (session.remote()) {
    return usageError("recover is a local-mode command; a server owns its own durable state and "
                      "loads it when it starts");
  }
  wf::RecoveryReport report;
  const wf::Status status = session.recover(report);
  printStatus("recover", status);
  std::cout << "recover " << report.describe() << '\n';
  if (!status.ok()) return kExitRefused;
  return kExitOk;
}

int commandReset(Session& session, const Command& command) {
  if (!command.args.empty()) return usageError("reset takes no arguments");
  const wf::Status status = session.reset();
  printStatus("reset", status);
  if (!status.ok()) return kExitRefused;
  std::cout << "reset: the in-memory state is cleared; any durable image is unchanged until the "
               "next save\n";
  return kExitOk;
}

int commandInit(Session& session, const Command& command) {
  if (!command.args.empty()) return usageError("init takes no arguments");
  SessionInfo info;
  const wf::Status status = session.info(info);
  if (!status.ok()) {
    printStatus("init", status);
    return kExitRefused;
  }
  std::cout << "init: mode=" << (info.remote ? "server" : "local")
            << " " << wf::renderFence(info.fence) << '\n';
  std::cout << "init: grids=" << info.grids << " domains=" << info.domains
            << " exclusionDomains=" << info.exclusionDomains
            << " capabilities=" << info.capabilities
            << " reservations=" << info.reservations
            << " auditRecords=" << info.auditRecords << '\n';
  return kExitOk;
}

// ---------------------------------------------------------------------------
// Self test
//
// A deterministic in-process scenario over a private runtime: register,
// publish, allocate, refuse a conflict, release, reclaim. Every step is checked
// against the exact expected token, and no step reads the wall clock.
// ---------------------------------------------------------------------------

constexpr std::uint64_t kSelfGridId = 900;
constexpr std::uint64_t kSelfDomainId = 900;

struct SelfTest {
  int checks{0};
  int failures{0};

  void check(bool condition, const std::string& what) {
    checks += 1;
    std::cout << "selftest " << (condition ? "PASS" : "FAIL") << ' ' << what << '\n';
    if (!condition) failures += 1;
  }
};

[[nodiscard]] wf::SpectrumRequest selfTestRequest(wf::SpectrumRuntime& runtime,
                                                  std::uint64_t requestId, std::uint32_t slots,
                                                  wf::Instant at, wf::Duration lease) {
  const wf::AuthorityState authority = runtime.authorityState();
  wf::SpectrumRequest request;
  request.requestId = wf::AllocationRequestId(requestId);
  request.requestGeneration = wf::AllocationRequestGeneration(1);
  request.owner = wf::OwnerId(1);
  request.ownerGeneration = wf::OwnerGeneration(1);
  request.domains = {wf::SpectrumDomainId(kSelfDomainId)};
  request.domainGenerations = {wf::SpectrumDomainGeneration(1)};
  request.grid = wf::ChannelGridId(kSelfGridId);
  request.gridGeneration = wf::GridGeneration(1);
  request.slots = slots;
  request.contiguity = wf::ContiguityRequirement::Required;
  request.continuity = wf::ContinuityRequirement::Required;
  request.leaseDuration = lease;
  request.requestedAt = at;
  request.notBefore = wf::Instant{};
  request.eligibilityAuthority =
      wf::EligibilityAuthority{authority.eligibilityGeneration, authority.fence};
  request.reservationAuthority =
      wf::ReservationAuthority{authority.reservationGeneration, authority.fence};
  return request;
}

int commandSelfTest(Session& session, const Command& command) {
  if (!command.args.empty()) return usageError("selftest takes no arguments");
  if (session.remote()) {
    return usageError("selftest is a local-mode command; it runs a deterministic scenario in a "
                      "private in-process runtime");
  }

  SelfTest test;
  wf::SpectrumRuntime runtime;
  const wf::Instant at = wf::Instant::fromSeconds(1'700'000'000);
  const wf::Duration lease = wf::Duration::seconds(10);

  // A flex grid of eight 50 GHz slots whose channels may occupy one to four
  // slots, so the scenario can exercise both a single slot channel and a wider
  // one. The runtime refuses a width the grid cannot express.
  wf::ChannelGrid grid;
  grid.id = wf::ChannelGridId(kSelfGridId);
  grid.generation = wf::GridGeneration(1);
  grid.kind = wf::GridKind::Flex;
  grid.anchorMhz = 191'400'000;
  grid.slotWidthMhz = 50'000;
  grid.slotCount = 8;
  grid.minSlotsPerChannel = 1;
  grid.maxSlotsPerChannel = 4;
  const wf::Status gridStatus = runtime.registerGrid(grid);
  test.check(gridStatus.ok(), "register grid: " + std::string(wf::toToken(gridStatus.code)));

  wf::SpectrumDomain domain;
  domain.id = wf::SpectrumDomainId(kSelfDomainId);
  domain.generation = wf::SpectrumDomainGeneration(1);
  domain.klass = wf::ResourceClass::AbstractDomain;
  domain.grid = grid.id;
  domain.gridGeneration = grid.generation;
  const wf::Status domainStatus = runtime.registerDomain(domain);
  test.check(domainStatus.ok(), "register domain: " + std::string(wf::toToken(domainStatus.code)));

  wf::SpectrumCapability capability;
  capability.domain = domain.id;
  capability.domainGeneration = domain.generation;
  capability.grid = grid.id;
  capability.gridGeneration = grid.generation;
  capability.support = wf::SpectrumSupport::Supported;
  capability.firstAllocatableSlot = 0;
  capability.allocatableSlots = grid.slotCount;
  capability.minTunableMhz = grid.anchorMhz;
  capability.maxTunableMhz = grid.endMhz();
  capability.presenceEvidence.present = true;
  capability.presenceEvidence.digest = 1;
  capability.presenceEvidence.source = "wfcli selftest";
  capability.generation = wf::CapabilityGeneration(1);
  capability.publisher = wf::ControllerId(1);
  capability.fence = runtime.fence();
  capability.publishedAt = at;
  const wf::Status capabilityStatus = runtime.publishCapability(capability);
  test.check(capabilityStatus.ok(),
             "publish capability: " + std::string(wf::toToken(capabilityStatus.code)));

  const wf::AllocationDecision first =
      runtime.allocate(selfTestRequest(runtime, 1, 1, at, lease));
  test.check(first.allocated() && first.reservation == wf::ReservationId(1),
             "allocate 1 slot: outcome=" + std::string(wf::toToken(first.outcome)) +
                 " reservation=" + std::to_string(first.reservation.raw()));

  wf::SpectrumRequest conflicting = selfTestRequest(runtime, 2, 1, at, lease);
  conflicting.frequencyWindows = {first.explanation.selected.frequency};
  const wf::AllocationDecision refused = runtime.allocate(conflicting);
  const bool refusedAsConflict = !refused.allocated() &&
                                 refused.outcome == wf::AllocationOutcome::RefusedConflict &&
                                 refused.explanation.conflicts ==
                                     std::vector<wf::ReservationId>{wf::ReservationId(1)};
  test.check(refusedAsConflict,
             "refuse a conflicting request: outcome=" +
                 std::string(wf::toToken(refused.outcome)) + " conflicts=" +
                 std::to_string(refused.explanation.conflicts.size()));

  const wf::AuthorityState authority = runtime.authorityState();
  const wf::ReleaseAuthority releaseAuthority{authority.releaseGeneration, authority.fence};
  const wf::Status released =
      runtime.release(wf::ReservationId(1), wf::ReservationGeneration(1), releaseAuthority, at);
  const std::optional<wf::SpectrumReservation> releasedValue =
      runtime.reservation(wf::ReservationId(1));
  test.check(released.ok() && releasedValue.has_value() &&
                 releasedValue->state == wf::ReservationState::Released,
             "release: status=" + std::string(wf::toToken(released.code)));

  const wf::AllocationDecision third = runtime.allocate(selfTestRequest(runtime, 3, 2, at, lease));
  test.check(third.allocated(), "allocate 2 slots after release: outcome=" +
                                    std::string(wf::toToken(third.outcome)));

  const wf::ReclaimReport report = runtime.reclaimExpired(at + wf::Duration::seconds(11));
  const std::optional<wf::SpectrumUsage> usage =
      runtime.usage(wf::SpectrumDomainId(kSelfDomainId), at + wf::Duration::seconds(11));
  const bool reclaimed = report.reclaimed == std::vector<wf::ReservationId>{third.reservation} &&
                         usage.has_value() && usage->freeSlots == grid.slotCount &&
                         usage->liveSlots == 0;
  test.check(reclaimed,
             "reclaim an expired lease: reclaimed=" + std::to_string(report.reclaimed.size()) +
                 " freeSlots=" + (usage.has_value() ? std::to_string(usage->freeSlots)
                                                    : std::string("unknown")));

  std::cout << "selftest: " << (test.failures == 0 ? "PASS" : "FAIL") << " checks="
            << test.checks << " failures=" << test.failures << '\n';
  return test.failures == 0 ? kExitOk : kExitRefused;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

// Commands that change what the runtime owns or knows. With a durable image
// configured, a successful one is persisted before the process exits so the
// next invocation sees it. "reset" is deliberately not one of them: it clears
// the in-memory state only, and the durable image survives until an explicit
// save. "allocate" is not one either, because a durable runtime already
// persists a committed allocation before the allocating call returns.
[[nodiscard]] bool isMutatingCommand(std::string_view name) noexcept {
  return name == "register-grid" || name == "register-domain" || name == "register-exclusion" ||
         name == "publish-capability" || name == "renew" || name == "activate" ||
         name == "deactivate" || name == "release" || name == "expire" || name == "reclaim";
}

int dispatch(Session& session, const Command& command) {
  if (command.name == "init") return commandInit(session, command);
  if (command.name == "register-grid") return commandRegisterGrid(session, command);
  if (command.name == "register-domain") return commandRegisterDomain(session, command);
  if (command.name == "register-exclusion") return commandRegisterExclusion(session, command);
  if (command.name == "publish-capability") return commandPublishCapability(session, command);
  if (command.name == "enumerate") return commandEnumerate(session, command);
  if (command.name == "explain") return commandExplain(session, command);
  if (command.name == "allocate") return commandAllocate(session, command);
  if (command.name == "renew") return commandRenew(session, command);
  if (command.name == "activate") return commandActivation(session, command, true);
  if (command.name == "deactivate") return commandActivation(session, command, false);
  if (command.name == "release") return commandRelease(session, command);
  if (command.name == "expire") return commandSweep(session, command, false);
  if (command.name == "reclaim") return commandSweep(session, command, true);
  if (command.name == "show") return commandShow(session, command);
  if (command.name == "save") return commandSave(session, command);
  if (command.name == "recover") return commandRecover(session, command);
  if (command.name == "reset") return commandReset(session, command);
  if (command.name == "selftest") return commandSelfTest(session, command);
  return usageError("unknown command '" + command.name + "'");
}

void printUsage() {
  std::cout <<
      "wfcli: inspection and control utility for the Wavelength Fabric runtime\n"
      "\n"
      "  wfcli local  [--state <path>] <command> [arguments] [@<nanos>]\n"
      "  wfcli server --host <host> --port <port> <command> [arguments] [@<nanos>]\n"
      "  wfcli <command> [...]         the same command against a local runtime\n"
      "\n"
      "  local   drives an in-process SpectrumRuntime. With --state the runtime\n"
      "          loads that durable image at startup, persists every committed\n"
      "          allocation before it returns, and persists every other successful\n"
      "          change before the process exits. 'reset' clears memory only: the\n"
      "          durable image survives until an explicit save.\n"
      "  server  drives a runtime owned by another process through the framed\n"
      "          loopback TCP client. The transport exposes no domain lookup, so\n"
      "          name the generation explicitly as <domainId>@<generation>.\n"
      "\n"
      "Commands\n"
      "  init                          create the runtime and load the durable image\n"
      "  register-grid <spec>          fixed:<id>:<gen>:<anchorMhz>:<slotWidthMhz>:<slots>\n"
      "                                flex:<id>:<gen>:<anchorMhz>:<slotWidthMhz>:<slots>:"
      "<min>:<max>\n"
      "  register-domain <spec>        <id>:<generation>:<gridId>:<gridGen>"
      "[:contiguous|fragmented]\n"
      "  register-exclusion <spec>     <id>:<generation>:<guardMhz>:<domainId,...>\n"
      "  publish-capability <spec>     <domain>:<domainGen>:<grid>:<gridGen>:\n"
      "                                <supported|unsupported|unknown>:<firstSlot>:<slots>:\n"
      "                                <minTunableMhz>:<maxTunableMhz>\n"
      "  enumerate <spec>              report the ordered candidate list without committing\n"
      "  explain <spec>                report the typed outcome without committing (dry run)\n"
      "  allocate <spec>               commit an allocation and print the typed outcome\n"
      "  renew <spec>                  <reservationId>:<generation>:<extensionSeconds>\n"
      "  activate <spec>               <reservationId>:<generation>\n"
      "  deactivate <spec>             <reservationId>:<generation>\n"
      "  release <spec>                <reservationId>:<generation>\n"
      "  expire [<nowNanos>]           mark every lapsed lease expired\n"
      "  reclaim [<nowNanos>]          reclaim every expired reservation\n"
      "  show reservations             every reservation this runtime knows\n"
      "  show usage                    per-domain occupancy accounting\n"
      "  show audit                    the most recent audit records\n"
      "  show stats                    runtime counters\n"
      "  show fence                    controller fence, generations and counts\n"
      "  save                          write the durable image\n"
      "  recover                       reload the durable image (local mode only)\n"
      "  reset                         clear the in-memory state\n"
      "  selftest                      run the deterministic in-process scenario\n"
      "\n"
      "  A request spec is <domainList>:<grid>:<gridGen>:<slots>:<contiguity>:<continuity>:\n"
      "  <leaseSeconds>, where <domainList> is a comma-separated list of <id> or\n"
      "  <id>@<generation> and the requirement tokens are required, not-required or\n"
      "  unspecified.\n"
      "\n"
      "  A trailing @<nanos> instant overrides the wall clock for commands that need\n"
      "  a time; expire and reclaim also accept a positional instant.\n"
      "\n"
      "  The runtime refuses a channel width the request grid cannot express: a\n"
      "  fixed grid carries exactly one slot per channel, and a flex grid is\n"
      "  bounded by its <min> and <max> slots per channel.\n"
      "\n"
      "  A reservation that is active when a durable process exits is restored as\n"
      "  reserved with a fresh generation, because activation evidence is not durable.\n"
      "  Deactivate or release it in the process that activated it, or drive a\n"
      "  long-lived runtime in server mode.\n"
      "\n"
      "Exit codes: 0 accepted, 1 refused or failed, 2 unusable command line.\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
  if (arguments.empty()) {
    printUsage();
    return kExitUsage;
  }

  std::string_view mode = arguments[0];
  if (mode == "help" || mode == "--help" || mode == "-h") {
    printUsage();
    return kExitOk;
  }
  std::size_t index = 1;
  if (mode != "local" && mode != "server") {
    // The two modes are "local" and "server". A bare command name is taken as
    // local mode, so the common single-command case does not need the keyword.
    mode = "local";
    index = 0;
  }
  std::string statePath;
  std::string host;
  std::uint16_t port = 0;
  while (index < arguments.size()) {
    const std::string_view option = arguments[index];
    if (option != "--state" && option != "--host" && option != "--port") break;
    if (index + 1 >= arguments.size()) {
      return usageError(std::string(option) + " requires a value");
    }
    const std::string_view value = arguments[index + 1];
    if (option == "--state") {
      if (mode != "local") {
        return usageError("--state is a local-mode option; a server owns its own durable state");
      }
      statePath = std::string(value);
    } else if (option == "--host") {
      if (mode != "server") return usageError("--host is a server-mode option");
      host = std::string(value);
    } else {
      if (mode != "server") return usageError("--port is a server-mode option");
      if (!parseU16(value, port) || port == 0) {
        return usageError("--port must be a port number in [1, 65535]");
      }
    }
    index += 2;
  }
  if (mode == "server" && (host.empty() || port == 0)) {
    return usageError("server mode requires --host <host> and --port <port>");
  }
  if (index >= arguments.size()) return usageError("no command given");

  Command command;
  command.name = std::string(arguments[index]);
  ++index;
  for (; index < arguments.size(); ++index) {
    const std::string_view token = arguments[index];
    if (!token.empty() && token.front() == '@') {
      if (index + 1 != arguments.size()) {
        return usageError("the @<nanos> instant must be the last argument");
      }
      wf::Instant at;
      if (!parseInstant(token.substr(1), at)) {
        return usageError("@<nanos> requires a non-negative integer nanosecond instant");
      }
      command.at = at;
      break;
    }
    command.args.push_back(token);
  }
  if (command.name.size() > 2 && command.name[0] == '-' && command.name[1] == '-') {
    return usageError("unrecognised option '" + command.name + "'");
  }

  std::unique_ptr<Session> session;
  if (mode == "local") {
    std::unique_ptr<LocalSession> local(new LocalSession(statePath));
    // "recover" performs the recovery itself, so it must not be loaded twice.
    if (command.name != "recover") {
      wf::RecoveryReport report;
      const wf::Status status = local->load(report);
      if (!status.ok()) {
        printStatus("load", status);
        std::cout << "load " << report.describe() << '\n';
        return kExitRefused;
      }
      if (report.recovered) {
        std::cout << "load: source=" << statePath << " recovered=true records="
                  << report.recordsRead << '/' << report.recordsAccepted << '/'
                  << report.recordsRejected << " grids=" << report.gridsRestored
                  << " domains=" << report.domainsRestored
                  << " capabilities=" << report.capabilitiesRestored
                  << " reservations=" << report.reservationsRestored
                  << " demotedFromActive=" << report.demotedFromActive << '\n';
      }
    }
    session = std::move(local);
  } else {
    std::unique_ptr<RemoteSession> remote(new RemoteSession(host, port));
    const wf::Status status = remote->connect();
    if (!status.ok()) {
      printStatus("connect", status);
      std::cout << "connect: host=" << host << " port=" << port << '\n';
      return kExitRefused;
    }
    session = std::move(remote);
  }

  const int code = dispatch(*session, command);
  if (code == kExitOk && !statePath.empty() && isMutatingCommand(command.name)) {
    const wf::Status saved = session->save();
    if (!saved.ok()) {
      printStatus("persist", saved);
      std::cout << "persist: the command took effect in memory but is not durable\n";
      return kExitRefused;
    }
    std::cout << "persist: state written to " << statePath << '\n';
  }
  return code;
}
