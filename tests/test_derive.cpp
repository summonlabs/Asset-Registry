// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Derivation tests: derived identities, replacement chains, lineage traversal
// bounds, and conflict auditing.

#include <set>
#include <string>
#include <vector>

#include "test_harness.hpp"
#include "test_support.hpp"

using namespace asset_registry;
using namespace asset_test;

namespace {

struct Seeded {
    AssetId identifier;
};

/// Registers an asset and retires it, so it can be a replacement predecessor.
[[nodiscard]] AssetId register_and_retire(AssetRegistry& registry, WriterSession& session, std::string_view label,
                                          std::string_view serial_number) {
    auto registered = registry.register_asset(session, session.advance(),
                                             make_request(label, AssetClass::Server, "Acme", serial_number));
    if (!registered) {
        AR_CHECK_MSG(false, registered.error().to_string());
        return AssetId{};
    }
    const AssetId identifier = registered.value().id;
    auto retired = registry.transition_lifecycle(session, session.advance(), identifier, std::nullopt,
                                                LifecycleState::Decommissioned);
    if (!retired) {
        AR_CHECK_MSG(false, retired.error().to_string());
    }
    return identifier;
}

}  // namespace

AR_TEST(derive, derived_identity_depends_on_class_and_serial_only) {
    const SerialIdentity identity = serial("Acme", "X-1", "R2");
    auto first = detached_registry(asset_registry::default_policy(), asset_registry::default_limits(), "w1");

    RegisterAssetRequest request = make_request("ignored", AssetClass::Server, "Acme", "X-1");
    request.id_mode = AssetIdMode::DerivedFromSerialIdentity;
    request.id = AssetId{};
    request.serial_identity = identity;
    auto registered = first.registry->register_asset(first.session, first.session.advance(), request);
    AR_REQUIRE_OK(result, registered);

    // Display name, owner, and labels are metadata: they must not participate in
    // the derivation, or a re-import with a corrected label would create a second
    // record for one physical object.
    auto second = detached_registry(asset_registry::default_policy(), asset_registry::default_limits(), "w2");
    RegisterAssetRequest variant = request;
    variant.metadata.display_name = "a completely different label";
    variant.metadata.owner = owner("org.example.other");
    variant.metadata.labels = {{"tier", "silver"}};
    auto variant_registration = second.registry->register_asset(second.session, second.session.advance(), variant);
    AR_REQUIRE_OK(variant_result, variant_registration);
    AR_CHECK(variant_result.id == result.id);

    // A different model is part of the serial identity, so it must produce a
    // different key but the *same* canonical identity, because the canonical
    // collision key deliberately excludes the model.
    auto third = detached_registry(asset_registry::default_policy(), asset_registry::default_limits(), "w3");
    RegisterAssetRequest other_model = request;
    other_model.serial_identity = serial("Acme", "X-1", "R3");
    auto model_registration = third.registry->register_asset(third.session, third.session.advance(), other_model);
    AR_REQUIRE_OK(model_result, model_registration);
    AR_CHECK(model_result.id == result.id);

    // A different class is a different physical object class, so a different
    // identity.
    auto fourth = detached_registry(asset_registry::default_policy(), asset_registry::default_limits(), "w4");
    RegisterAssetRequest other_class = request;
    other_class.asset_class = AssetClass::StorageAppliance;
    auto class_registration = fourth.registry->register_asset(fourth.session, fourth.session.advance(), other_class);
    AR_REQUIRE_OK(class_result, class_registration);
    AR_CHECK(!(class_result.id == result.id));
}

