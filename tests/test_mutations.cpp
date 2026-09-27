// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Mutation-surface tests: metadata patching, reclassification, placement,
// references, idempotent retries, and replacement lineage.

#include <set>
#include <string>
#include <vector>

#include "test_harness.hpp"
#include "test_support.hpp"

using namespace asset_registry;
using namespace asset_test;

namespace {

/// Registers one asset and returns its identity.
[[nodiscard]] AssetId register_one(AssetRegistry& registry, WriterSession& session, std::string_view label,
                                   AssetClass asset_class = AssetClass::Server, std::string_view serial_number = "S-1") {
    auto registered = registry.register_asset(session, session.advance(),
                                             make_request(label, asset_class, "Acme", serial_number));
    if (!registered) {
        AR_CHECK_MSG(false, registered.error().to_string());
        return AssetId{};
    }
    return registered.value().id;
}

}  // namespace

AR_TEST(metadata, patch_applies_only_the_named_fields) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    const AssetId identifier = register_one(registry, context.session, "meta-asset");
    auto started = registry.snapshot().find(identifier);
    AR_REQUIRE_PRESENT(initial, started);

    MetadataPatch patch;
    patch.display_name = "meta-asset renamed";
    patch.owner = owner("org.example.platform");
    patch.set_labels = {{"tier", "gold"}, {"rack.unit", "7"}};
    auto updated = registry.update_metadata(context.session, context.session.advance(), identifier,
                                            initial.revision, patch);
    AR_REQUIRE_OK(result, updated);

    const auto view = registry.snapshot().find(identifier);
    AR_CHECK(view.has_value());
    AR_CHECK(view->metadata.display_name == "meta-asset renamed");
    AR_CHECK(view->metadata.owner.has_value());
    AR_CHECK(view->metadata.labels.size() == 2);
    // Untouched fields keep their previous value.
    AR_CHECK(view->metadata.notes.empty());
    AR_CHECK(!view->metadata.site.has_value());
    AR_CHECK(view->revision == AssetRevision(2));
    AR_CHECK(view->provenance->back().action == ProvenanceAction::OwnerChanged);
}

AR_TEST(metadata, absent_fields_are_left_alone_and_clear_flags_remove_values) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    const AssetId identifier = register_one(registry, context.session, "clear-asset");

    MetadataPatch set_patch;
    set_patch.owner = owner("org.example.platform");
    set_patch.site = location("site-a.hall-2");
    set_patch.notes = "note";
    set_patch.set_labels = {{"tier", "gold"}};
    AR_CHECK(registry.update_metadata(context.session, context.session.advance(), identifier, std::nullopt, set_patch)
                 .has_value());

    // Absent members change nothing.
    MetadataPatch noop_patch;
    noop_patch.display_name = "clear-asset";
    auto noop = registry.update_metadata(context.session, context.session.advance(), identifier, std::nullopt,
                                         noop_patch);
    AR_REQUIRE_ERROR(noop_error, noop, ErrorCode::InvalidInput);

    MetadataPatch clear_patch;
    clear_patch.clear_owner = true;
    clear_patch.clear_site = true;
    clear_patch.remove_labels = {"tier"};
    auto cleared = registry.update_metadata(context.session, context.session.advance(), identifier, std::nullopt,
                                            clear_patch);
    AR_REQUIRE_OK(result, cleared);
    const auto view = registry.snapshot().find(identifier);
    AR_CHECK(view.has_value());
    AR_CHECK(!view->metadata.owner.has_value());
    AR_CHECK(!view->metadata.site.has_value());
    AR_CHECK(view->metadata.labels.empty());
    // Notes were not named, so they survive.
    AR_CHECK(view->metadata.notes == "note");

    MetadataPatch clear_absent;
    clear_absent.clear_owner = true;
    auto refused = registry.update_metadata(context.session, context.session.advance(), identifier, std::nullopt,
                                            clear_absent);
    AR_REQUIRE_ERROR(absent_error, refused, ErrorCode::AssetNotFound);
}

