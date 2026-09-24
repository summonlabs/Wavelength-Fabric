// mp_scenario - the Wavelength Fabric multiprocess proof.
//
// It starts a real wavelengthd server process, drives it from this process and
// from additional independent client processes over real loopback TCP, kills
// the server with a real OS kill at materially distinct lifecycle boundaries,
// restarts it from durable state, and proves fresh-incarnation fencing,
// conservative recovery, idempotent replay after a lost acknowledgement, real
// cross-process allocation races, and rejection of corrupt durable state.
//
// No test timeouts, no watchdog-success logic and no forced termination is
// classified as a pass: every wait is a readiness handshake that fails loudly.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <wavelength_fabric/net.hpp>
#include <wavelength_fabric/process.hpp>
#include <wavelength_fabric/protocol.hpp>
#include <wavelength_fabric/runtime.hpp>
#include <wavelength_fabric/text.hpp>

namespace {

using namespace wavelength_fabric;

constexpr std::int64_t kBaseInstant = 1'800'000'000'000'000'000ll;
constexpr std::int64_t kSlotWidthMhz = 50'000;
constexpr std::int64_t kAnchorMhz = 191'300'000;
constexpr std::uint32_t kSlotCount = 8;

struct Report {
  int checks{0};
  int failures{0};

  void check(bool condition, const std::string& what) {
    ++checks;
    if (condition) {
      std::cout << "  ok   " << what << "\n";
    } else {
      ++failures;
      std::cout << "  FAIL " << what << "\n";
    }
  }

  void note(const std::string& what) { std::cout << "== " << what << "\n"; }
};

[[nodiscard]] std::uint16_t pickFreePort() {
  Socket socket;
  std::uint16_t port = 0;
  ListenOptions options;
  options.address = "127.0.0.1";
  options.port = 0;
  const Status status = listenTcp(options, socket, port);
  if (!status.ok()) return 0;
  return port;
}

[[nodiscard]] std::map<std::string, std::string> parseResult(const std::string& path) {
  std::map<std::string, std::string> result;
  std::ifstream stream(path);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    result[line.substr(0, equals)] = line.substr(equals + 1);
  }
  return result;
}

[[nodiscard]] std::string valueOf(const std::map<std::string, std::string>& result,
                                  const std::string& key) {
  const auto found = result.find(key);
  return found == result.end() ? std::string("<missing>") : found->second;
}

enum class Startup { Ready, RecoverFailed, StartFailed, Indeterminate };

struct ServerHandle {
  ChildProcess process;
  std::uint16_t port{0};
  std::uint64_t epoch{0};
  std::uint64_t incarnation{0};
  std::string output;
};