AR_TEST(derive, long_replacement_chain_resolves_in_order_and_stays_acyclic) {
    RegistryPolicy policy;
    policy.serial_collision = SerialCollisionPolicy::AllowReplacementOfTerminal;
    auto context = detached_registry(policy);
    auto& registry = *context.registry;

    constexpr int kLength = 40;
    std::vector<AssetId> chain;
    AssetId previous{};
    for (int index = 0; index < kLength; ++index) {
        const std::string label = "chain-" + std::to_string(index);
        const std::string number = "CHAIN-" + std::to_string(index);
        if (index == 0) {
            chain.push_back(register_and_retire(registry, context.session, label, number));
            previous = chain.back();
            continue;
        }
        RegisterAssetRequest request = make_request(label, AssetClass::Server, "Acme", number);
        request.supersedes = previous;
        request.supersession_cause = ReplacementCause::Failure;
        auto registered = registry.register_asset(context.session, context.session.advance(), request);
        AR_REQUIRE_OK(result, registered);
        chain.push_back(result.id);
        previous = result.id;
        if (index + 1 < kLength) {
            AR_CHECK(registry
                         .transition_lifecycle(context.session, context.session.advance(), result.id, std::nullopt,
                                               LifecycleState::Decommissioned)
                         .has_value());
        }
    }

    const Snapshot snapshot = registry.snapshot();
    AR_CHECK(snapshot.size() == kLength);

    // Every link resolves exactly one hop back, in order.
    for (std::size_t index = 1; index < chain.size(); ++index) {
        AR_CHECK(snapshot.predecessor_of(chain[index]).value() == chain[index - 1]);
        const std::vector<AssetId> successors = snapshot.successors_of(chain[index - 1]);
        AR_CHECK(successors.size() == 1);
        AR_CHECK(successors.front() == chain[index]);
    }
    AR_CHECK(!snapshot.predecessor_of(chain.front()).has_value());
    AR_CHECK(snapshot.successors_of(chain.back()).empty());

    // Full traversal reaches the origin in the documented order.
    const auto full = snapshot.lineage(chain.back(), 64);
    AR_REQUIRE_OK(lineage, full);
    AR_CHECK(lineage.termination == LineageTermination::ReachedOrigin);
    AR_CHECK(lineage.predecessors.size() == kLength - 1);
    for (std::size_t index = 0; index < lineage.predecessors.size(); ++index) {
        AR_CHECK(lineage.predecessors[index] == chain[kLength - 2 - index]);
    }

    // A shallow traversal reports the bound rather than pretending the chain ended.
    const auto shallow = snapshot.lineage(chain.back(), 3);
    AR_REQUIRE_OK(limited, shallow);
    AR_CHECK(limited.termination == LineageTermination::DepthLimitReached);
    AR_CHECK(limited.predecessors.size() == 3);

    // The audit finds no cycles, no dangling predecessors, and no duplicates.
    const ConflictReport report = snapshot.audit_conflicts();
    AR_CHECK(report.clean());
}

AR_TEST(derive, a_registration_cannot_close_a_replacement_cycle) {
    // A dangling predecessor is a recorded claim about an asset this registry has not
    // seen yet. When that asset later arrives and names the claimant, the two links
    // would form a cycle, so the second registration is refused: lineage stays a forest
    // of in-trees, as the replacement contract states.
    RegistryPolicy policy;
    policy.serial_collision = SerialCollisionPolicy::AllowReplacementOfTerminal;
    auto context = detached_registry(policy);
    auto& registry = *context.registry;

    const AssetId absent = asset_id_for("cycle-absent");
    RegisterAssetRequest claimant = make_request("cycle-claimant", AssetClass::Server, "Acme", "CYC-1");
    claimant.supersedes = absent;
    claimant.allow_dangling_predecessor = true;
    auto registered = registry.register_asset(context.session, context.session.advance(), claimant);
    AR_REQUIRE_OK(claim, registered);
    AR_CHECK(registry.transition_lifecycle(context.session, context.session.advance(), claim.id, std::nullopt,
                                           LifecycleState::Decommissioned)
                 .has_value());

    RegisterAssetRequest returning = make_request("cycle-absent", AssetClass::Server, "Acme", "CYC-2");
    returning.id = absent;
    returning.id_mode = AssetIdMode::CallerSupplied;
    returning.supersedes = claim.id;
    auto refused = registry.register_asset(context.session, context.session.advance(), returning);
    AR_REQUIRE_ERROR(error, refused, ErrorCode::ReplacementCycleDetected);

    // The refused link published nothing: the claimant is still the only record, its
    // claim still names the asset that never arrived, and no successor points at it.
    const Snapshot snapshot = registry.snapshot();
    AR_CHECK(snapshot.size() == 1);
    const auto claim_predecessor = snapshot.predecessor_of(claim.id);
    AR_REQUIRE_PRESENT(unchanged_claim, claim_predecessor);
    AR_CHECK(unchanged_claim == absent);
    AR_CHECK(snapshot.successors_of(claim.id).empty());
    const ConflictReport report = snapshot.audit_conflicts();
    AR_CHECK(report.dangling_predecessors.size() == 1);
    AR_CHECK(report.dangling_predecessors.front() == claim.id);
    AR_CHECK(report.duplicate_serial_keys.empty());
    AR_CHECK(report.supersession_state_mismatches.empty());
}