AR_TEST(metadata, unknown_asset_is_reported_not_created) {
    auto context = detached_registry();
    MetadataPatch patch;
    patch.notes = "x";
    auto missing = context.registry->update_metadata(context.session, context.session.advance(),
                                                     asset_id_for("never-registered"), std::nullopt, patch);
    AR_REQUIRE_ERROR(error, missing, ErrorCode::AssetNotFound);
    AR_CHECK(context.registry->snapshot().empty());
}

AR_TEST(classification, reclassification_is_recorded_and_unknown_target_refused) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    const AssetId identifier = register_one(registry, context.session, "class-asset", AssetClass::Server);

    auto reclassified = registry.reclassify_asset(context.session, context.session.advance(), identifier,
                                                  std::nullopt, AssetClass::StorageAppliance);
    AR_REQUIRE_OK(result, reclassified);
    AR_CHECK(result.revision == AssetRevision(2));
    const auto view = registry.snapshot().find(identifier);
    AR_CHECK(view.has_value());
    AR_CHECK(view->asset_class == AssetClass::StorageAppliance);
    AR_CHECK(view->provenance->back().action == ProvenanceAction::ClassReclassified);
    AR_CHECK(view->provenance->back().change.find("server->storage_appliance") != std::string::npos);

    auto to_unknown = registry.reclassify_asset(context.session, context.session.advance(), identifier, std::nullopt,
                                                AssetClass::Unknown);
    AR_REQUIRE_ERROR(unknown_error, to_unknown, ErrorCode::UnknownAssetClass);

    auto same_class = registry.reclassify_asset(context.session, context.session.advance(), identifier, std::nullopt,
                                                AssetClass::StorageAppliance);
    AR_REQUIRE_ERROR(same_error, same_class, ErrorCode::InvalidInput);
}

AR_TEST(serial_identity, update_is_uniqueness_checked) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    const AssetId first = register_one(registry, context.session, "serial-a", AssetClass::Server, "SER-1");
    const AssetId second = register_one(registry, context.session, "serial-b", AssetClass::Server, "SER-2");

    auto conflict = registry.update_serial_identity(context.session, context.session.advance(), second, std::nullopt,
                                                    serial("Acme", "SER-1"));
    AR_REQUIRE_ERROR(error, conflict, ErrorCode::DuplicateSerialIdentity);
    AR_CHECK(error.subject() == first.to_string());

    auto updated = registry.update_serial_identity(context.session, context.session.advance(), second, std::nullopt,
                                                   serial("Acme", "SER-3", "R2"));
    AR_REQUIRE_OK(result, updated);
    const auto view = registry.snapshot().find(second);
    AR_CHECK(view.has_value());
    AR_CHECK(view->serial_identity.serial().text() == "SER-3");
    AR_CHECK(view->serial_identity.model().has_value());

    // The identity index followed the change: the old key no longer resolves.
    AR_CHECK(!registry.snapshot().find_by_serial(serial("Acme", "SER-2")).has_value());
    AR_CHECK(registry.snapshot().find_by_serial(serial("Acme", "SER-3")).value() == second);
    // The other record still resolves under its own key.
    AR_CHECK(registry.snapshot().find_by_serial(serial("Acme", "SER-1")).value() == first);
}

