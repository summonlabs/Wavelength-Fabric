# Wavelength Fabric

**Wavelength Fabric is an open-source, vendor-neutral C++20 runtime for wavelength and channel allocation, reservation, authority, conflict detection, and lifecycle over abstract optical spectrum resources.**

**It answers one systems question:**

**Under this exact optical topology and capability generation, who owns a wavelength or channel resource now, across which resources, under what authority, and can another request legally coexist with it?**

Wavelength Fabric models abstract spectrum: domains, spans, ports, channel grids, wavelength and frequency slots, contiguous slot ranges, reservations, leases, generations, epochs, controller incarnations, constraints, and conflict domains. It does not control optical hardware, and it does not claim standards conformance beyond what is implemented and tested here.

## Defining thesis

**A candidate wavelength is not owned because an allocator selected it. It is owned only when an authoritative reservation, minted by the current controller incarnation at the current epoch, commits it against every competing claim and survives every stale replay.**

## Systems boundaries

Wavelength Fabric governs spectrum ownership and nothing else. These adjacent runtimes are explicitly out of scope and are never implemented here:

- **Optical Fabric** owns physical connectivity lifecycle. Wavelength Fabric does not bring a link up, tear it down, or claim that a span is physically usable. It consumes connectivity facts as opaque references on a spectrum domain.
- **Transceiver Registry** owns transceiver identity and inventory. Wavelength Fabric never mints, resolves, or validates a transceiver identity.
- **Optical Path Planner** owns route computation across optical resources. Wavelength Fabric does not compute a path; a caller supplies the ordered list of spectrum domains a request spans.
- **Link Quality Fabric** owns measured link quality. Wavelength Fabric does not measure, estimate, or act on optical signal quality, and it never infers capability from measurement.
- **Fabric Capability Registry** and **Hardware Capability Registry** own device capability discovery. Wavelength Fabric consumes an explicitly supplied capability publication instead.
- **Reservation Fabric** governs advance commitments of compute, memory, bandwidth and residency. Wavelength Fabric governs spectrum only.

Within its boundary, Wavelength Fabric owns: spectrum and channel capability registration, candidate enumeration, deterministic allocation under explicit constraints, contiguous-range requirements, exclusion and conflict domains, reservation, renewal and release, lease expiry and reclamation, activation and deactivation state, explanation of every accepted and refused request, persistence and recovery, and audit history.

## UNSUPPORTED is a first-class outcome

A spectrum domain is allocatable only when a controller incarnation has published a capability that says so, with explicit evidence. The runtime distinguishes three states and never collapses them:

- **SUPPORTED** - the domain may carry allocations inside its declared allocatable window and tunable range, and the publication carried a non-zero evidence digest.
- **UNSUPPORTED** - the domain cannot carry allocations. Every request that spans it is refused with the typed outcome `refused-unsupported`. No synthetic channel is ever created, no candidate is ever emitted for it, and no reservation, usage counter, or statistic is touched by the refusal.
- **UNKNOWN** - no authoritative capability has been established. Requests are refused with `refused-unknown-capability`. UNKNOWN is never treated as SUPPORTED.

Conversion and regeneration capability is never assumed. A request that spans domains on different grids is refused with `refused-conversion` unless every spanned domain published conversion support together with a non-zero evidence digest.

## Authority model

Four authorities are separate types and separate tokens. A token carries the authority generation together with the controller fence (epoch and incarnation) it was minted under.

- **Eligibility authority** decides whether a request may even be considered. Candidate enumeration checks it.
- **Reservation authority** decides whether a candidate becomes owned. Only `allocate` checks it, and only a successful commit creates ownership.
- **Activation authority** decides whether an owned reservation becomes active.
- **Release authority** decides whether owned spectrum is returned.

A token whose epoch or incarnation does not match the current fence is rejected as stale even when its generation is current. A stale generation, an expired lease, or a dead controller incarnation cannot mutate current ownership.

## Identity, generation, epoch and incarnation

