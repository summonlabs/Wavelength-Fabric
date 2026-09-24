# Contributing to Wavelength Fabric

We welcome contributions from individuals and organizations.

## Contribution terms

Contributions are submitted under the terms of the Apache License 2.0. No
Contributor License Agreement (CLA) is required. By submitting a contribution
you agree that it may be distributed under the Apache License 2.0.

## Scope and boundaries

Keep changes within the repository's architectural and system boundary. Do not
expand scope into adjacent systems, sibling repositories, or unrelated
concerns. Wavelength Fabric owns wavelength/channel allocation, reservation,
authority, conflict detection, and lifecycle. It does not own physical
connectivity lifecycle, transceiver identity, route computation, or measured
link quality.

Do not describe capabilities that are not implemented and tested. UNSUPPORTED
is a first-class outcome and must never be approximated.

## Quality expectations

Changes must meet the repository's normal code-quality, build, test,
documentation, and cleanup expectations:

- the first-party code must build warning-clean under strict warnings
  (`/W4 /WX` on MSVC, `-Wall -Wextra -Werror` elsewhere);
- the full test suite must pass with no timeouts and no skipped assertions;
- new behavior must come with tests, and adversarial or property tests where
  the behavior is algorithmic;
- the working tree must be clean before a pull request (no build output, logs,
  state files, or temporary validation residue).

## Attribution

Do not add AI attribution or unintended "Co-authored-by" trailers to commit
messages. The commit author is the authoritative attribution.

## Telemetry

Do not introduce telemetry transmission.