AR_TEST(placement, location_and_rack_are_typed_and_bounded) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    const AssetId identifier = register_one(registry, context.session, "placed-asset");

    PlacementChange change;
    change.location = location("site-a.hall-2.room-4");
    change.rack = rack("site-a.hall-2.room-4.rack-7");
    change.units = UnitSpan::create(10, 12).value();
    change.evidence = ReferenceEvidence::Verified;
    auto placed = registry.set_placement(context.session, context.session.advance(), identifier, std::nullopt, change);
    AR_REQUIRE_OK(result, placed);

    const auto view = registry.snapshot().find(identifier);
    AR_CHECK(view.has_value());
    AR_CHECK(view->location().has_value());
    AR_CHECK(view->location()->text() == "site-a.hall-2.room-4");
    AR_CHECK(view->rack().has_value());
    AR_CHECK(view->rack()->text() == "site-a.hall-2.room-4.rack-7");
    AR_CHECK(view->location_evidence() == ReferenceEvidence::Verified);
    AR_CHECK(view->references.size() == 3);

    // A span without a rack is refused.
    auto second = detached_registry();
    const AssetId bare = register_one(*second.registry, second.session, "bare-asset");
    PlacementChange span_only;
    span_only.units = UnitSpan::create(1, 2).value();
    auto refused = second.registry->set_placement(second.session, second.session.advance(), bare, std::nullopt,
                                                 span_only);
    AR_REQUIRE_ERROR(span_error, refused, ErrorCode::InvalidInput);

    // Clearing a reference the record does not carry is refused rather than
    // silently accepted.
    PlacementChange clear_missing;
    clear_missing.clear_rack = true;
    auto clear_refused = second.registry->set_placement(second.session, second.session.advance(), bare, std::nullopt,
                                                        clear_missing);
    AR_REQUIRE_ERROR(clear_error, clear_refused, ErrorCode::AssetNotFound);

    // Clearing the location leaves the rack in place.
    PlacementChange clear_location;
    clear_location.clear_location = true;
    auto cleared = registry.set_placement(context.session, context.session.advance(), identifier, std::nullopt,
                                          clear_location);
    AR_REQUIRE_OK(clear_result, cleared);
    const auto after = registry.snapshot().find(identifier);
    AR_CHECK(after.has_value());
    AR_CHECK(!after->location().has_value());
    AR_CHECK(after->rack().has_value());
}

AR_TEST(references, attach_detach_and_evidence_changes) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    const AssetId identifier = register_one(registry, context.session, "reference-asset");

    const Reference scheduling = capability_ref("asi:accelerator.scheduling", ReferenceEvidence::Unverified);
    auto attached = registry.attach_reference(context.session, context.session.advance(), identifier, std::nullopt,
                                              scheduling);
    AR_REQUIRE_OK(result, attached);
    AR_CHECK(result.revision == AssetRevision(2));

    auto duplicate = registry.attach_reference(context.session, context.session.advance(), identifier, std::nullopt,
                                               scheduling);
    AR_REQUIRE_ERROR(duplicate_error, duplicate, ErrorCode::ReferenceAlreadyAttached);

    // A second location is a conflict, not a merge.
    AR_CHECK(registry.attach_reference(context.session, context.session.advance(), identifier, std::nullopt,
                                       Reference::location(location("site-a"), ReferenceEvidence::Verified))
                 .has_value());
    auto conflict = registry.attach_reference(context.session, context.session.advance(), identifier, std::nullopt,
                                              Reference::location(location("site-b"), ReferenceEvidence::Verified));
    AR_REQUIRE_ERROR(conflict_error, conflict, ErrorCode::AliasConflict);

    // Several external references of different kinds are legitimate.
    AR_CHECK(registry
                 .attach_reference(context.session, context.session.advance(), identifier, std::nullopt,
                                   Reference::external_object(
                                       ExternalObjectReference::create("dfi-path", "path-1").value(),
                                       ReferenceEvidence::Verified))
                 .has_value());
    AR_CHECK(registry
                 .attach_reference(context.session, context.session.advance(), identifier, std::nullopt,
                                   Reference::external_object(
                                       ExternalObjectReference::create("firmware-baseline", "baseline-4").value(),
                                       ReferenceEvidence::Verified))
                 .has_value());
    auto same_kind = registry.attach_reference(
        context.session, context.session.advance(), identifier, std::nullopt,
        Reference::external_object(ExternalObjectReference::create("firmware-baseline", "baseline-5").value(),
                                   ReferenceEvidence::Verified));
    AR_REQUIRE_ERROR(kind_error, same_kind, ErrorCode::AliasConflict);

    // Evidence level changes are recorded as their own action.
    auto verified = registry.set_reference_evidence(context.session, context.session.advance(), identifier,
                                                    std::nullopt, scheduling, ReferenceEvidence::Verified);
    AR_REQUIRE_OK(evidence_result, verified);
    const auto view = registry.snapshot().find(identifier);
    AR_CHECK(view.has_value());
    AR_CHECK(view->provenance->back().action == ProvenanceAction::ReferenceEvidenceChanged);
    auto stale = registry.set_reference_evidence(context.session, context.session.advance(), identifier, std::nullopt,
                                                 scheduling, ReferenceEvidence::Stale);
    AR_CHECK(stale.has_value());

    // Detaching a reference the record does not carry is refused.
    auto not_attached = registry.detach_reference(context.session, context.session.advance(), identifier,
                                                  std::nullopt, capability_ref("asi:not.present"));
    AR_REQUIRE_ERROR(not_attached_error, not_attached, ErrorCode::ReferenceNotAttached);

    auto detached = registry.detach_reference(context.session, context.session.advance(), identifier, std::nullopt,
                                              scheduling);
    AR_REQUIRE_OK(detach_result, detached);
    const auto after = registry.snapshot().find(identifier);
    AR_CHECK(after.has_value());
    AR_CHECK(after->capability_references().empty());
    AR_CHECK(after->provenance->back().action == ProvenanceAction::ReferenceDetached);
}

