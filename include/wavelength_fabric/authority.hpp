#pragma once

#include <cstdint>
#include <string_view>

#include "wavelength_fabric/error.hpp"
#include "wavelength_fabric/identity.hpp"

// Separated authority.
//
// Eligibility authority decides whether a candidate may be considered.
// Reservation authority decides whether a candidate becomes owned.
// Activation authority decides whether an owned reservation becomes active.
// Release authority decides whether owned spectrum is returned.
//
// The four are distinct types: a holder of eligibility authority cannot commit
// a reservation, and a holder of reservation authority cannot activate one.
// Every token carries the controller fence it was minted under, so a token
// issued by a dead incarnation is rejected even if its generation is current.

namespace wavelength_fabric {

// The template parameter is the authority generation type of that domain, so
// a token can only ever be checked against the matching generation counter.
template <class GenerationTag>
struct AuthorityToken {
  using generation_type = StrongValue<GenerationTag>;

  generation_type generation{};
  ControllerFence fence{};

  [[nodiscard]] bool valid() const noexcept { return generation.valid() && fence.valid(); }

  friend bool operator==(const AuthorityToken&, const AuthorityToken&) = default;
};

using EligibilityAuthority = AuthorityToken<EligibilityAuthorityGenerationTag>;
using ReservationAuthority = AuthorityToken<ReservationAuthorityGenerationTag>;
using ActivationAuthority = AuthorityToken<ActivationAuthorityGenerationTag>;
using ReleaseAuthority = AuthorityToken<ReleaseAuthorityGenerationTag>;

// The authority state the runtime currently recognises.
struct AuthorityState {
  EligibilityAuthorityGeneration eligibilityGeneration{};
  ReservationAuthorityGeneration reservationGeneration{};
  ActivationAuthorityGeneration activationGeneration{};
  ReleaseAuthorityGeneration releaseGeneration{};
  ControllerFence fence{};
};

// Checks one token against the current authority state. Returns Ok, or a
// Status whose code distinguishes stale generation, stale epoch, stale
// incarnation and invalid token.
[[nodiscard]] Status checkEligibilityAuthority(const AuthorityState& state, const EligibilityAuthority& token);
[[nodiscard]] Status checkReservationAuthority(const AuthorityState& state, const ReservationAuthority& token);
[[nodiscard]] Status checkActivationAuthority(const AuthorityState& state, const ActivationAuthority& token);
[[nodiscard]] Status checkReleaseAuthority(const AuthorityState& state, const ReleaseAuthority& token);

// Fence-only check used by paths that require a live incarnation but no
// specific authority domain.
[[nodiscard]] Status checkFence(const AuthorityState& state, const ControllerFence& fence);

}  // namespace wavelength_fabric
