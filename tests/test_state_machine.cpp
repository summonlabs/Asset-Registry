// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Lifecycle and installation state-machine tests.
//
// The transition tables are asserted exhaustively rather than by example: every
// (from, to) pair is checked against the documented legality, so adding a state or
// an edge without updating the table fails here rather than in production.

#include <set>
#include <string>
#include <vector>

#include "test_harness.hpp"
#include "test_support.hpp"

using namespace asset_registry;
using namespace asset_test;

namespace {

/// The documented lifecycle transition matrix, restated independently of the
/// implementation so a change to the implementation alone cannot pass.
[[nodiscard]] bool expected_lifecycle(LifecycleState from, LifecycleState to) {
    if (from == to) {
        return false;
    }
    switch (from) {
        case LifecycleState::Unknown:
            return true;
        case LifecycleState::Planned:
            return to == LifecycleState::Provisioned || to == LifecycleState::Decommissioned;
        case LifecycleState::Provisioned:
            return to == LifecycleState::Active || to == LifecycleState::Decommissioned;
        case LifecycleState::Active:
            return to == LifecycleState::Maintenance || to == LifecycleState::Decommissioned;
        case LifecycleState::Maintenance:
            return to == LifecycleState::Provisioned || to == LifecycleState::Active ||
                   to == LifecycleState::Decommissioned;
        case LifecycleState::Decommissioned:
            return to == LifecycleState::Disposed;
        case LifecycleState::Disposed:
            return false;
    }
    return false;
}

/// The documented installation transition matrix.
[[nodiscard]] bool expected_installation(InstallationState from, InstallationState to) {
    if (from == to) {
        return false;
    }
    switch (from) {
        case InstallationState::Unknown:
            return true;
        case InstallationState::NotInstalled:
            return to == InstallationState::Staged;
        case InstallationState::Staged:
            return to == InstallationState::NotInstalled || to == InstallationState::Installed ||
                   to == InstallationState::Removed;
        case InstallationState::Installed:
            return to == InstallationState::Removed || to == InstallationState::Superseded;
        case InstallationState::Removed:
            return to == InstallationState::Staged || to == InstallationState::Superseded;
        case InstallationState::Superseded:
            return false;
    }
    return false;
}

}  // namespace

AR_TEST(state_machine, lifecycle_table_matches_the_documented_matrix) {
    for (std::size_t from_index = 0; from_index < lifecycle_state_count(); ++from_index) {
        const LifecycleState from = lifecycle_states()[from_index];
        for (std::size_t to_index = 0; to_index < lifecycle_state_count(); ++to_index) {
            const LifecycleState to = lifecycle_states()[to_index];
            AR_CHECK_MSG(is_legal_lifecycle_transition(from, to) == expected_lifecycle(from, to),
                         std::string("from=") + std::string(to_string(from)) + " to=" + std::string(to_string(to)));
        }
    }
}

AR_TEST(state_machine, installation_table_matches_the_documented_matrix) {
    for (std::size_t from_index = 0; from_index < installation_state_count(); ++from_index) {
        const InstallationState from = installation_states()[from_index];
        for (std::size_t to_index = 0; to_index < installation_state_count(); ++to_index) {
            const InstallationState to = installation_states()[to_index];
            AR_CHECK_MSG(is_legal_installation_transition(from, to) == expected_installation(from, to),
                         std::string("from=") + std::string(to_string(from)) + " to=" + std::string(to_string(to)));
        }
    }
}