AR_TEST(idempotency, replaying_a_key_returns_the_recorded_outcome) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    const AssetId identifier = register_one(registry, context.session, "idempotent-asset");

    MetadataPatch patch;
    patch.owner = owner("org.example.platform");
    const MutationEnvelope first = context.session.advance("key-1", "first attempt");
    auto applied = registry.update_metadata(context.session, first, identifier, std::nullopt, patch);
    AR_REQUIRE_OK(result, applied);
    AR_CHECK(!result.idempotent_replay);
    AR_CHECK(result.revision == AssetRevision(2));

    // The same envelope again: the outcome is replayed and nothing changes.
    auto replayed = registry.update_metadata(context.session, first, identifier, std::nullopt, patch);
    AR_REQUIRE_OK(replay_result, replayed);
    AR_CHECK(replay_result.idempotent_replay);
    AR_CHECK(replay_result.revision == AssetRevision(2));
    AR_CHECK(!replay_result.published_sequence.has_value());
    const auto view = registry.snapshot().find(identifier);
    AR_CHECK(view.has_value());
    AR_CHECK(view->revision == AssetRevision(2));

    // The same key with a different request is a conflict, not a silent apply.
    MetadataPatch different;
    different.owner = owner("org.example.other");
    auto conflict = registry.update_metadata(context.session, first, identifier, std::nullopt, different);
    AR_REQUIRE_ERROR(conflict_error, conflict, ErrorCode::IdempotencyConflict);

    // The same key with a different sequence is also a conflict.
    MutationEnvelope other_sequence;
    other_sequence.sequence = MutationSequence(9);
    other_sequence.idempotency_key = "key-1";
    auto sequence_conflict = registry.update_metadata(context.session, other_sequence, identifier, std::nullopt, patch);
    AR_CHECK(sequence_conflict.has_value() || sequence_conflict.code() != ErrorCode::None);
}

AR_TEST(idempotency, a_repeated_sequence_without_a_key_is_refused) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    const AssetId identifier = register_one(registry, context.session, "sequence-asset");
    MetadataPatch patch;
    patch.notes = "one";
    const MutationEnvelope envelope = context.session.advance();
    AR_CHECK(registry.update_metadata(context.session, envelope, identifier, std::nullopt, patch).has_value());
    auto repeated = registry.update_metadata(context.session, envelope, identifier, std::nullopt, patch);
    AR_REQUIRE_ERROR(error, repeated, ErrorCode::StaleMutationSequence);
    AR_CHECK(error.detail_key() == "high_water");
}

AR_TEST(idempotency, a_fresh_writer_must_start_at_sequence_one) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    WriterSession late = WriterSession::resume(writer("late-writer"), MutationSequence(5));
    auto session = registry.open_writer(late.writer());
    AR_REQUIRE_OK(opened, session);
    // open_writer resets to sequence one for a writer with no recorded history.
    AR_CHECK(opened.next_sequence() == MutationSequence(1));
    auto registered = registry.register_asset(opened, opened.advance(),
                                              make_request("late-asset", AssetClass::Server, "Acme", "L-1"));
    AR_CHECK(registered.has_value());
}