Every identity is a distinct strong type: `SpectrumDomainId`, `SpanId`, `PortId`, `ChannelGridId`, `ExclusionDomainId`, `ReservationId`, `AllocationRequestId`, `OwnerId`, `ControllerId`, and a separate generation type for each. They never convert into one another.

- A **generation** advances when an object is superseded. A stale generation never releases, renews, or activates the current reservation.
- An **epoch** advances once per successful durable recovery.
- An **incarnation** is fresh for every runtime construction, including a restart that recovers no durable state.
- A **fence** is the (epoch, incarnation) pair. A fence that does not match the current one is rejected as stale.

## Spectrum model

A **channel grid** is arithmetic, vendor-neutral data: a kind (`fixed` or `flex`), an anchor frequency in MHz, a slot width in MHz, a slot count, and the per-channel slot bounds. A fixed grid carries exactly one slot per channel; a flex grid carries one or more contiguous slots per channel. Frequencies are integer megahertz with checked arithmetic everywhere; slot ranges and frequency ranges are half-open.

A **spectrum domain** is the allocatable spectrum window the runtime governs. A **span** is an abstract conduit and a **port** is an abstract endpoint; both are opaque references supplied by the caller. Nothing in the runtime derives them.

A **capability publication** binds one domain, at one domain generation, on one grid generation, to a support state, an allocatable slot window, a tunable frequency range, a contiguity flag, and explicit evidence. Capability generations are monotonic: the same generation is `duplicate`, an older generation is `stale-generation`.

## Candidate enumeration and deterministic allocation

`enumerateCandidates` is read-only and returns an ordered, bounded list of `SpectrumCandidate` values with the per-domain slot range each one resolves to. Ordering is canonical: by free run in the anchor grid in ascending slot order, then by the candidate's own position. Identical inputs against identical state produce an identical list.

`allocate` commits the **first eligible candidate** in that order. A candidate is eligible only when it is inside the allocatable window and tunable range of every spanned domain, inside every permitted frequency window, clear of every excluded slot and frequency range, clear of every guard band, and clear of every live reservation on the domain or on a member of any exclusion domain that contains it. The candidate list is bounded by `RuntimeConfig::maxCandidatesPerRequest`; a truncated list reports `complete = false` together with the exact number of candidates that were not retained.

A reservation is created only by a commit. The commit drives the guarded state machine `None -> Requested -> Evaluated -> Committing -> Reserved`. If durable commits are enabled and the state cannot be written, the commit is rolled back exactly: no ownership, no reservation record, and no committed statistic survives a commit that did not cross the durable boundary.

## Continuity and contiguity

When a request spans more than one optical resource, `ContinuityRequirement` must be stated explicitly; when a channel occupies more than one slot, `ContiguityRequirement` must be stated explicitly. An unstated requirement on such a request is refused as an invalid request rather than guessed.

- `continuity = required` means every spanned domain receives the same absolute frequency range. If the spanned domains are on different grids, the mapped range must be representable exactly, and conversion evidence is required on every spanned domain.
- `continuity = not-required` means the domains are independent placements of the same channel width.
- The runtime allocates exactly one contiguous slot range per reservation. `contiguity = not-required` on a domain that permits a fragmented channel is refused with `refused-contiguity`, because a contiguous allocation is not what was asked for; every committed reservation therefore occupies one contiguous range.

## Exclusion and conflict domains

An **exclusion domain** groups spectrum domains that must not hold overlapping authoritative frequency allocations at the same time, even across different media. It carries a guard band that is enforced between members. Within one domain, two live reservations conflict when their frequency ranges, each expanded by the larger of the two guard bands, overlap. Across members of an exclusion domain, the exclusion guard band is added. Guard bands keep neighbours separated. A conflict refusal names the blocking reservation identities.

## Reservation lifecycle

`Reserved -> Activating -> Active -> Deactivating -> Reserved`, with `Renewing`, `Releasing`, `Reclaiming`, `Committing` and `Recovering` as transient states, and `Released`, `Expired`, `Reclaimed`, `Superseded`, `Refused` and `Retired` as terminal states. Illegal transitions are rejected deterministically and the reason names both states.