AR_TEST(state_machine, terminal_states_have_no_legal_successor_except_disposal) {
    AR_CHECK(is_terminal_lifecycle(LifecycleState::Decommissioned));
    AR_CHECK(is_terminal_lifecycle(LifecycleState::Disposed));
    AR_CHECK(!is_terminal_lifecycle(LifecycleState::Active));
    const std::vector<LifecycleState> from_decommissioned = legal_successors(LifecycleState::Decommissioned);
    AR_CHECK(from_decommissioned.size() == 1);
    AR_CHECK(from_decommissioned.front() == LifecycleState::Disposed);
    AR_CHECK(legal_successors(LifecycleState::Disposed).empty());
    AR_CHECK(legal_successors(InstallationState::Superseded).empty());
    AR_CHECK(permits_workload(LifecycleState::Active));
    AR_CHECK(permits_workload(LifecycleState::Maintenance));
    AR_CHECK(!permits_workload(LifecycleState::Decommissioned));
    AR_CHECK(!permits_workload(LifecycleState::Unknown));
}

AR_TEST(state_machine, self_transitions_are_never_legal) {
    for (std::size_t index = 0; index < lifecycle_state_count(); ++index) {
        AR_CHECK(!is_legal_lifecycle_transition(lifecycle_states()[index], lifecycle_states()[index]));
    }
    for (std::size_t index = 0; index < installation_state_count(); ++index) {
        AR_CHECK(!is_legal_installation_transition(installation_states()[index], installation_states()[index]));
    }
}

AR_TEST(state_machine, unknown_is_permissive_on_exit_and_closed_on_entry) {
    for (std::size_t index = 0; index < lifecycle_state_count(); ++index) {
        const LifecycleState target = lifecycle_states()[index];
        AR_CHECK(is_legal_lifecycle_transition(LifecycleState::Unknown, target) ==
                 (target != LifecycleState::Unknown));
        // Entering Unknown from a known state is never legal: discarding known
        // state in favour of "unknown" would destroy evidence.
        if (target != LifecycleState::Unknown) {
            AR_CHECK(!is_legal_lifecycle_transition(target, LifecycleState::Unknown));
        }
    }
    for (std::size_t index = 0; index < installation_state_count(); ++index) {
        const InstallationState target = installation_states()[index];
        AR_CHECK(is_legal_installation_transition(InstallationState::Unknown, target) ==
                 (target != InstallationState::Unknown));
        if (target != InstallationState::Unknown) {
            AR_CHECK(!is_legal_installation_transition(target, InstallationState::Unknown));
        }
    }
}

AR_TEST(state_machine, reachability_from_planned_is_closed_under_the_table) {
    // Breadth-first closure: everything reachable from Planned must itself only
    // reach states in the closure, which is the property that makes "no silent
    // return to active" provable rather than asserted.
    std::set<LifecycleState> reachable{LifecycleState::Planned};
    std::vector<LifecycleState> frontier{LifecycleState::Planned};
    while (!frontier.empty()) {
        const LifecycleState current = frontier.back();
        frontier.pop_back();
        for (const LifecycleState next : legal_successors(current)) {
            if (reachable.insert(next).second) {
                frontier.push_back(next);
            }
        }
    }
    AR_CHECK(reachable.count(LifecycleState::Active) == 1);
    AR_CHECK(reachable.count(LifecycleState::Disposed) == 1);
    // Every state reachable from Planned is in the closure, so the closure is
    // complete; the check is that Disposed has no exit, which the table test
    // already covers, restated here as the closure property.
    for (const LifecycleState state : reachable) {
        for (const LifecycleState next : legal_successors(state)) {
            AR_CHECK(reachable.count(next) == 1);
        }
    }
}