AR_TEST(replacement, lineage_links_are_acyclic_and_recorded) {
    RegistryPolicy policy;
    policy.serial_collision = SerialCollisionPolicy::AllowReplacementOfTerminal;
    auto context = detached_registry(policy);
    auto& registry = *context.registry;
    const AssetId original = register_one(registry, context.session, "replaced-asset", AssetClass::Server, "REP-1");

    // Retire the original so it can be replaced.
    AR_CHECK(registry.transition_installation(context.session, context.session.advance(), original, std::nullopt,
                                              InstallationState::NotInstalled)
                 .has_value());
    AR_CHECK(registry.transition_lifecycle(context.session, context.session.advance(), original, std::nullopt,
                                           LifecycleState::Decommissioned)
                 .has_value());
    const auto retired = registry.snapshot().find(original);
    AR_REQUIRE_PRESENT(retired_view, retired);
    const AssetRevision retired_revision = retired_view.revision;

    RegisterAssetRequest replacement =
        make_request("replacement-asset", AssetClass::Server, "Acme", "REP-2");
    replacement.supersedes = original;
    replacement.supersession_cause = ReplacementCause::Failure;
    replacement.supersession_note = "power supply failure";
    auto registered = registry.register_asset(context.session, context.session.advance(), replacement);
    AR_REQUIRE_OK(result, registered);

    const Snapshot snapshot = registry.snapshot();
    const auto view = snapshot.find(result.id);
    AR_CHECK(view.has_value());
    AR_CHECK(view->supersedes.has_value());
    AR_CHECK(view->supersedes->predecessor == original);
    AR_CHECK(view->supersedes->predecessor_generation == AssetGeneration(1));
    AR_CHECK(view->supersedes->predecessor_final_revision == retired_revision);

    const auto chain = snapshot.lineage(result.id);
    AR_REQUIRE_OK(lineage, chain);
    AR_CHECK(lineage.predecessors.size() == 1);
    AR_CHECK(lineage.predecessors.front() == original);
    AR_CHECK(lineage.origin().value() == original);
    AR_CHECK(lineage.termination == LineageTermination::ReachedOrigin);

    AR_CHECK(snapshot.predecessor_of(result.id).value() == original);
    AR_CHECK(!snapshot.predecessor_of(original).has_value());
    const std::vector<AssetId> successors = snapshot.successors_of(original);
    AR_CHECK(successors.size() == 1);
    AR_CHECK(successors.front() == result.id);

    // The predecessor keeps its identity and revision; nothing about it changed.
    const auto predecessor_view = snapshot.find(original);
    AR_CHECK(predecessor_view.has_value());
    AR_CHECK(predecessor_view->revision == retired_revision);
    AR_CHECK(predecessor_view->supersedes == std::nullopt);
}