A **lease** records when it was granted, when it expires, how many times it has been renewed, and its renewal cap. Lease start is the later of the request instant and the requested start; expiry is start plus the requested duration. A reservation is live only while its state is live **and** its lease has not expired at the evaluation instant, so an expired lease immediately stops blocking new allocations. Expiry and reclamation are separate, explicit steps: `expireLeases` marks lapsed reservations `Expired` and returns their spectrum, and `reclaimExpired` moves them to `Reclaimed` with exact accounting. A lapsed lease can be released but never renewed or activated.

## Explanation

Every decision is a typed `AllocationDecision` carrying an `AllocationOutcome`, a `Status`, and a `DecisionExplanation` with the candidate count, the rejected candidates, the blocking reservation identities, and an ordered list of reasons whose first entry is decisive. Distinct refusals stay distinct: `refused-unsupported`, `refused-unknown-capability`, `refused-no-capacity`, `refused-conflict`, `refused-contiguity`, `refused-continuity`, `refused-conversion`, `refused-constraint`, `refused-exclusion`, `refused-stale-generation`, `refused-stale-incarnation`, `refused-stale-epoch`, `refused-stale-authority`, `refused-stale-capability`, `refused-duplicate`, `refused-invalid-request`, `refused-lease-expired`, `refused-limit-exceeded`, `refused-unknown-domain`, `refused-not-permitted` and `refused-channel-width` are all separately observable. A refusal is never reported as success.

## Idempotent replay

Replaying the same request identity at the same request generation with the same structural shape is an idempotent success that returns the already committed reservation: no second reservation is created, no commit statistic moves, and no duplicate commit is recorded. This is exactly the recovery path for a process that died after the commit but before the acknowledgement reached the caller. The same identity with a different shape is `refused-duplicate`, and a newer request generation supersedes the previous live reservation exactly once.

## Persistence and recovery

Durable state is a versioned binary container: an eight-byte magic, a format version, a fixed 40-byte header with its own checksum, a record stream with a per-record CRC-32 and an explicit length, and a 24-byte trailer carrying the body checksum, the declared file length and a closing magic. Every length, count and enum tag is validated against a bound before anything is read into memory. Truncation, trailing garbage, a bad magic, an unsupported version, a corrupted byte and a semantically invalid image (for example two conflicting live reservations, a dangling reference, or an impossible state) are all rejected, and a rejected image is never partially applied.

Writes are atomic: the new image is written to a sibling temporary file, flushed, and renamed over the target. A temporary file left behind by an interrupted write is ignored on the next read and removed by the next write.

Recovery advances the epoch by exactly one and mints a fresh incarnation. Activation evidence is not durable, so every reservation that was `Active` is conservatively returned to `Reserved`, flagged as needing revalidation, and given a bumped generation so that references minted by the dead incarnation are fenced. Reservations, capabilities, grids, domains, exclusion domains and audit history are restored exactly.

## Audit history

Every ownership-affecting operation appends a bounded audit record carrying its sequence number, instant, kind, controller fence, typed outcome, reservation identity and generation, domain, slot range and frequency range. The audit trail is part of durable state, so a recovered runtime can explain what the previous incarnation did.

## Reference transport

`wavelength_fabric_transport` is a reference deployment mechanism layered on the core; the core never opens a socket and never spawns a thread. It provides a framed, checksummed protocol over loopback TCP: a 24-byte header carrying a magic, the protocol version, the opcode, the request identity, a bounded payload length and a CRC-32 of the payload. A frame whose magic, version, length or checksum is wrong is rejected and the connection is closed; nothing is executed for a rejected frame. The server serves connections from a fixed worker pool behind a bounded queue, and the accept loop is polled so that shutdown is immediate and never blocks on a join that the workers need.

## Testing

The suite is deterministic and runs to natural completion. No test sets a timeout, no test sleeps to wait for a condition, and no test treats a forced termination as a pass.

