#pragma once

#include <string>
#include <string_view>

#include "wavelength_fabric/audit.hpp"
#include "wavelength_fabric/candidate.hpp"
#include "wavelength_fabric/capability.hpp"
#include "wavelength_fabric/decision.hpp"
#include "wavelength_fabric/grid.hpp"
#include "wavelength_fabric/lifecycle.hpp"
#include "wavelength_fabric/request.hpp"
#include "wavelength_fabric/resource.hpp"
#include "wavelength_fabric/runtime.hpp"

// Stable textual tokens for every enum the runtime exposes, plus human-readable
// renderings used by the audit trail, the CLI, and test diagnostics. Tokens are
// part of the observable surface of the runtime: they never change meaning
// within a major version.

namespace wavelength_fabric {

[[nodiscard]] std::string renderSlotRange(const SlotRange& range);
[[nodiscard]] std::string renderFrequencyRange(const FrequencyRange& range);
[[nodiscard]] std::string renderDuration(Duration duration);
[[nodiscard]] std::string renderInstant(Instant instant);
[[nodiscard]] std::string renderFence(const ControllerFence& fence);

}  // namespace wavelength_fabric