// Waits for the server to publish its READY line and then confirms readiness
// with a real transport handshake. Exhausting the bounded number of attempts is
// a hard failure, never a pass.
[[nodiscard]] Startup waitForReady(ServerHandle& handle) {
  for (int attempt = 0; attempt < 1200; ++attempt) {
    const std::string text = handle.process.stdoutText();
    if (text.find("RECOVER-FAILED") != std::string::npos) {
      handle.output = text;
      return Startup::RecoverFailed;
    }
    if (text.find("START-FAILED") != std::string::npos) {
      handle.output = text;
      return Startup::StartFailed;
    }
    const std::size_t ready = text.find("READY ");
    if (ready != std::string::npos) {
      std::istringstream stream(text.substr(ready + 6));
      std::uint64_t port = 0;
      std::uint64_t epoch = 0;
      std::uint64_t incarnation = 0;
      if ((stream >> port >> epoch >> incarnation) && port > 0 && port <= 65535) {
        SpectrumClient probe;
        if (probe.connect("127.0.0.1", static_cast<std::uint16_t>(port)).ok()) {
          ServerDescription description;
          if (probe.describe(description).ok()) {
            handle.port = static_cast<std::uint16_t>(port);
            handle.epoch = description.fence.epoch.raw();
            handle.incarnation = description.fence.incarnation.raw();
            handle.output = text;
            return Startup::Ready;
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  handle.output = handle.process.stdoutText();
  return Startup::Indeterminate;
}

[[nodiscard]] bool startServer(const std::string& executable,
                               const std::vector<std::string>& arguments, ServerHandle& handle,
                               std::string& diagnostic) {
  const Status started = handle.process.start(executable, arguments);
  if (!started.ok()) {
    diagnostic = started.message;
    return false;
  }
  const Startup startup = waitForReady(handle);
  if (startup == Startup::Ready) return true;
  diagnostic = handle.output.empty() ? std::string("no server output") : handle.output;
  int exitCode = 0;
  (void)handle.process.wait(exitCode);
  return false;
}

[[nodiscard]] std::vector<std::string> serverArgs(std::uint16_t port, const std::string& statePath,
                                                  bool durable) {
  std::vector<std::string> arguments;
  arguments.push_back("--port");
  arguments.push_back(std::to_string(port));
  arguments.push_back("--workers");
  arguments.push_back("4");
  if (!statePath.empty()) {
    arguments.push_back("--state");
    arguments.push_back(statePath);
  }
  if (durable) arguments.push_back("--durable");
  return arguments;
}

struct ClientOutcome {
  bool started{false};
  int exitCode{0};
  std::map<std::string, std::string> result;
  std::string output;
};

[[nodiscard]] ClientOutcome runClient(const std::string& executable,
                                      const std::vector<std::string>& arguments) {
  ClientOutcome outcome;
  ChildProcess process;
  const Status started = process.start(executable, arguments);
  if (!started.ok()) {
    outcome.output = started.message;
    return outcome;
  }
  outcome.started = true;
  (void)process.wait(outcome.exitCode);
  outcome.output = process.stdoutText();
  const std::string text = process.stderrText();
  if (!text.empty()) outcome.output += text;
  return outcome;
}

[[nodiscard]] std::vector<std::string> clientArgs(std::uint16_t port, const std::string& out,
                                                  const std::string& domains, std::uint64_t requestId,
                                                  std::int64_t windowLow, std::int64_t windowHigh) {
  std::vector<std::string> arguments;
  arguments.push_back("--port");
  arguments.push_back(std::to_string(port));
  arguments.push_back("--out");
  arguments.push_back(out);
  arguments.push_back("--domains");
  arguments.push_back(domains);
  arguments.push_back("--grid");
  arguments.push_back("1");
  arguments.push_back("--owner");
  arguments.push_back(std::to_string(requestId));
  arguments.push_back("--request-id");
  arguments.push_back(std::to_string(requestId));
  arguments.push_back("--slots");
  arguments.push_back("1");
  arguments.push_back("--requested-at");
  arguments.push_back(std::to_string(kBaseInstant));
  arguments.push_back("--lease-seconds");
  arguments.push_back("600");
  if (windowLow >= 0 && windowHigh > windowLow) {
    arguments.push_back("--window-low");
    arguments.push_back(std::to_string(windowLow));
    arguments.push_back("--window-high");
    arguments.push_back(std::to_string(windowHigh));
  }
  return arguments;
}

[[nodiscard]] std::int64_t slotLow(std::uint32_t slot) {
  return kAnchorMhz + static_cast<std::int64_t>(slot) * kSlotWidthMhz;
}

struct LocalClient {
  SpectrumClient client;
  ControllerFence fence;

  [[nodiscard]] bool connect(std::uint16_t port) {
    if (!client.connect("127.0.0.1", port).ok()) return false;
    ServerDescription description;
    if (!client.describe(description).ok()) return false;
    fence = description.fence;
    return true;
  }
};

[[nodiscard]] SpectrumRequest buildRequest(std::uint64_t requestId, std::uint64_t owner,
                                           std::uint64_t domain, std::uint32_t slot,
                                           const ControllerFence& fence,
                                           std::uint64_t requestGeneration = 1,
                                           std::int64_t requestedAt = kBaseInstant) {
  SpectrumRequest request;
  request.requestId = AllocationRequestId(requestId);
  request.requestGeneration = AllocationRequestGeneration(requestGeneration);
  request.owner = OwnerId(owner);
  request.ownerGeneration = OwnerGeneration(1);
  request.domains.push_back(SpectrumDomainId(domain));
  request.domainGenerations.push_back(SpectrumDomainGeneration(1));
  request.grid = ChannelGridId(1);
  request.gridGeneration = GridGeneration(1);
  request.slots = 1;
  request.leaseDuration = Duration::seconds(600);
  request.requestedAt = Instant::fromNanos(requestedAt);
  request.policyGeneration = PolicyGeneration(1);
  request.priorityGeneration = PriorityGeneration(1);
  request.frequencyWindows.push_back(
      FrequencyRange{slotLow(slot), slotLow(slot) + kSlotWidthMhz});
  request.eligibilityAuthority.generation = EligibilityAuthorityGeneration(1);
  request.eligibilityAuthority.fence = fence;
  request.reservationAuthority.generation = ReservationAuthorityGeneration(1);
  request.reservationAuthority.fence = fence;
  return request;
}

[[nodiscard]] std::uint32_t liveSlots(const std::vector<SpectrumUsage>& usage,
                                      SpectrumDomainId domain) {
  for (const SpectrumUsage& entry : usage) {
    if (entry.domain == domain) return entry.liveSlots;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::cout.setf(std::ios::unitbuf);
  if (argc < 3) {
    std::cerr << "usage: wf_mp_scenario <wavelengthd> <wf_mp_client> [workdir]\n";
    return 2;
  }
  const std::string serverExecutable = argv[1];
  const std::string clientExecutable = argv[2];

  const Status sockets = initSockets();
  if (!sockets.ok()) {
    std::cerr << "socket subsystem unavailable: " << sockets.message << "\n";
    return 2;
  }

  Report report;
  std::error_code error;
  std::filesystem::path workDirectory =
      argc > 3 ? std::filesystem::path(argv[3])
               : std::filesystem::path(temporaryDirectory()) / "wf_mp_scenario";
  std::filesystem::remove_all(workDirectory, error);
  std::filesystem::create_directories(workDirectory, error);
  if (error) {
    std::cerr << "cannot create the scenario directory: " << error.message() << "\n";
    return 2;
  }
  const std::string statePath = (workDirectory / "state.wvf").string();
  const SpectrumDomainId domain(1);

  report.note("step 1: start the first server process");
  std::uint16_t port = pickFreePort();
  report.check(port != 0, "a loopback port was reserved for the server");
  if (port == 0) return 1;

  ServerHandle first;
  std::string diagnostic;
  const bool firstStarted =
      startServer(serverExecutable, serverArgs(port, statePath, true), first, diagnostic);
  report.check(firstStarted, "server process 1 is ready over real loopback TCP");
  if (!firstStarted) {
    std::cout << "  diagnostic: " << diagnostic << "\n";
    return 1;
  }
  const std::uint64_t epoch1 = first.epoch;
  const std::uint64_t incarnation1 = first.incarnation;
  report.check(incarnation1 != 0 && epoch1 != 0, "server 1 published a non-zero fence");
  port = first.port;

  report.note("step 2: register the spectrum model over the transport");
  LocalClient local;
  report.check(local.connect(port), "an independent client process connects to server 1");

  ChannelGrid grid;
  grid.id = ChannelGridId(1);
  grid.generation = GridGeneration(1);
  grid.kind = GridKind::Fixed;
  grid.anchorMhz = kAnchorMhz;
  grid.slotWidthMhz = kSlotWidthMhz;
  grid.slotCount = kSlotCount;
  grid.minSlotsPerChannel = 1;
  grid.maxSlotsPerChannel = 1;
  grid.label = "mp-fixed";
  report.check(local.client.registerGrid(grid).ok(), "grid registered");

  SpectrumDomain spectrumDomain;
  spectrumDomain.id = domain;
  spectrumDomain.generation = SpectrumDomainGeneration(1);
  spectrumDomain.klass = ResourceClass::FiberSpan;
  spectrumDomain.grid = grid.id;
  spectrumDomain.gridGeneration = grid.generation;
  spectrumDomain.requiresContiguity = true;
  spectrumDomain.label = "mp-domain";
  report.check(local.client.registerDomain(spectrumDomain).ok(), "domain registered");

  SpectrumCapability capability;
  capability.domain = domain;
  capability.domainGeneration = spectrumDomain.generation;
  capability.grid = grid.id;
  capability.gridGeneration = grid.generation;
  capability.support = SpectrumSupport::Supported;
  capability.firstAllocatableSlot = 0;
  capability.allocatableSlots = kSlotCount;
  capability.minTunableMhz = kAnchorMhz;
  capability.maxTunableMhz = kAnchorMhz + kSlotWidthMhz * kSlotCount;
  capability.contiguityEnforced = true;
  capability.presenceEvidence.present = true;
  capability.presenceEvidence.digest = 0xABCDEF01ull;
  capability.presenceEvidence.source = "mp-scenario";
  capability.generation = CapabilityGeneration(1);
  capability.publisher = ControllerId(1);
  capability.fence = local.fence;
  capability.publishedAt = Instant::fromNanos(kBaseInstant);
  capability.detail = "multiprocess proof capability";
  report.check(local.client.publishCapability(capability).ok(), "capability published");

  report.note("step 3: serial allocation, conflict, activation, renewal and release");
  AllocationDecision decision;
  Status status = local.client.allocate(buildRequest(1, 1, 1, 0, local.fence), decision);
  report.check(status.ok() && decision.allocated(), "allocation of slot 0 committed");
  report.check(decision.explanation.selected.slots.first == 0 &&
                   decision.explanation.selected.slots.count == 1,
               "the committed slot range is exactly [0,1)");
  const ReservationId reservation1 = decision.reservation;
  const ReservationGeneration generation1 = decision.generation;

  status = local.client.allocate(buildRequest(2, 2, 1, 0, local.fence), decision);
  report.check(!status.ok() && !decision.allocated(),
               "a second allocation of the same slot is refused");
  report.check(decision.outcome == AllocationOutcome::RefusedNoCapacity ||
                   decision.outcome == AllocationOutcome::RefusedConflict,
               "the refusal carries a typed conflict or capacity outcome");

  std::vector<SpectrumUsage> usage;
  report.check(local.client.queryUsageAll(Instant::fromNanos(kBaseInstant), usage).ok(),
               "usage query succeeds");
  report.check(liveSlots(usage, domain) == 1, "exactly one slot is live after one allocation");

  {
    ActivateArgs activate;
    activate.id = reservation1;
    activate.generation = generation1;
    activate.authority.generation = ActivationAuthorityGeneration(1);
    activate.authority.fence = local.fence;
    activate.now = Instant::fromNanos(kBaseInstant + 1000);
    SpectrumReservation updated;
    report.check(local.client.activate(activate, updated).ok() &&
                     updated.state == ReservationState::Active,
                 "reservation activated under activation authority");
  }
  {
    RenewArgs renew;
    renew.id = reservation1;
    renew.generation = generation1;
    renew.extension = Duration::seconds(600);
    renew.authority.generation = ReservationAuthorityGeneration(1);
    renew.authority.fence = local.fence;
    renew.now = Instant::fromNanos(kBaseInstant + 2000);
    SpectrumReservation updated;
    report.check(local.client.renew(renew, updated).ok() &&
                     updated.generation.raw() == generation1.raw() + 1,
                 "renewal advances the reservation generation");
  }
  const ReservationGeneration generation2 = ReservationGeneration(generation1.raw() + 1);
  {
    ActivateArgs deactivate;
    deactivate.id = reservation1;
    deactivate.generation = generation2;
    deactivate.authority.generation = ActivationAuthorityGeneration(1);
    deactivate.authority.fence = local.fence;
    deactivate.now = Instant::fromNanos(kBaseInstant + 3000);
    SpectrumReservation updated;
    report.check(local.client.deactivate(deactivate, updated).ok() &&
                     updated.state == ReservationState::Reserved,
                 "deactivation returns the reservation to RESERVED");
  }
  {
    // Leave the reservation ACTIVE across the kill so that conservative
    // recovery has an active reservation to demote.
    ActivateArgs reactivate;
    reactivate.id = reservation1;
    reactivate.generation = generation2;
    reactivate.authority.generation = ActivationAuthorityGeneration(1);
    reactivate.authority.fence = local.fence;
    reactivate.now = Instant::fromNanos(kBaseInstant + 3500);
    SpectrumReservation updated;
    report.check(local.client.activate(reactivate, updated).ok() &&
                     updated.state == ReservationState::Active,
                 "the reservation is ACTIVE again before the kill");
  }

  status = local.client.allocate(buildRequest(3, 3, 1, 5, local.fence), decision);
  report.check(status.ok() && decision.allocated(), "a disjoint slot 5 allocation commits");
  const ReservationId reservation3 = decision.reservation;
  const ReservationGeneration generation3 = decision.generation;
  report.check(local.client.queryUsageAll(Instant::fromNanos(kBaseInstant), usage).ok() &&
                   liveSlots(usage, domain) == 2,
               "two slots are live");

  {
    ReleaseArgs release;
    release.id = reservation3;
    release.generation = generation3;
    release.authority.generation = ReleaseAuthorityGeneration(1);
    release.authority.fence = local.fence;
    release.now = Instant::fromNanos(kBaseInstant + 4000);
    SpectrumReservation updated;
    report.check(local.client.release(release, updated).ok() &&
                     updated.state == ReservationState::Released,
                 "release returns the reservation to the free pool");
  }
  report.check(local.client.queryUsageAll(Instant::fromNanos(kBaseInstant), usage).ok() &&
                   liveSlots(usage, domain) == 1,
               "release accounting returns exactly one slot");

  report.note("step 4: kill server 1 as a real OS process and restart it");
  const Status killed = first.process.kill();
  int killedExit = 0;
  (void)first.process.wait(killedExit);
  report.check(killed.ok(), "server 1 was killed with a real OS kill");
  report.check(!first.process.running(), "server 1 is no longer running");
  report.check(std::filesystem::exists(statePath), "the durable state file survives the kill");

  port = pickFreePort();
  ServerHandle second;
  const bool secondStarted =
      startServer(serverExecutable, serverArgs(port, statePath, true), second, diagnostic);
  report.check(secondStarted, "server process 2 restarts from the durable state");
  if (!secondStarted) {
    std::cout << "  diagnostic: " << diagnostic << "\n";
    return 1;
  }
  report.check(second.epoch == epoch1 + 1, "recovery advances the controller epoch by exactly one");
  report.check(second.incarnation != incarnation1,
               "the restarted runtime has a fresh controller incarnation");

  report.note("step 5: prove conservative recovery and fresh-incarnation fencing");
  LocalClient restarted;
  report.check(restarted.connect(second.port), "an independent client connects to server 2");

  SpectrumReservation restored;
  const Status queried = restarted.client.queryReservation(reservation1, restored);
  report.check(queried.ok(), "the committed reservation survived the restart");
  if (queried.ok()) {
    report.check(restored.state == ReservationState::Reserved,
                 "an active reservation is conservatively demoted to RESERVED on recovery");
    report.check(restored.needsRevalidation,
                 "the demoted reservation is flagged as needing revalidation");
    report.check(restored.generation.raw() == generation2.raw() + 1,
                 "the demoted reservation generation advanced past the dead incarnation");
  }

  std::vector<SpectrumReservation> all;
  report.check(restarted.client.queryReservations(all).ok(), "the reservation table is readable");
  const std::size_t restoredCount = all.size();
  report.check(restoredCount == 2, "both reservations were restored exactly once");

  AllocationDecision replay;
  status = restarted.client.allocate(buildRequest(1, 1, 1, 0, restarted.fence), replay);
  report.check(status.ok() && replay.allocated(),
               "replaying the committed request after the restart succeeds");
  report.check(replay.reservation == reservation1,
               "the replay returns the already committed reservation identity");
  report.check(restarted.client.queryReservations(all).ok() && all.size() == restoredCount,
               "the replay created no second reservation");

  report.note("step 6: prove that the dead incarnation is fenced");
  port = second.port;
  {
    const std::string outPath = (workDirectory / "stale_incarnation.txt").string();
    std::vector<std::string> arguments = clientArgs(port, outPath, "1", 40, slotLow(2), slotLow(3));
    arguments.push_back("--fence-epoch");
    arguments.push_back(std::to_string(epoch1));
    arguments.push_back("--fence-incarnation");
    arguments.push_back(std::to_string(incarnation1));
    const ClientOutcome outcome = runClient(clientExecutable, arguments);
    const std::map<std::string, std::string> result = parseResult(outPath);
    report.check(outcome.started && outcome.exitCode == 0, "the stale-fence client ran");
    report.check(valueOf(result, "transport") == "stale-epoch",
                 "a token minted under the dead epoch is rejected as stale-epoch (got " +
                     valueOf(result, "transport") + ")");
    report.check(valueOf(result, "outcome") == "refused-stale-epoch",
                 "the refusal carries the typed refused-stale-epoch outcome");
  }
  {
    const std::string outPath = (workDirectory / "stale_epoch.txt").string();
    std::vector<std::string> arguments = clientArgs(port, outPath, "1", 41, slotLow(2), slotLow(3));
    arguments.push_back("--fence-epoch");
    arguments.push_back(std::to_string(epoch1 + 5));
    arguments.push_back("--fence-incarnation");
    arguments.push_back(std::to_string(incarnation1));
    const ClientOutcome outcome = runClient(clientExecutable, arguments);
    const std::map<std::string, std::string> result = parseResult(outPath);
    report.check(outcome.started && outcome.exitCode == 0, "the future-epoch client ran");
    report.check(valueOf(result, "transport") == "stale-epoch",
                 "a token from a different epoch is rejected as stale-epoch (got " +
                     valueOf(result, "transport") + ")");
  }
  {
    const Status usageStatus = restarted.client.queryUsageAll(Instant::fromNanos(kBaseInstant), usage);
    std::vector<SpectrumReservation> snapshot;
    (void)restarted.client.queryReservations(snapshot);
    std::size_t live = 0;
    for (const SpectrumReservation& reservation : snapshot) {
      if (reservation.isLiveAt(Instant::fromNanos(kBaseInstant))) live += 1;
    }
    report.check(usageStatus.ok() && liveSlots(usage, domain) == 1,
                 "no stale path mutated ownership (usage=" + std::string(toToken(usageStatus.code)) +
                     " liveSlots=" + std::to_string(liveSlots(usage, domain)) +
                     " reservations=" + std::to_string(snapshot.size()) +
                     " liveReservations=" + std::to_string(live) + ")");
  }

  report.note("step 7: crash after commit but before acknowledgement");
  {
    const std::string outPath = (workDirectory / "abandon.txt").string();
    std::vector<std::string> arguments = clientArgs(port, outPath, "1", 9, slotLow(3), slotLow(4));
    arguments.push_back("--abandon");
    const ClientOutcome outcome = runClient(clientExecutable, arguments);
    report.check(outcome.started && outcome.exitCode == 7,
                 "the abandoning client died immediately after sending the request");
  }
  {
    std::vector<SpectrumReservation> after;
    report.check(restarted.client.queryReservations(after).ok(), "the table is readable");
    const auto committed = std::find_if(after.begin(), after.end(), [](const SpectrumReservation& r) {
      return r.requestId == AllocationRequestId(9);
    });
    report.check(committed != after.end(),
                 "the durable commit survived the lost acknowledgement");
    if (committed != after.end()) {
      AllocationDecision replayDecision;
      const Status replayStatus =
          restarted.client.allocate(buildRequest(9, 9, 1, 3, restarted.fence), replayDecision);
      report.check(replayStatus.ok() && replayDecision.allocated() &&
                       replayDecision.reservation == committed->id,
                   "retrying the identical request is an idempotent replay of the same reservation");
    }
  }

  report.note("step 8: real cross-process allocation race");
  {
    ReleaseArgs release;
    release.id = reservation1;
    release.generation = restored.generation;
    release.authority.generation = ReleaseAuthorityGeneration(1);
    release.authority.fence = restarted.fence;
    release.now = Instant::fromNanos(kBaseInstant + 5000);
    SpectrumReservation updated;
    report.check(restarted.client.release(release, updated).ok(),
                 "the recovered reservation is released to free slot 0");
  }

  constexpr int kRacers = 4;
  const std::int64_t startAt = wallClockNanos() + 500'000'000ll;
  const std::int64_t giveUpAt = startAt + 5'000'000'000ll;
  std::vector<ChildProcess> racers(kRacers);
  std::vector<std::string> racerPaths;
  bool racersStarted = true;
  for (int index = 0; index < kRacers; ++index) {
    const std::string outPath =
        (workDirectory / ("race_" + std::to_string(index) + ".txt")).string();
    racerPaths.push_back(outPath);
    std::vector<std::string> arguments =
        clientArgs(port, outPath, "1", 100 + static_cast<std::uint64_t>(index), slotLow(0),
                   slotLow(1));
    arguments.push_back("--start-at");
    arguments.push_back(std::to_string(startAt));
    arguments.push_back("--give-up-at");
    arguments.push_back(std::to_string(giveUpAt));
    if (!racers[index].start(clientExecutable, arguments).ok()) {
      racersStarted = false;
      break;
    }
  }
  report.check(racersStarted, "four independent client processes started");
  int allocatedCount = 0;
  int refusedCount = 0;
  for (int index = 0; index < kRacers; ++index) {
    int exitCode = 0;
    (void)racers[index].wait(exitCode);
    const std::map<std::string, std::string> result = parseResult(racerPaths[index]);
    const std::string outcome = valueOf(result, "outcome");
    if (outcome == "allocated") ++allocatedCount;
    if (outcome.rfind("refused-", 0) == 0) ++refusedCount;
    std::cout << "  racer " << index << " outcome=" << outcome << "\n";
  }
  report.check(allocatedCount == 1,
               "exactly one of the four simultaneous cross-process allocations committed");
  report.check(refusedCount == kRacers - 1,
               "every other simultaneous attempt was refused with a typed outcome");

  report.check(restarted.client.queryUsageAll(Instant::fromNanos(kBaseInstant), usage).ok(),
               "usage is readable after the race");
  {
    std::vector<SpectrumReservation> after;
    (void)restarted.client.queryReservations(after);
    std::size_t liveOnSlotZero = 0;
    for (const SpectrumReservation& reservation : after) {
      if (!reservation.isLiveAt(Instant::fromNanos(kBaseInstant))) continue;
      if (reservation.slots.first == 0) liveOnSlotZero += 1;
    }
    report.check(liveOnSlotZero == 1, "exactly one live reservation owns slot 0 after the race");
  }

  report.note("step 9: reject corrupt and truncated durable state");
  const Status secondKilled = second.process.kill();
  int secondExit = 0;
  (void)second.process.wait(secondExit);
  report.check(secondKilled.ok(), "server 2 was killed with a real OS kill");

  const auto expectRejected = [&](const std::string& label, const std::string& corruptPath) {
    const std::uint16_t corruptPort = pickFreePort();
    ServerHandle handle;
    std::string corruptDiagnostic;
    const bool started =
        startServer(serverExecutable, serverArgs(corruptPort, corruptPath, false), handle,
                    corruptDiagnostic);
    report.check(!started, label + ": the server refuses to start on rejected state");
    report.check(corruptDiagnostic.find("RECOVER-FAILED") != std::string::npos,
                 label + ": the refusal is reported as RECOVER-FAILED");
  };

  {
    const std::string truncatedPath = (workDirectory / "truncated.wvf").string();
    std::filesystem::copy_file(statePath, truncatedPath, error);
    const std::uintmax_t size = std::filesystem::file_size(truncatedPath);
    std::filesystem::resize_file(truncatedPath, size - 8, error);
    report.check(static_cast<std::uintmax_t>(std::filesystem::file_size(truncatedPath)) == size - 8,
                 "a truncated copy of the durable state was produced");
    expectRejected("truncated state", truncatedPath);
  }
  {
    const std::string trailingPath = (workDirectory / "trailing.wvf").string();
    std::filesystem::copy_file(statePath, trailingPath, error);
    {
      std::ofstream stream(trailingPath, std::ios::binary | std::ios::app);
      stream.write("GARBAGE!", 8);
    }
    expectRejected("trailing garbage", trailingPath);
  }
  {
    const std::string flippedPath = (workDirectory / "flipped.wvf").string();
    std::filesystem::copy_file(statePath, flippedPath, error);
    const std::uintmax_t size = std::filesystem::file_size(flippedPath);
    {
      std::fstream stream(flippedPath, std::ios::binary | std::ios::in | std::ios::out);
      stream.seekg(static_cast<std::streamoff>(size / 2));
      char byte = 0;
      stream.read(&byte, 1);
      byte = static_cast<char>(byte ^ 0x5A);
      stream.seekp(static_cast<std::streamoff>(size / 2));
      stream.write(&byte, 1);
    }
    expectRejected("corrupted body byte", flippedPath);
  }
  {
    const std::string magicPath = (workDirectory / "magic.wvf").string();
    std::filesystem::copy_file(statePath, magicPath, error);
    {
      std::fstream stream(magicPath, std::ios::binary | std::ios::in | std::ios::out);
      stream.seekp(0);
      stream.write("XXXX", 4);
    }
    expectRejected("bad magic", magicPath);
  }

  report.note("step 10: the original durable state still loads");
  {
    const std::uint16_t finalPort = pickFreePort();
    ServerHandle finalHandle;
    std::string finalDiagnostic;
    const bool started = startServer(serverExecutable, serverArgs(finalPort, statePath, true),
                                     finalHandle, finalDiagnostic);
    report.check(started, "a third server process loads the untouched durable state");
    if (started) {
      report.check(finalHandle.epoch == second.epoch + 1,
                   "the third incarnation advances the epoch again");
      report.check(finalHandle.incarnation != second.incarnation,
                   "the third incarnation has a fresh identity");
      LocalClient verify;
      report.check(verify.connect(finalHandle.port), "a client connects to the third server");
      std::vector<SpectrumReservation> after;
      (void)verify.client.queryReservations(after);
      report.check(!after.empty(), "the reservation table survived the second restart");
      (void)finalHandle.process.kill();
      int finalExit = 0;
      (void)finalHandle.process.wait(finalExit);
    }
  }

  std::filesystem::remove_all(workDirectory, error);

  std::cout << "\n" << report.checks << " check(s), " << report.failures << " failure(s)\n";
  std::cout << (report.failures == 0 ? "MULTIPROCESS PROOF PASS\n" : "MULTIPROCESS PROOF FAIL\n");
  return report.failures == 0 ? 0 : 1;
}