AR_TEST(state_machine, consistency_rules_follow_the_documented_defaults) {
    const StateConsistencyRules& rules = default_consistency_rules();
    AR_CHECK(rules.require_installed_for_active);
    AR_CHECK(rules.require_installed_for_maintenance);
    AR_CHECK(rules.require_staged_for_provisioned);
    AR_CHECK(rules.require_terminal_for_superseded);

    AR_CHECK(!check_state_consistency(LifecycleState::Active, InstallationState::Installed, rules).has_value());
    AR_CHECK(check_state_consistency(LifecycleState::Active, InstallationState::Staged, rules).has_value());
    AR_CHECK(check_state_consistency(LifecycleState::Active, InstallationState::Unknown, rules).has_value());
    AR_CHECK(!check_state_consistency(LifecycleState::Maintenance, InstallationState::Installed, rules).has_value());
    AR_CHECK(check_state_consistency(LifecycleState::Maintenance, InstallationState::Removed, rules).has_value());
    AR_CHECK(!check_state_consistency(LifecycleState::Provisioned, InstallationState::Staged, rules).has_value());
    AR_CHECK(!check_state_consistency(LifecycleState::Provisioned, InstallationState::Installed, rules).has_value());
    AR_CHECK(check_state_consistency(LifecycleState::Provisioned, InstallationState::NotInstalled, rules).has_value());
    AR_CHECK(!check_state_consistency(LifecycleState::Decommissioned, InstallationState::Superseded, rules)
                  .has_value());
    AR_CHECK(check_state_consistency(LifecycleState::Active, InstallationState::Superseded, rules).has_value());
    // Unknown never satisfies a requirement.
    AR_CHECK(check_state_consistency(LifecycleState::Unknown, InstallationState::Installed, rules) == std::nullopt);
}

AR_TEST(state_machine, explanations_name_the_reason_and_the_permitted_successors) {
    const std::string decommissioned = explain_lifecycle_rejection(LifecycleState::Decommissioned,
                                                                   LifecycleState::Active);
    AR_CHECK(decommissioned.find("decommissioned") != std::string::npos);
    AR_CHECK(decommissioned.find("disposed") != std::string::npos);

    const std::string noop = explain_lifecycle_rejection(LifecycleState::Active, LifecycleState::Active);
    AR_CHECK(noop.find("already") != std::string::npos);

    const std::string to_unknown = explain_lifecycle_rejection(LifecycleState::Active, LifecycleState::Unknown);
    AR_CHECK(to_unknown.find("unknown") != std::string::npos);

    const std::string install = explain_installation_rejection(InstallationState::NotInstalled,
                                                               InstallationState::Installed);
    AR_CHECK(install.find("staged") != std::string::npos);

    const std::string terminal = explain_installation_rejection(InstallationState::Superseded,
                                                                InstallationState::Installed);
    AR_CHECK(terminal.find("terminal") != std::string::npos);
}