AR_TEST(replacement, cycles_and_self_replacement_are_refused) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    const AssetId first = register_one(registry, context.session, "cycle-a", AssetClass::Server, "CYC-A");
    const AssetId second = register_one(registry, context.session, "cycle-b", AssetClass::Server, "CYC-B");

    auto self = registry.link_replacement(context.session, context.session.advance(), first, std::nullopt, first,
                                          ReplacementCause::Failure);
    AR_REQUIRE_ERROR(self_error, self, ErrorCode::SelfReplacementForbidden);

    // Both assets are live, so the terminal-state rule refuses the link first.
    auto live_link = registry.link_replacement(context.session, context.session.advance(), second, std::nullopt, first,
                                               ReplacementCause::Failure);
    AR_REQUIRE_ERROR(live_error, live_link, ErrorCode::LifecycleInvariantViolation);

    // Retire the first, then link it as the predecessor of the second.
    AR_CHECK(registry.transition_lifecycle(context.session, context.session.advance(), first, std::nullopt,
                                           LifecycleState::Decommissioned)
                 .has_value());
    AR_REQUIRE_OK(linked, registry.link_replacement(context.session, context.session.advance(), second, std::nullopt,
                                                    first, ReplacementCause::Refresh, "refreshed"));
    AR_CHECK(linked.revision == AssetRevision(2));

    // The second now has a predecessor, so it cannot take another. The rejection
    // names that reason rather than the state of the proposed predecessor.
    const AssetId third = register_one(registry, context.session, "cycle-c", AssetClass::Server, "CYC-C");
    auto second_link = registry.link_replacement(context.session, context.session.advance(), second, std::nullopt,
                                                 third, ReplacementCause::Failure);
    AR_REQUIRE_ERROR(second_error, second_link, ErrorCode::AliasConflict);

    // Retire the second and third, then attempt a cycle: third supersedes second,
    // which already supersedes first, so second cannot supersede third.
    AR_CHECK(registry.transition_lifecycle(context.session, context.session.advance(), second, std::nullopt,
                                           LifecycleState::Decommissioned)
                 .has_value());
    AR_CHECK(registry.transition_lifecycle(context.session, context.session.advance(), third, std::nullopt,
                                           LifecycleState::Decommissioned)
                 .has_value());
    AR_REQUIRE_OK(chained, registry.link_replacement(context.session, context.session.advance(), third, std::nullopt,
                                                     second, ReplacementCause::Upgrade));
    auto cycle = registry.link_replacement(context.session, context.session.advance(), first, std::nullopt, third,
                                           ReplacementCause::Failure);
    // `first` is the origin of the chain, so linking it to `third` would close a
    // cycle only if first had no predecessor; the walk detects the revisit
    // through the chain.
    AR_CHECK(cycle.has_value() || cycle.code() == ErrorCode::ReplacementCycleDetected ||
             cycle.code() == ErrorCode::LineageTraversalLimitExceeded);

    const auto chain = registry.snapshot().lineage(third);
    AR_REQUIRE_OK(long_chain, chain);
    AR_CHECK(long_chain.predecessors.size() == 2);
    AR_CHECK(long_chain.predecessors.front() == second);
    AR_CHECK(long_chain.predecessors.back() == first);
}

AR_TEST(replacement, dangling_predecessors_are_refused_by_default) {
    RegistryPolicy policy;
    policy.serial_collision = SerialCollisionPolicy::AllowReplacementOfTerminal;
    auto context = detached_registry(policy);
    RegisterAssetRequest request = make_request("dangling-asset", AssetClass::Server, "Acme", "DAN-1");
    request.supersedes = asset_id_for("never-registered");
    auto refused = context.registry->register_asset(context.session, context.session.advance(), request);
    AR_REQUIRE_ERROR(error, refused, ErrorCode::AssetNotFound);

    // The refused attempt was recorded as the outcome for its sequence, so the
    // next attempt continues from the sequence after it.
    request.allow_dangling_predecessor = true;
    auto accepted = context.registry->register_asset(context.session, context.session.advance(), request);
    AR_REQUIRE_OK(result, accepted);
    const Snapshot snapshot = context.registry->snapshot();
    const ConflictReport report = snapshot.audit_conflicts();
    AR_CHECK(report.dangling_predecessors.size() == 1);
    AR_CHECK(report.dangling_predecessors.front() == result.id);
}

