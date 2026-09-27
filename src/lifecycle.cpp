// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_registry/lifecycle.hpp"

#include <array>

namespace asset_registry {
namespace {

struct StateEntry {
    LifecycleState value;
    std::string_view token;
};

constexpr std::array<StateEntry, 7> kLifecycleStates = {{
    {LifecycleState::Unknown, "unknown"},
    {LifecycleState::Planned, "planned"},
    {LifecycleState::Provisioned, "provisioned"},
    {LifecycleState::Active, "active"},
    {LifecycleState::Maintenance, "maintenance"},
    {LifecycleState::Decommissioned, "decommissioned"},
    {LifecycleState::Disposed, "disposed"},
}};

struct InstallEntry {
    InstallationState value;
    std::string_view token;
};

constexpr std::array<InstallEntry, 6> kInstallationStates = {{
    {InstallationState::Unknown, "unknown"},
    {InstallationState::NotInstalled, "not_installed"},
    {InstallationState::Staged, "staged"},
    {InstallationState::Installed, "installed"},
    {InstallationState::Removed, "removed"},
    {InstallationState::Superseded, "superseded"},
}};

constexpr std::size_t index_of(LifecycleState value) noexcept {
    return static_cast<std::size_t>(value);
}

constexpr std::size_t index_of(InstallationState value) noexcept {
    return static_cast<std::size_t>(value);
}

// Lifecycle transition table. Rows are the current state, columns the target.
//
// Semantics enforced by the table:
//   * Unknown is permissive on exit: an unknown state is a statement about
//     missing information, not about the asset, so supplying information is
//     always legal. It is never permissive on entry from a known state, because
//     discarding known state in favour of "unknown" would destroy evidence.
//   * Decommissioned and Disposed are terminal for the live identity. Returning
//     to service requires a new physical object, which is a new identity or an
//     explicit identity reuse, not a transition.
constexpr std::array<std::array<bool, 7>, 7> kLifecycleTable = {{
    // to:           Unknown Planned Provisioned Active Maint Decom Disposed
    /* Unknown     */ {false, true, true, true, true, true, true},
    /* Planned     */ {false, false, true, false, false, true, false},
    /* Provisioned */ {false, false, false, true, false, true, false},
    /* Active      */ {false, false, false, false, true, true, false},
    /* Maintenance */ {false, false, true, true, false, true, false},
    /* Decommissioned */ {false, false, false, false, false, false, true},
    /* Disposed    */ {false, false, false, false, false, false, false},
}};

// Installation transition table.
//
//   * NotInstalled is the starting point for an asset that physically exists but
//     occupies no position.
//   * Installed requires passing through Staged at least once. There is no
//     NotInstalled -> Installed edge, because an installation that nobody staged
//     is a discrepancy worth surfacing rather than accepting.
//   * Removed may return to Staged (the same unit staged for a different
//     position) but not directly to Installed, for the same reason.
//   * Superseded is terminal for the live incarnation: the position is now
//     occupied by a different object.
constexpr std::array<std::array<bool, 6>, 6> kInstallationTable = {{
    // to:              Unknown NotInst Staged Installed Removed Superseded
    /* Unknown        */ {false, true, true, true, true, true},
    /* NotInstalled   */ {false, false, true, false, false, false},
    /* Staged         */ {false, true, false, true, true, false},
    /* Installed      */ {false, false, false, false, true, true},
    /* Removed        */ {false, false, true, false, false, true},
    /* Superseded     */ {false, false, false, false, false, false},
}};

[[nodiscard]] std::string render_lifecycle(LifecycleState value) {
    return std::string(to_string(value));
}

[[nodiscard]] std::string render_installation(InstallationState value) {
    return std::string(to_string(value));
}

}  // namespace

std::string_view to_string(LifecycleState value) noexcept {
    for (const StateEntry& entry : kLifecycleStates) {
        if (entry.value == value) {
            return entry.token;
        }
    }
    return "unknown";
}

std::string_view to_string(InstallationState value) noexcept {
    for (const InstallEntry& entry : kInstallationStates) {
        if (entry.value == value) {
            return entry.token;
        }
    }
    return "unknown";
}

std::optional<LifecycleState> lifecycle_state_from_token(std::string_view token) noexcept {
    for (const StateEntry& entry : kLifecycleStates) {
        if (entry.token == token) {
            return entry.value;
        }
    }
    return std::nullopt;
}

std::optional<InstallationState> installation_state_from_token(std::string_view token) noexcept {
    for (const InstallEntry& entry : kInstallationStates) {
        if (entry.token == token) {
            return entry.value;
        }
    }
    return std::nullopt;
}

const LifecycleState* lifecycle_states() noexcept {
    static const std::array<LifecycleState, 7> kValues = {
        LifecycleState::Unknown,       LifecycleState::Planned,       LifecycleState::Provisioned,
        LifecycleState::Active,        LifecycleState::Maintenance,   LifecycleState::Decommissioned,
        LifecycleState::Disposed,
    };
    return kValues.data();
}

std::size_t lifecycle_state_count() noexcept {
    return 7;
}

const InstallationState* installation_states() noexcept {
    static const std::array<InstallationState, 6> kValues = {
        InstallationState::Unknown, InstallationState::NotInstalled, InstallationState::Staged,
        InstallationState::Installed, InstallationState::Removed,    InstallationState::Superseded,
    };
    return kValues.data();
}

std::size_t installation_state_count() noexcept {
    return 6;
}

bool is_terminal_lifecycle(LifecycleState value) noexcept {
    return value == LifecycleState::Decommissioned || value == LifecycleState::Disposed;
}

bool permits_workload(LifecycleState value) noexcept {
    return value == LifecycleState::Active || value == LifecycleState::Maintenance;
}

bool is_legal_lifecycle_transition(LifecycleState from, LifecycleState to) noexcept {
    if (from == to) {
        return false;
    }
    const std::size_t from_index = index_of(from);
    const std::size_t to_index = index_of(to);
    if (from_index >= kLifecycleTable.size() || to_index >= kLifecycleTable[from_index].size()) {
        return false;
    }
    return kLifecycleTable[from_index][to_index];
}

bool is_legal_installation_transition(InstallationState from, InstallationState to) noexcept {
    if (from == to) {
        return false;
    }
    const std::size_t from_index = index_of(from);
    const std::size_t to_index = index_of(to);
    if (from_index >= kInstallationTable.size() || to_index >= kInstallationTable[from_index].size()) {
        return false;
    }
    return kInstallationTable[from_index][to_index];
}

std::string explain_lifecycle_rejection(LifecycleState from, LifecycleState to) {
    std::string message = "lifecycle transition ";
    message += render_lifecycle(from);
    message += " -> ";
    message += render_lifecycle(to);
    message += " is not permitted";
    if (from == to) {
        message += ": the record is already in that state";
        return message;
    }
    if (from == LifecycleState::Disposed) {
        message += ": disposed is terminal; returning an asset to service requires a new physical object";
        return message;
    }
    if (from == LifecycleState::Decommissioned && !is_terminal_lifecycle(to)) {
        message += ": decommissioned assets may only be disposed; reactivation requires a new identity generation";
        return message;
    }
    if (to == LifecycleState::Unknown) {
        message += ": a known state is never discarded in favour of unknown";
        return message;
    }
    const std::vector<LifecycleState> successors = legal_successors(from);
    message += "; permitted next states from ";
    message += render_lifecycle(from);
    message += " are:";
    if (successors.empty()) {
        message += " none (terminal state)";
        return message;
    }
    for (const LifecycleState successor : successors) {
        message += ' ';
        message += render_lifecycle(successor);
    }
    return message;
}

std::string explain_installation_rejection(InstallationState from, InstallationState to) {
    std::string message = "installation transition ";
    message += render_installation(from);
    message += " -> ";
    message += render_installation(to);
    message += " is not permitted";
    if (from == to) {
        message += ": the record is already in that state";
        return message;
    }
    if (from == InstallationState::Superseded) {
        message += ": superseded is terminal for the current identity generation";
        return message;
    }
    if (to == InstallationState::Unknown) {
        message += ": a known installation state is never discarded in favour of unknown";
        return message;
    }
    if (to == InstallationState::Installed && from != InstallationState::Staged) {
        message += ": installation must pass through staged";
        return message;
    }
    const std::vector<InstallationState> successors = legal_successors(from);
    message += "; permitted next states from ";
    message += render_installation(from);
    message += " are:";
    if (successors.empty()) {
        message += " none (terminal state)";
        return message;
    }
    for (const InstallationState successor : successors) {
        message += ' ';
        message += render_installation(successor);
    }
    return message;
}

std::vector<LifecycleState> legal_successors(LifecycleState from) {
    std::vector<LifecycleState> result;
    for (std::size_t index = 0; index < lifecycle_state_count(); ++index) {
        const LifecycleState candidate = lifecycle_states()[index];
        if (is_legal_lifecycle_transition(from, candidate)) {
            result.push_back(candidate);
        }
    }
    return result;
}

std::vector<InstallationState> legal_successors(InstallationState from) {
    std::vector<InstallationState> result;
    for (std::size_t index = 0; index < installation_state_count(); ++index) {
        const InstallationState candidate = installation_states()[index];
        if (is_legal_installation_transition(from, candidate)) {
            result.push_back(candidate);
        }
    }
    return result;
}

const StateConsistencyRules& default_consistency_rules() noexcept {
    static const StateConsistencyRules kRules{};
    return kRules;
}

std::optional<std::string> check_state_consistency(LifecycleState lifecycle, InstallationState installation,
                                                   const StateConsistencyRules& rules) {
    if (rules.require_installed_for_active && lifecycle == LifecycleState::Active &&
        installation != InstallationState::Installed) {
        std::string message = "active lifecycle requires installation state installed, observed ";
        message += render_installation(installation);
        return message;
    }
    if (rules.require_installed_for_maintenance && lifecycle == LifecycleState::Maintenance &&
        installation != InstallationState::Installed) {
        std::string message = "maintenance lifecycle requires installation state installed, observed ";
        message += render_installation(installation);
        return message;
    }
    if (rules.require_staged_for_provisioned && lifecycle == LifecycleState::Provisioned &&
        installation != InstallationState::Staged && installation != InstallationState::Installed) {
        std::string message = "provisioned lifecycle requires installation state staged or installed, observed ";
        message += render_installation(installation);
        return message;
    }
    if (rules.require_terminal_for_superseded && installation == InstallationState::Superseded &&
        !is_terminal_lifecycle(lifecycle)) {
        std::string message = "superseded installation requires a terminal lifecycle state, observed ";
        message += render_lifecycle(lifecycle);
        return message;
    }
    return std::nullopt;
}

}  // namespace asset_registry