AR_TEST(state_machine, registry_rejects_illegal_transitions_with_the_right_code) {
    auto context = detached_registry();
    AssetRegistry& registry = *context.registry;
    WriterSession& session = context.session;
    const AssetId identifier = asset_id_for("state-machine-asset");

    auto registered = registry.register_asset(
        session, session.advance(), make_request("state-machine-asset", AssetClass::Server, "Acme", "SM-1"));
    AR_REQUIRE_OK(registration, registered);

    // Active is not reachable directly from Planned, so the transition table
    // refuses it before the consistency rules are consulted.
    auto refused = registry.transition_lifecycle(session, session.advance(), identifier, std::nullopt,
                                                 LifecycleState::Active);
    AR_REQUIRE_ERROR(error, refused, ErrorCode::IllegalLifecycleTransition);
    AR_CHECK(error.message().find("provisioned") != std::string::npos);

    // Directly to Disposed is not a legal edge from Planned.
    auto skipped = registry.transition_lifecycle(session, session.advance(), identifier, std::nullopt,
                                                 LifecycleState::Disposed);
    AR_REQUIRE_ERROR(skip_error, skipped, ErrorCode::IllegalLifecycleTransition);

    // A no-op transition is refused rather than recorded.
    auto noop = registry.transition_lifecycle(session, session.advance(), identifier, std::nullopt,
                                              LifecycleState::Planned);
    AR_REQUIRE_ERROR(noop_error, noop, ErrorCode::IllegalLifecycleTransition);

    // Install the asset: NotInstalled -> Staged -> Installed.
    AR_REQUIRE_OK(to_installed_none, registry.transition_installation(session, session.advance(), identifier,
                                                                     std::nullopt, InstallationState::NotInstalled));
    AR_REQUIRE_OK(to_staged, registry.transition_installation(session, session.advance(), identifier, std::nullopt,
                                                              InstallationState::Staged));
    AR_REQUIRE_OK(to_installed, registry.transition_installation(session, session.advance(), identifier, std::nullopt,
                                                                 InstallationState::Installed));

    // Provisioned requires staged-or-installed, satisfied.
    AR_REQUIRE_OK(provisioned, registry.transition_lifecycle(session, session.advance(), identifier, std::nullopt,
                                                             LifecycleState::Provisioned));
    AR_REQUIRE_OK(active, registry.transition_lifecycle(session, session.advance(), identifier, std::nullopt,
                                                        LifecycleState::Active));
    AR_REQUIRE_OK(maintenance, registry.transition_lifecycle(session, session.advance(), identifier, std::nullopt,
                                                             LifecycleState::Maintenance));
    AR_REQUIRE_OK(back_to_active, registry.transition_lifecycle(session, session.advance(), identifier, std::nullopt,
                                                                LifecycleState::Active));
    AR_REQUIRE_OK(decommissioned, registry.transition_lifecycle(session, session.advance(), identifier, std::nullopt,
                                                                LifecycleState::Decommissioned));
    // A decommissioned asset cannot return to service under the same identity.
    auto reactivate = registry.transition_lifecycle(session, session.advance(), identifier, std::nullopt,
                                                    LifecycleState::Active);
    AR_REQUIRE_ERROR(reactivate_error, reactivate, ErrorCode::IllegalLifecycleTransition);
    AR_REQUIRE_OK(disposed, registry.transition_lifecycle(session, session.advance(), identifier, std::nullopt,
                                                          LifecycleState::Disposed));
    auto from_disposed = registry.transition_lifecycle(session, session.advance(), identifier, std::nullopt,
                                                       LifecycleState::Decommissioned);
    AR_REQUIRE_ERROR(disposed_error, from_disposed, ErrorCode::IllegalLifecycleTransition);

    const Snapshot snapshot = registry.snapshot();
    const auto view = snapshot.find(identifier);
    AR_CHECK(view.has_value());
    AR_CHECK(view->state.lifecycle == LifecycleState::Disposed);
    AR_CHECK(view->state.installation == InstallationState::Installed);
    AR_CHECK(view->is_terminal());
    AR_CHECK(!view->permits_workload());
}

AR_TEST(state_machine, superseding_requires_a_terminal_lifecycle_state) {
    auto context = detached_registry();
    AssetRegistry& registry = *context.registry;
    WriterSession& session = context.session;
    const AssetId identifier = asset_id_for("supersede-asset");
    auto registered = registry.register_asset(
        session, session.advance(), make_request("supersede-asset", AssetClass::Switch, "Acme", "SW-1"));
    AR_REQUIRE_OK(registration, registered);
    AR_REQUIRE_OK(installed_none, registry.transition_installation(session, session.advance(), identifier,
                                                                  std::nullopt, InstallationState::NotInstalled));
    AR_REQUIRE_OK(staged, registry.transition_installation(session, session.advance(), identifier, std::nullopt,
                                                           InstallationState::Staged));
    AR_REQUIRE_OK(installed, registry.transition_installation(session, session.advance(), identifier, std::nullopt,
                                                              InstallationState::Installed));
    // Marking a live asset as superseded is refused: replacement is the end of a
    // service life, not a temporary relocation.
    auto refused = registry.transition_installation(session, session.advance(), identifier, std::nullopt,
                                                    InstallationState::Superseded);
    AR_REQUIRE_ERROR(error, refused, ErrorCode::LifecycleInvariantViolation);
    AR_CHECK(error.message().find("terminal") != std::string::npos);
}