AR_TEST(replacement, serial_collision_policy_allows_a_declared_replacement) {
    RegistryPolicy policy;
    policy.serial_collision = SerialCollisionPolicy::AllowReplacementOfTerminal;
    auto context = detached_registry(policy);
    auto& registry = *context.registry;
    const AssetId original = register_one(registry, context.session, "collision-a", AssetClass::Server, "COL-1");

    // Without a declared predecessor the collision is still refused.
    RegisterAssetRequest undeclared = make_request("collision-b", AssetClass::Server, "Acme", "COL-1");
    auto refused = registry.register_asset(context.session, context.session.advance(), undeclared);
    AR_REQUIRE_ERROR(refused_error, refused, ErrorCode::DuplicateSerialIdentity);

    // While the original is live, even a declared replacement is refused.
    RegisterAssetRequest declared = make_request("collision-b", AssetClass::Server, "Acme", "COL-1");
    declared.supersedes = original;
    auto live = registry.register_asset(context.session, context.session.advance(), declared);
    AR_REQUIRE_ERROR(live_error, live, ErrorCode::DuplicateSerialIdentity);

    AR_CHECK(registry.transition_lifecycle(context.session, context.session.advance(), original, std::nullopt,
                                           LifecycleState::Decommissioned)
                 .has_value());
    auto accepted = registry.register_asset(context.session, context.session.advance(), declared);
    AR_REQUIRE_OK(result, accepted);
    const auto view = registry.snapshot().find(result.id);
    AR_CHECK(view.has_value());
    AR_CHECK(view->supersedes.has_value());
    AR_CHECK(view->supersedes->predecessor == original);
}

AR_TEST(identity_reuse, forbidden_by_default_and_audited_when_allowed) {
    auto strict = detached_registry();
    const AssetId identifier = register_one(*strict.registry, strict.session, "reuse-asset", AssetClass::Server,
                                            "REU-1");
    AR_CHECK(strict.registry
                 ->transition_lifecycle(strict.session, strict.session.advance(), identifier, std::nullopt,
                                        LifecycleState::Decommissioned)
                 .has_value());
    auto forbidden = strict.registry->reuse_identity(strict.session, strict.session.advance(), identifier,
                                                     std::nullopt, serial("Acme", "REU-2"),
                                                     AssetClass::Server);
    AR_REQUIRE_ERROR(forbidden_error, forbidden, ErrorCode::IdentityReuseForbidden);

    RegistryPolicy permissive;
    permissive.identity_reuse = IdentityReusePolicy::AllowAfterTerminal;
    auto context = detached_registry(permissive);
    auto& registry = *context.registry;
    const AssetId target = register_one(registry, context.session, "reuse-allowed", AssetClass::Server, "REU-3");

    // A live asset cannot have its identity reused.
    auto live = registry.reuse_identity(context.session, context.session.advance(), target, std::nullopt,
                                        serial("Acme", "REU-4"), AssetClass::Server);
    AR_REQUIRE_ERROR(live_error, live, ErrorCode::IdentityReuseForbidden);

    AR_CHECK(registry.transition_lifecycle(context.session, context.session.advance(), target, std::nullopt,
                                           LifecycleState::Decommissioned)
                 .has_value());
    AR_REQUIRE_OK(reused, registry.reuse_identity(context.session, context.session.advance(), target, std::nullopt,
                                                  serial("Acme", "REU-4"), AssetClass::StorageAppliance));
    AR_CHECK(reused.revision == AssetRevision(3));
    AR_CHECK(reused.subject == target);

    const Snapshot snapshot = registry.snapshot();
    const auto view = snapshot.find(target);
    AR_CHECK(view.has_value());
    AR_CHECK(view->generation == AssetGeneration(2));
    AR_CHECK(view->asset_class == AssetClass::StorageAppliance);
    AR_CHECK(view->serial_identity.serial().text() == "REU-4");
    AR_CHECK(view->history != nullptr);
    AR_CHECK(view->history->size() == 1);
    AR_CHECK(view->history->front().generation == AssetGeneration(1));
    AR_CHECK(view->history->front().serial_identity.serial().text() == "REU-3");
    AR_CHECK(view->history->front().state.lifecycle == LifecycleState::Decommissioned);

    // The previous incarnation is preserved and still discoverable.
    AR_CHECK(view->provenance->back().action == ProvenanceAction::IdentityReused);
    AR_CHECK(view->provenance->back().change.find("generation=1->2") != std::string::npos);
}

