// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Asset lifecycle and installation state machines.
//
// Two orthogonal dimensions describe an asset:
//
//   * InstallationState -- where the physical object is. It answers "is this
//     thing in a rack right now". It never regresses silently: removal is an
//     explicit transition with provenance.
//
//   * LifecycleState -- what the facility is permitted to do with the object.
//     It answers "may this asset carry production load". It is monotonic in the
//     sense that matters: a decommissioned asset cannot return to active within
//     the same identity generation.
//
// Both dimensions have an explicit Unknown member representing "the source did
// not supply this field". Unknown is never treated as a positive claim: it
// satisfies no invariant that requires a known state, and it can be left only
// by an explicit transition that records where the information came from.

#ifndef ASSET_REGISTRY_LIFECYCLE_HPP
#define ASSET_REGISTRY_LIFECYCLE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_registry/error.hpp"

namespace asset_registry {

enum class LifecycleState : std::uint8_t {
    Unknown = 0,
    Planned = 1,
    Provisioned = 2,
    Active = 3,
    Maintenance = 4,
    Decommissioned = 5,
    Disposed = 6,
};

enum class InstallationState : std::uint8_t {
    Unknown = 0,
    NotInstalled = 1,
    Staged = 2,
    Installed = 3,
    Removed = 4,
    Superseded = 5,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(LifecycleState value) noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(InstallationState value) noexcept;

/// Strict parse. Returns std::nullopt for unrecognised tokens.
[[nodiscard]] ASSET_REGISTRY_API std::optional<LifecycleState> lifecycle_state_from_token(
    std::string_view token) noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::optional<InstallationState> installation_state_from_token(
    std::string_view token) noexcept;

/// Every lifecycle state in ascending numeric order, including Unknown.
[[nodiscard]] ASSET_REGISTRY_API const LifecycleState* lifecycle_states() noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::size_t lifecycle_state_count() noexcept;

/// Every installation state in ascending numeric order, including Unknown.
[[nodiscard]] ASSET_REGISTRY_API const InstallationState* installation_states() noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::size_t installation_state_count() noexcept;

/// Terminal lifecycle states: no legal outbound transition except Disposed from
/// Decommissioned. A record in a terminal state cannot be reactivated.
[[nodiscard]] ASSET_REGISTRY_API bool is_terminal_lifecycle(LifecycleState value) noexcept;

/// States in which the facility may place production load on the asset.
[[nodiscard]] ASSET_REGISTRY_API bool permits_workload(LifecycleState value) noexcept;

/// Lifecycle transition legality. Self-transitions are not legal transitions;
/// a no-op request is rejected as IllegalLifecycleTransition so that callers
/// cannot mistake a no-op for an applied state change.
[[nodiscard]] ASSET_REGISTRY_API bool is_legal_lifecycle_transition(LifecycleState from, LifecycleState to) noexcept;

/// Installation transition legality. Self-transitions are rejected for the same
/// reason as above.
[[nodiscard]] ASSET_REGISTRY_API bool is_legal_installation_transition(InstallationState from,
                                                                       InstallationState to) noexcept;

/// Human explanation of why a transition is illegal, naming the missing
/// intermediate states.
[[nodiscard]] ASSET_REGISTRY_API std::string explain_lifecycle_rejection(LifecycleState from, LifecycleState to);

[[nodiscard]] ASSET_REGISTRY_API std::string explain_installation_rejection(InstallationState from,
                                                                            InstallationState to);

/// The set of states reachable in one legal step from `from`, in ascending
/// numeric order. Used by tests and by the CLI's `states` command.
[[nodiscard]] ASSET_REGISTRY_API std::vector<LifecycleState> legal_successors(LifecycleState from);

[[nodiscard]] ASSET_REGISTRY_API std::vector<InstallationState> legal_successors(InstallationState from);

/// Cross-dimension consistency rules.
struct StateConsistencyRules {
    /// When true, LifecycleState::Active requires InstallationState::Installed.
    /// DCCP Tranche 1 treats "active but not physically installed" as an
    /// inconsistency: it means the facility believes an asset is carrying load
    /// while its installation evidence says otherwise.
    bool require_installed_for_active = true;

    /// When true, LifecycleState::Maintenance requires
    /// InstallationState::Installed. Maintenance of an asset that is not in place
    /// is a contradiction: either the asset is installed and being serviced, or
    /// it has been removed, in which case the lifecycle state is Planned or
    /// Provisioned rather than Maintenance.
    bool require_installed_for_maintenance = true;

    /// When true, LifecycleState::Provisioned requires the asset to be at least
    /// Staged. Provisioning an asset nobody has physically received is modelled
    /// as Planned until it is staged.
    bool require_staged_for_provisioned = true;

    /// When true, InstallationState::Superseded requires a terminal lifecycle
    /// state (Decommissioned or Disposed). Physical replacement is the end of an
    /// asset's service life, not a temporary relocation.
    bool require_terminal_for_superseded = true;

    friend bool operator==(const StateConsistencyRules& lhs, const StateConsistencyRules& rhs) noexcept {
        return lhs.require_installed_for_active == rhs.require_installed_for_active &&
               lhs.require_installed_for_maintenance == rhs.require_installed_for_maintenance &&
               lhs.require_staged_for_provisioned == rhs.require_staged_for_provisioned &&
               lhs.require_terminal_for_superseded == rhs.require_terminal_for_superseded;
    }
};

/// The default rule set. Documented and tested; changing it requires a store
/// format version bump because consistency verdicts are persisted.
[[nodiscard]] ASSET_REGISTRY_API const StateConsistencyRules& default_consistency_rules() noexcept;

/// Evaluates the cross-dimension rules. Returns an empty optional when the pair
/// is consistent, otherwise a machine-readable explanation.
///
/// The evaluation treats Unknown defensively: an Unknown state never satisfies a
/// requirement, so a pair containing Unknown is reported as inconsistent when
/// the other member is a known positive claim.
[[nodiscard]] ASSET_REGISTRY_API std::optional<std::string> check_state_consistency(LifecycleState lifecycle,
                                                                                    InstallationState installation,
                                                                                    const StateConsistencyRules& rules);

/// Pair of the two state dimensions, ordered deterministically.
struct AssetState {
    LifecycleState lifecycle = LifecycleState::Unknown;
    InstallationState installation = InstallationState::Unknown;

    friend bool operator==(const AssetState& lhs, const AssetState& rhs) noexcept {
        return lhs.lifecycle == rhs.lifecycle && lhs.installation == rhs.installation;
    }
    friend bool operator!=(const AssetState& lhs, const AssetState& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const AssetState& lhs, const AssetState& rhs) noexcept {
        return lhs.lifecycle != rhs.lifecycle ? lhs.lifecycle < rhs.lifecycle : lhs.installation < rhs.installation;
    }
};

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_LIFECYCLE_HPP