- Unit tests cover grid arithmetic and validation, capability registration, request validation, candidate enumeration and eligibility, allocation and commit, conflict and exclusion behaviour, the lifecycle state machine, lease expiry and reclamation, stale authority and stale generations, idempotent replay and supersede, the binary codec, and durable state.
- Seeded property tests cross-check allocation, release, expiry and occupancy accounting against a slow independent reference model written in the test file, and print the failing seed and reproduction parameters.
- Concurrency tests run real threads against one runtime and assert the ownership invariant, exact accounting and monotone audit sequences afterwards.
- Adversarial tests exercise malformed input, extreme values, duplicate identities, reordered events, stale authority, replay, configured-limit exhaustion and unusable persistence paths.
- The multiprocess proof starts a real server process, drives it over real loopback TCP from this process and from additional independent client processes, kills the server with a real operating-system kill at distinct lifecycle boundaries, restarts it from durable state, and proves fresh-incarnation fencing, conservative demotion of active reservations, idempotent replay after a deliberately lost acknowledgement, a genuine cross-process allocation race in which exactly one of four simultaneous attempts commits, and rejection of truncated, corrupted, trailing-garbage and bad-magic state.

## Limits

- Wavelength Fabric does not control optical hardware, does not speak any vendor protocol, and does not claim conformance to any standards body's grid definition. A fixed grid here means one slot per channel and a flex grid here means a channel of one or more contiguous slots.
- Exactly one contiguous slot range is allocated per reservation. Fragmented channels are not implemented; the request that would ask for one is refused rather than silently satisfied.
- Conversion and regeneration are modelled only as an abstract exact grid-to-grid frequency mapping gated on explicit capability evidence. No physical conversion behaviour is claimed.
- The reservation table is bounded by `RuntimeConfig::maxReservations`. When a commit needs room while the table is at that bound, the oldest terminal record is pruned; its history stays in the audit trail. A table in which every record still owns spectrum refuses further commits with `refused-limit-exceeded`.
- The runtime is a single authoritative process. The reference transport proves independent-process client and server behaviour over loopback TCP on one host; it does not implement consensus, replication or any multi-host behaviour.
- The multiprocess proof was executed and verified on Windows x64 with MSVC 19.44. The POSIX code paths are implemented and built from the same sources but were not exercised in this environment.
- No GCC or Clang toolchain was available in this environment, so the `-Wall -Wextra -Werror -Wpedantic` configuration is wired into the build but was not executed here. The first-party code avoids compiler-specific constructs and is built warning-clean with `/W4 /WX`.
- AddressSanitizer could not be run in this environment: this Visual Studio configuration does not have the MSVC address-sanitizer runtime installed, and no GCC or Clang toolchain is present. The equivalent runtime checker that is available is the Debug configuration, which enables the MSVC debug STL's iterator and container bounds checking; the suite is built and run in both Release and Debug. No sanitizer result is claimed.

## Build

Requires a C++20 toolchain and CMake 3.20 or newer.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix <prefix>
```

Options: `WAVELENGTH_FABRIC_BUILD_TESTS`, `WAVELENGTH_FABRIC_BUILD_EXAMPLES`, `WAVELENGTH_FABRIC_BUILD_TOOLS`, `WAVELENGTH_FABRIC_BUILD_BENCHMARKS`, `WAVELENGTH_FABRIC_BUILD_DISTRIBUTED`, `WAVELENGTH_FABRIC_ENABLE_ASAN`, `WAVELENGTH_FABRIC_WARNINGS_AS_ERRORS`.

The install exports a CMake package. A downstream project consumes it with:

```
find_package(WavelengthFabric 1.0 REQUIRED CONFIG)
target_link_libraries(app PRIVATE WavelengthFabric::WavelengthFabric WavelengthFabric::Transport)
```

The `consumer` directory is a standalone project that builds against an installed package and independently of the repository build tree.

## Layout

`include/wavelength_fabric` public headers; `src` runtime and library; `tests` test suite and proofs; `benchmarks`; `examples`; `tools` CLI; `distributed` reference TCP server, client and multiprocess proof; `consumer` downstream `find_package` consumer; `cmake` packaging.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