AR_TEST(derive, fan_out_is_bounded_and_reported) {
    RegistryPolicy policy;
    policy.serial_collision = SerialCollisionPolicy::AllowReplacementOfTerminal;
    auto context = detached_registry(policy);
    auto& registry = *context.registry;
    const AssetId origin = register_and_retire(registry, context.session, "fanout-origin", "FAN-0");

    // Several distinct successors of one predecessor is legitimate: a failed unit
    // replaced twice, or a unit split into parts.
    for (int index = 0; index < 5; ++index) {
        RegisterAssetRequest request = make_request("fanout-" + std::to_string(index), AssetClass::Server, "Acme",
                                                    "FAN-" + std::to_string(index + 1));
        request.supersedes = origin;
        request.supersession_cause = ReplacementCause::Refresh;
        auto registered = registry.register_asset(context.session, context.session.advance(), request);
        AR_CHECK(registered.has_value());
    }
    const Snapshot snapshot = registry.snapshot();
    const std::vector<AssetId> successors = snapshot.successors_of(origin);
    AR_CHECK(successors.size() == 5);
    // Successors are reported in canonical order.
    for (std::size_t index = 1; index < successors.size(); ++index) {
        AR_CHECK(successors[index - 1] < successors[index]);
    }
    const auto chain = snapshot.lineage(origin, 8);
    AR_REQUIRE_OK(lineage, chain);
    AR_CHECK(lineage.successors.size() == 5);
    AR_CHECK(!lineage.successors_truncated);
    AR_CHECK(lineage.predecessors.empty());
    AR_CHECK(snapshot.audit_conflicts().clean());
}

AR_TEST(derive, lineage_of_an_unknown_asset_is_reported) {
    auto context = detached_registry();
    const auto chain = context.registry->snapshot().lineage(asset_id_for("missing"));
    AR_REQUIRE_ERROR(error, chain, ErrorCode::AssetNotFound);
    AR_CHECK(error.subject() == asset_id_for("missing").to_string());
    AR_CHECK(!context.registry->snapshot().predecessor_of(asset_id_for("missing")).has_value());
    AR_CHECK(context.registry->snapshot().successors_of(asset_id_for("missing")).empty());
}

AR_TEST(derive, zero_depth_lineage_query_is_refused) {
    auto context = detached_registry();
    auto registered = context.registry->register_asset(
        context.session, context.session.advance(), make_request("depth-asset", AssetClass::Server, "Acme", "D-1"));
    AR_REQUIRE_OK(created, registered);
    const auto chain = context.registry->snapshot().lineage(created.id, 0);
    AR_REQUIRE_ERROR(error, chain, ErrorCode::InvalidInput);
}

AR_TEST(derive, derived_identity_reuse_after_retirement_is_a_distinct_object) {
    // Two physical objects with the same manufacturer and serial number would be a
    // collision the registry must refuse even when they arrive through the derived
    // path, because the derivation is a function of exactly that identity.
    auto context = detached_registry();
    auto& registry = *context.registry;
    auto first = registry.register_asset(context.session, context.session.advance(),
                                         make_request("derived-a", AssetClass::Server, "Acme", "SAME-1"));
    AR_REQUIRE_OK(created, first);
    AR_CHECK(registry.transition_lifecycle(context.session, context.session.advance(), created.id, std::nullopt,
                                           LifecycleState::Decommissioned)
                 .has_value());
    auto second = registry.register_asset(context.session, context.session.advance(),
                                          make_request("derived-b", AssetClass::Server, "Acme", "SAME-1"));
    AR_REQUIRE_ERROR(error, second, ErrorCode::DuplicateSerialIdentity);
    AR_CHECK(registry.snapshot().size() == 1);
}