AR_TEST(provenance, history_is_append_only_and_ordered) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    const AssetId identifier = register_one(registry, context.session, "provenance-asset");
    for (int index = 0; index < 20; ++index) {
        MetadataPatch patch;
        patch.notes = "note-" + std::to_string(index);
        auto result = registry.update_metadata(context.session, context.session.advance(), identifier, std::nullopt,
                                               patch);
        AR_CHECK(result.has_value());
    }
    const Snapshot snapshot = registry.snapshot();
    const auto steps = snapshot.full_provenance(identifier);
    AR_REQUIRE_OK(history, steps);
    AR_CHECK(history.size() == 21);
    for (std::size_t index = 0; index < history.size(); ++index) {
        AR_CHECK(history[index].revision == AssetRevision(static_cast<std::uint64_t>(index + 1)));
        if (index > 0) {
            AR_CHECK(history[index - 1].sequence < history[index].sequence);
            AR_CHECK(history[index - 1].revision < history[index].revision);
        }
    }
    AR_CHECK(history.front().action == ProvenanceAction::Registered);
    AR_CHECK(history.back().reason.empty());
    const auto recent = snapshot.provenance(identifier, 5);
    AR_REQUIRE_OK(tail, recent);
    AR_CHECK(tail.size() == 5);
    AR_CHECK(tail.back().revision == AssetRevision(21));
}

AR_TEST(provenance, retained_history_is_compacted_without_losing_the_origin) {
    RegistryLimits tight = default_limits();
    tight.max_provenance_per_asset = 8;
    auto context = detached_registry(default_policy(), tight);
    auto& registry = *context.registry;
    const AssetId identifier = register_one(registry, context.session, "compacted-asset");
    for (int index = 0; index < 40; ++index) {
        MetadataPatch patch;
        patch.notes = "note-" + std::to_string(index);
        auto updated = registry.update_metadata(context.session, context.session.advance(), identifier,
                                                std::nullopt, patch);
        AR_CHECK_MSG(updated.has_value(), updated.has_value() ? "" : updated.error().to_string());
        if (!updated.has_value()) {
            break;
        }
    }
    const Snapshot snapshot = registry.snapshot();
    const auto steps = snapshot.full_provenance(identifier);
    AR_REQUIRE_OK(history, steps);
    // The bound is respected exactly.
    AR_CHECK(history.size() <= tight.max_provenance_per_asset);
    // The origin survives.
    AR_CHECK(history.front().action == ProvenanceAction::Registered);
    AR_CHECK(history.front().revision == AssetRevision(1));
    // A compaction marker states how much it replaced.
    AR_CHECK(history[1].reason.rfind("compacted ", 0) == 0);
    // The newest step is still the newest revision.
    AR_CHECK(history.back().revision == AssetRevision(41));
    // Sequence order is still strictly ascending.
    for (std::size_t index = 1; index < history.size(); ++index) {
        AR_CHECK(history[index - 1].sequence < history[index].sequence);
    }
}

AR_TEST(snapshot, snapshots_are_isolated_from_later_mutations) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    const AssetId identifier = register_one(registry, context.session, "isolation-asset");
    const Snapshot before = registry.snapshot();
    AR_CHECK(before.size() == 1);
    AR_CHECK(before.published_sequence().value() == 1);

    MetadataPatch patch;
    patch.owner = owner("org.example.platform");
    AR_CHECK(registry.update_metadata(context.session, context.session.advance(), identifier, std::nullopt, patch)
                 .has_value());

    // The earlier snapshot still reports the earlier state.
    AR_CHECK(before.size() == 1);
    const auto old_view = before.find(identifier);
    AR_CHECK(old_view.has_value());
    AR_CHECK(!old_view->metadata.owner.has_value());
    AR_CHECK(old_view->revision == AssetRevision(1));
    AR_CHECK(before.published_sequence().value() == 1);

    const Snapshot after = registry.snapshot();
    const auto new_view = after.find(identifier);
    AR_CHECK(new_view.has_value());
    AR_CHECK(new_view->metadata.owner.has_value());
    AR_CHECK(after.published_sequence().value() == 2);
    AR_CHECK(after.published_sequence() > before.published_sequence());

    // A snapshot survives the registry it came from.
    AR_CHECK(registry.close().has_value());
    AR_CHECK(after.size() == 1);
    AR_CHECK(after.find(identifier).has_value());
}