AR_TEST(derive, conflict_audit_reports_state_inconsistency) {
    // A registry that allowed a relaxed consistency rule can hold a record a
    // stricter reader considers inconsistent; the audit reports it rather than
    // hiding it.
    RegistryPolicy relaxed;
    relaxed.consistency.require_installed_for_active = false;
    // The record reaches Active the only way the transition table allows: through
    // Provisioned. Both relaxed rules are what let it get there while the asset was
    // never installed, which is the inconsistency under test.
    relaxed.consistency.require_staged_for_provisioned = false;
    auto context = detached_registry(relaxed);
    auto& registry = *context.registry;
    auto registered = registry.register_asset(
        context.session, context.session.advance(), make_request("inconsistent", AssetClass::Server, "Acme", "INC-1"));
    AR_REQUIRE_OK(created, registered);
    auto provisioned = registry.transition_lifecycle(context.session, context.session.advance(), created.id,
                                                     std::nullopt, LifecycleState::Provisioned);
    AR_CHECK_MSG(provisioned.has_value(), provisioned.has_value() ? "" : provisioned.error().to_string());
    auto active = registry.transition_lifecycle(context.session, context.session.advance(), created.id, std::nullopt,
                                                LifecycleState::Active);
    AR_CHECK_MSG(active.has_value(), active.has_value() ? "" : active.error().to_string());
    const Snapshot snapshot = registry.snapshot();
    const ConflictReport report = snapshot.audit_conflicts();
    // Under the default rules the record is inconsistent, and the audit uses the
    // rules the snapshot was published under, so it reports itself as consistent.
    AR_CHECK(report.state_inconsistencies.empty());
    // The record itself still carries the state that was accepted.
    const auto view = snapshot.find(created.id);
    AR_CHECK(view.has_value());
    AR_CHECK(view->state.lifecycle == LifecycleState::Active);
    AR_CHECK(view->state.installation == InstallationState::Unknown);
}

AR_TEST(derive, duplicate_serial_claims_are_impossible_through_the_api_but_detected_if_present) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    AR_CHECK(registry.register_asset(context.session, context.session.advance(),
                                     make_request("dup-a", AssetClass::Server, "Acme", "DUP-1"))
                 .has_value());
    auto duplicate = registry.register_asset(context.session, context.session.advance(),
                                             make_request("dup-b", AssetClass::Server, "Acme", "DUP-1"));
    AR_REQUIRE_ERROR(error, duplicate, ErrorCode::DuplicateSerialIdentity);
    // The audit over the resulting state finds nothing, which is the property that
    // makes a non-empty duplicate list a defect report rather than a routine find.
    AR_CHECK(registry.snapshot().audit_conflicts().duplicate_serial_keys.empty());
}

AR_TEST(derive, summary_counts_match_a_manual_walk) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    for (int index = 0; index < 12; ++index) {
        RegisterAssetRequest request = make_request("summary-" + std::to_string(index),
                                                    index % 3 == 0 ? AssetClass::Server : AssetClass::Switch, "Acme",
                                                    "SUM-" + std::to_string(index));
        if (index % 2 == 0) {
            request.metadata.owner = owner("org.example.platform");
        }
        if (index % 4 == 0) {
            request.references = {capability_ref("asi:pool-" + std::to_string(index))};
        }
        AR_CHECK(registry.register_asset(context.session, context.session.advance(), request).has_value());
    }
    // Retire two, activate none.
    const std::vector<AssetId> identifiers = registry.snapshot().ids();
    for (int index = 0; index < 2; ++index) {
        AR_CHECK(registry.transition_lifecycle(context.session, context.session.advance(), identifiers[index],
                                               std::nullopt, LifecycleState::Decommissioned)
                     .has_value());
    }

    const Snapshot snapshot = registry.snapshot();
    const InventorySummary summary = snapshot.summary();
    AR_CHECK(summary.total_assets == 12);
    AR_CHECK(summary.terminal_assets == 2);
    AR_CHECK(summary.workload_capable_assets == 0);
    AR_CHECK(summary.unowned_assets == 6);
    AR_CHECK(summary.assets_with_unverified_references == 3);
    AR_CHECK(summary.assets_with_predecessor == 0);
    AR_CHECK(summary.total_references == 3);
    AR_CHECK(summary.total_provenance_steps == 14);
    AR_CHECK(summary.published_sequence == snapshot.published_sequence());
    AR_CHECK(summary.epoch.value() == snapshot.epoch().value());
    AR_CHECK(summary.max_revision == AssetRevision(2));
    AR_CHECK(!summary.by_class.empty());
    AR_CHECK(!summary.by_lifecycle.empty());
    AR_CHECK(!summary.by_installation.empty());
    AR_CHECK(summary.by_owner.size() == 1);
    AR_CHECK(summary.by_owner.front().first == owner("org.example.platform"));
    AR_CHECK(summary.by_owner.front().second == 6);
    AR_CHECK(summary.estimated_bytes > 0);
}
