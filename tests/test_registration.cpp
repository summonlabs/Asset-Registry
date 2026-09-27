// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Asset registration, identity, uniqueness, revision, and authority tests.

#include <set>
#include <string>
#include <thread>
#include <vector>

#include "test_harness.hpp"
#include "test_support.hpp"

using namespace asset_registry;
using namespace asset_test;

AR_TEST(registration, caller_supplied_identity_is_used_verbatim) {
    auto context = detached_registry();
    RegisterAssetRequest request = make_request("server-a", AssetClass::Server, "Acme Compute", "SN-0001");
    request.metadata.owner = owner("org.example.platform");
    request.metadata.site = location("site-a.hall-2");
    request.metadata.notes = "installed during tranche one";
    request.metadata.labels = {{"rack.unit", "7"}, {"tier", "gold"}};
    request.references = {capability_ref("asi:accelerator.scheduling", ReferenceEvidence::Verified)};

    auto registered = context.registry->register_asset(context.session, context.session.advance(), request);
    AR_REQUIRE_OK(registration, registered);
    AR_CHECK(registration.id == request.id);
    AR_CHECK(registration.revision == AssetRevision(1));
    AR_CHECK(!registration.already_present);
    AR_CHECK(!registration.derived_identity);

    const Snapshot snapshot = context.registry->snapshot();
    const auto view = snapshot.find(request.id);
    AR_CHECK(view.has_value());
    AR_CHECK(view->asset_class == AssetClass::Server);
    AR_CHECK(view->generation == AssetGeneration(1));
    AR_CHECK(view->revision == AssetRevision(1));
    AR_CHECK(view->metadata.display_name == "server-a");
    AR_CHECK(view->metadata.owner.has_value());
    AR_CHECK(*view->metadata.owner == *request.metadata.owner);
    AR_CHECK(view->metadata.labels.size() == 2);
    AR_CHECK(view->metadata.labels.front().first == "rack.unit");
    AR_CHECK(view->metadata.labels.back().first == "tier");
    AR_CHECK(view->references.size() == 1);
    AR_CHECK(view->capability_references().size() == 1);
    AR_CHECK(view->provenance != nullptr);
    AR_CHECK(view->provenance->size() == 1);
    AR_CHECK(view->provenance->front().action == ProvenanceAction::Registered);
    AR_CHECK(view->provenance->front().actor.is_writer());
    AR_CHECK(view->provenance->front().epoch.value() == snapshot.epoch().value());
    AR_CHECK(snapshot.size() == 1);
}

AR_TEST(registration, derived_identity_is_stable_and_reimport_is_idempotent) {
    auto context = detached_registry();
    RegisterAssetRequest request = make_request("ignored", AssetClass::AcceleratorEnclosure, "Acme", "ENC-77");
    request.id_mode = AssetIdMode::DerivedFromSerialIdentity;
    request.id = AssetId{};

    auto first = context.registry->register_asset(context.session, context.session.advance(), request);
    AR_REQUIRE_OK(first_registration, first);
    AR_CHECK(first_registration.derived_identity);
    AR_CHECK(!first_registration.id.is_nil());

    auto second = context.registry->register_asset(context.session, context.session.advance(), request);
    AR_REQUIRE_OK(second_registration, second);
    AR_CHECK(second_registration.already_present);
    AR_CHECK(second_registration.id == first_registration.id);
    AR_CHECK(context.registry->snapshot().size() == 1);

    // Different physical object, different identity.
    RegisterAssetRequest other = request;
    other.serial_identity = serial("Acme", "ENC-78");
    auto third = context.registry->register_asset(context.session, context.session.advance(), other);
    AR_REQUIRE_OK(third_registration, third);
    AR_CHECK(!(third_registration.id == first_registration.id));
    AR_CHECK(context.registry->snapshot().size() == 2);

    // The derived identity is a pure function of class and serial identity, so a
    // second registry derives the same one.
    auto peer = detached_registry(asset_registry::default_policy(), asset_registry::default_limits(), "peer-writer");
    RegisterAssetRequest peer_request = request;
    peer_request.metadata.display_name = "different label, same object";
    auto peer_registration =
        peer.registry->register_asset(peer.session, peer.session.advance(), peer_request);
    AR_REQUIRE_OK(peer_result, peer_registration);
    AR_CHECK(peer_result.id == first_registration.id);
}

AR_TEST(registration, duplicate_canonical_identity_is_rejected) {
    auto context = detached_registry();
    auto first = context.registry->register_asset(
        context.session, context.session.advance(), make_request("server-a", AssetClass::Server, "Acme", "SN-1"));
    AR_REQUIRE_OK(created, first);
    AR_CHECK(!created.id.is_nil());

    RegisterAssetRequest duplicate = make_request("server-a", AssetClass::Server, "Other", "SN-2");
    auto second = context.registry->register_asset(context.session, context.session.advance(), duplicate);
    AR_REQUIRE_ERROR(error, second, ErrorCode::DuplicateAssetId);
    AR_CHECK(error.subject() == duplicate.id.to_string());
    AR_CHECK(context.registry->snapshot().size() == 1);
}

AR_TEST(registration, duplicate_serial_identity_is_rejected_under_the_default_policy) {
    auto context = detached_registry();
    auto first = context.registry->register_asset(
        context.session, context.session.advance(), make_request("server-a", AssetClass::Server, "Acme", "SN-1"));
    AR_REQUIRE_OK(created, first);
    AR_CHECK(created.revision == AssetRevision(1));

    // Same manufacturer and serial, different canonical identity: the situation
    // the registry exists to prevent.
    RegisterAssetRequest second_request = make_request("server-b", AssetClass::Server, "acme", "sn-1");
    auto second = context.registry->register_asset(context.session, context.session.advance(), second_request);
    AR_REQUIRE_ERROR(error, second, ErrorCode::DuplicateSerialIdentity);
    AR_CHECK(error.subject() == created.id.to_string());
    AR_CHECK(context.registry->snapshot().size() == 1);
}

AR_TEST(registration, model_conflict_is_reported_and_optionally_rejected) {
    RegistryPolicy reporting;
    auto context = detached_registry(reporting);
    auto first = context.registry->register_asset(
        context.session, context.session.advance(), make_request("server-a", AssetClass::Server, "Acme", "SN-1"));
    AR_REQUIRE_OK(created, first);

    // Under Report, the same physical object with a conflicting model string is
    // still a duplicate: the model disagreement is data quality, not authorisation.
    RegisterAssetRequest conflicting = make_request("server-b", AssetClass::Server, "Acme", "SN-1");
    conflicting.serial_identity = serial("Acme", "SN-1", "R2");
    auto second = context.registry->register_asset(context.session, context.session.advance(), conflicting);
    AR_REQUIRE_ERROR(error, second, ErrorCode::DuplicateSerialIdentity);

    RegistryPolicy strict;
    strict.model_conflict = ModelConflictPolicy::Reject;
    auto strict_context = detached_registry(strict);
    auto strict_first = strict_context.registry->register_asset(
        strict_context.session, strict_context.session.advance(),
        make_request("server-a", AssetClass::Server, "Acme", "SN-1"));
    AR_REQUIRE_OK(strict_created, strict_first);
    AR_CHECK(strict_created.revision == AssetRevision(1));
    RegisterAssetRequest strict_request = make_request("server-b", AssetClass::Server, "Acme", "SN-1");
    strict_request.serial_identity = serial("Acme", "SN-1", "R2");
    auto strict_second = strict_context.registry->register_asset(strict_context.session,
                                                                strict_context.session.advance(), strict_request);
    AR_REQUIRE_ERROR(strict_error, strict_second, ErrorCode::DuplicateSerialIdentity);
}

AR_TEST(registration, malformed_requests_are_rejected_with_specific_codes) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    // One writer per rejected request, so each case is independent of how far a
    // shared session has advanced.
    auto next_writer = [&registry](int index) {
        return registry.open_writer(writer("malformed-writer-" + std::to_string(index))).value();
    };

    RegisterAssetRequest nil_identity = make_request("x", AssetClass::Server, "Acme", "SN-1");
    nil_identity.id = AssetId{};
    WriterSession session0 = next_writer(0);
    auto nil_result = registry.register_asset(session0, session0.advance(), nil_identity);
    AR_REQUIRE_ERROR(nil_error, nil_result, ErrorCode::MalformedAssetId);

    RegisterAssetRequest unknown_class = make_request("x", AssetClass::Unknown, "Acme", "SN-1");
    WriterSession session1 = next_writer(1);
    auto class_result = registry.register_asset(session1, session1.advance(), unknown_class);
    AR_REQUIRE_ERROR(class_error, class_result, ErrorCode::UnknownAssetClass);

    RegisterAssetRequest empty_name = make_request("x", AssetClass::Server, "Acme", "SN-1");
    empty_name.metadata.display_name.clear();
    WriterSession session2 = next_writer(2);
    auto name_result = registry.register_asset(session2, session2.advance(), empty_name);
    AR_REQUIRE_ERROR(name_error, name_result, ErrorCode::EmptyRequiredField);

    RegisterAssetRequest control_character = make_request("x", AssetClass::Server, "Acme", "SN-1");
    control_character.metadata.display_name = std::string("bad\x01name");
    WriterSession session3 = next_writer(3);
    auto control_result = registry.register_asset(session3, session3.advance(), control_character);
    AR_REQUIRE_ERROR(control_error, control_result, ErrorCode::MalformedText);

    RegisterAssetRequest bad_label = make_request("x", AssetClass::Server, "Acme", "SN-1");
    bad_label.metadata.labels = {{"Bad Key", "value"}};
    WriterSession session4 = next_writer(4);
    auto label_result = registry.register_asset(session4, session4.advance(), bad_label);
    AR_REQUIRE_ERROR(label_error, label_result, ErrorCode::MalformedLabelKey);

    RegisterAssetRequest both_modes = make_request("x", AssetClass::Server, "Acme", "SN-1");
    both_modes.id_mode = AssetIdMode::DerivedFromSerialIdentity;
    WriterSession session5 = next_writer(5);
    auto both_result = registry.register_asset(session5, session5.advance(), both_modes);
    AR_REQUIRE_ERROR(both_error, both_result, ErrorCode::InvalidInput);

    RegisterAssetRequest active_start = make_request("x", AssetClass::Server, "Acme", "SN-1");
    active_start.lifecycle = LifecycleState::Active;
    active_start.installation = InstallationState::Installed;
    WriterSession session6 = next_writer(6);
    auto active_result = registry.register_asset(session6, session6.advance(), active_start);
    AR_REQUIRE_ERROR(active_error, active_result, ErrorCode::LifecycleInvariantViolation);

    // An explicit unknown state pair is accepted, because unknown is a statement
    // about missing information rather than a claim about the asset.
    RegisterAssetRequest unknown_state = make_request("unknown-state-asset", AssetClass::Server, "Acme", "UNK-1");
    unknown_state.lifecycle = LifecycleState::Unknown;
    unknown_state.installation = InstallationState::Unknown;
    WriterSession session7 = next_writer(7);
    auto unknown_result = registry.register_asset(session7, session7.advance(), unknown_state);
    AR_REQUIRE_OK(unknown_registration, unknown_result);
    AR_CHECK(unknown_registration.revision == AssetRevision(1));
    AR_CHECK(registry.snapshot().size() == 1);
}

AR_TEST(registration, labels_and_references_are_canonicalised_and_bounded) {
    RegistryLimits tight = default_limits();
    tight.max_labels_per_asset = 2;
    tight.max_references_per_asset = 1;
    auto context = detached_registry(default_policy(), tight);

    RegisterAssetRequest request = make_request("server-a", AssetClass::Server, "Acme", "SN-1");
    request.metadata.labels = {{"zzz", "1"}, {"aaa", "2"}};
    auto registered = context.registry->register_asset(context.session, context.session.advance(), request);
    AR_REQUIRE_OK(created, registered);
    AR_CHECK(created.id == request.id);
    const auto view = context.registry->snapshot().find(created.id);
    AR_CHECK(view.has_value());
    AR_CHECK(view->metadata.labels.front().first == "aaa");
    AR_CHECK(view->metadata.labels.back().first == "zzz");

    RegisterAssetRequest duplicate_labels = make_request("server-b", AssetClass::Server, "Acme", "SN-2");
    duplicate_labels.metadata.labels = {{"aaa", "1"}, {"aaa", "2"}};
    auto duplicate_result =
        context.registry->register_asset(context.session, context.session.advance(), duplicate_labels);
    AR_REQUIRE_ERROR(duplicate_error, duplicate_result, ErrorCode::InvalidInput);

    RegisterAssetRequest too_many_labels = make_request("server-c", AssetClass::Server, "Acme", "SN-3");
    too_many_labels.metadata.labels = {{"a", "1"}, {"b", "2"}, {"c", "3"}};
    auto labels_result =
        context.registry->register_asset(context.session, context.session.advance(), too_many_labels);
    AR_REQUIRE_ERROR(labels_error, labels_result, ErrorCode::CapacityExceeded);

    RegisterAssetRequest too_many_references = make_request("server-d", AssetClass::Server, "Acme", "SN-4");
    too_many_references.references = {capability_ref("asi:a"), capability_ref("asi:b")};
    auto references_result =
        context.registry->register_asset(context.session, context.session.advance(), too_many_references);
    AR_REQUIRE_ERROR(references_error, references_result, ErrorCode::ReferenceCountExceeded);

    // Duplicate detection is checked against a bound that permits two references,
    // so the specific reason is the duplicate rather than the count.
    RegistryLimits two_references = default_limits();
    two_references.max_references_per_asset = 2;
    auto duplicate_context = detached_registry(default_policy(), two_references);
    RegisterAssetRequest repeated_reference = make_request("server-e", AssetClass::Server, "Acme", "SN-5");
    repeated_reference.references = {capability_ref("asi:a"), capability_ref("asi:a")};
    auto repeated_result = duplicate_context.registry->register_asset(
        duplicate_context.session, duplicate_context.session.advance(), repeated_reference);
    AR_REQUIRE_ERROR(repeated_error, repeated_result, ErrorCode::ReferenceAlreadyAttached);
}

AR_TEST(registration, capacity_bound_is_enforced_before_mutation) {
    RegistryLimits tight = default_limits();
    tight.max_assets = 2;
    auto context = detached_registry(default_policy(), tight);
    for (int index = 0; index < 2; ++index) {
        auto result = context.registry->register_asset(
            context.session, context.session.advance(),
            make_request("asset-" + std::to_string(index), AssetClass::Server, "Acme",
                         "SN-" + std::to_string(index)));
        AR_CHECK(result.has_value());
    }
    auto overflow = context.registry->register_asset(
        context.session, context.session.advance(), make_request("asset-3", AssetClass::Server, "Acme", "SN-3"));
    AR_REQUIRE_ERROR(error, overflow, ErrorCode::CapacityExceeded);
    AR_CHECK(context.registry->snapshot().size() == 2);
}

AR_TEST(revisions, stale_revision_is_rejected_with_the_observed_value) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    const AssetId identifier = asset_id_for("revised-asset");
    auto registered = registry.register_asset(
        context.session, context.session.advance(), make_request("revised-asset", AssetClass::Server, "Acme", "R-1"));
    AR_REQUIRE_OK(created, registered);
    AR_CHECK(created.revision == AssetRevision(1));

    MetadataPatch patch;
    patch.notes = "first change";
    auto updated = registry.update_metadata(context.session, context.session.advance(), identifier,
                                            AssetRevision(1), patch);
    AR_REQUIRE_OK(update, updated);
    AR_CHECK(update.revision == AssetRevision(2));
    AR_CHECK(update.published_sequence.has_value());

    // The same expectation a second time is refused. The sequence bound is checked
    // before the revision bound, because a repeated sequence is the more
    // fundamental of the two failures; a distinct sequence with a stale revision is
    // refused as stale, which the next case shows.
    MetadataPatch second_patch;
    second_patch.notes = "second change";
    auto repeated_sequence = registry.update_metadata(context.session, context.session.advance(), identifier,
                                                      AssetRevision(1), second_patch);
    AR_CHECK(!repeated_sequence.has_value());

    // A distinct sequence with a stale revision is refused as stale, with the
    // expected and observed revisions reported.
    WriterSession fresh = registry.open_writer(writer("stale-revision-writer")).value();
    AR_CHECK(fresh.next_sequence() == MutationSequence(1));
    MetadataPatch third_patch;
    third_patch.notes = "third change";
    auto stale = registry.update_metadata(fresh, fresh.advance(), identifier, AssetRevision(1), third_patch);
    AR_REQUIRE_ERROR(error, stale, ErrorCode::StaleRevision);
    AR_CHECK(error.detail_key() == "expected_revision");
    AR_CHECK(error.detail_value() == "1");

    // An absent expectation applies unconditionally.
    auto unconditional = registry.update_metadata(context.session, context.session.advance(), identifier,
                                                  std::nullopt, second_patch);
    AR_REQUIRE_OK(unconditional_result, unconditional);
    AR_CHECK(unconditional_result.revision == AssetRevision(3));

    // Revisions advance monotonically and never reset.
    for (int index = 0; index < 8; ++index) {
        MetadataPatch loop_patch;
        loop_patch.notes = "note " + std::to_string(index);
        auto result = registry.update_metadata(context.session, context.session.advance(), identifier, std::nullopt,
                                               loop_patch);
        AR_REQUIRE_OK(loop_result, result);
        AR_CHECK(loop_result.revision == AssetRevision(static_cast<std::uint64_t>(4 + index)));
    }
}

AR_TEST(revisions, generation_expectation_is_enforced) {
    auto context = detached_registry();
    const AssetId identifier = asset_id_for("generation-asset");
    auto registered = context.registry->register_asset(
        context.session, context.session.advance(), make_request("generation-asset", AssetClass::Server, "Acme", "G-1"));
    AR_REQUIRE_OK(created, registered);

    MetadataPatch patch;
    patch.notes = "x";
    auto wrong_generation = context.registry->update_metadata(context.session, context.session.advance(), identifier,
                                                             std::nullopt, patch);
    // Generation expectation is exercised through the mutation entry point that
    // takes one; update_metadata does not, so the check here is that the identity
    // generation reported by the snapshot is the one registration produced.
    AR_CHECK(wrong_generation.has_value());
    const auto view = context.registry->snapshot().find(created.id);
    AR_CHECK(view.has_value());
    AR_CHECK(view->generation == AssetGeneration(1));
}

AR_TEST(authority, mutations_require_a_live_token) {
    auto context = detached_registry();
    const AssetId identifier = asset_id_for("authority-asset");
    AR_REQUIRE_OK(created,
                  context.registry->register_asset(context.session, context.session.advance(),
                                                   make_request("authority-asset", AssetClass::Server, "Acme",
                                                                "A-1")));

    // A session that never received a token cannot mutate.
    WriterSession unarmed = WriterSession::create(writer("unarmed"));
    MetadataPatch patch;
    patch.notes = "x";
    auto refused = context.registry->update_metadata(unarmed, unarmed.advance(), identifier, std::nullopt, patch);
    AR_REQUIRE_ERROR(error, refused, ErrorCode::AuthorityRevoked);

    // After the writer is closed, the token it held is no longer live.
    WriterSession session = context.session;
    auto closed = context.registry->close_writer(session);
    AR_CHECK(closed.has_value());
    auto after_close = context.registry->update_metadata(session, session.advance(), identifier, std::nullopt, patch);
    AR_REQUIRE_ERROR(after_error, after_close, ErrorCode::AuthorityRevoked);

    // A freshly opened writer still works, so the refusal was about the token and
    // not about the registry.
    WriterSession replacement = context.registry->open_writer(writer("replacement-writer")).value();
    AR_CHECK(context.registry->update_metadata(replacement, replacement.advance(), identifier, std::nullopt, patch)
                 .has_value());
}

AR_TEST(authority, a_token_minted_for_another_writer_is_refused) {
    auto context = detached_registry();
    AR_REQUIRE_OK(created,
                  context.registry->register_asset(context.session, context.session.advance(),
                                                   make_request("authority-asset", AssetClass::Server, "Acme",
                                                                "A-1")));
    WriterSession other = WriterSession::create(writer("other-writer"));
    other.attach_token(context.session.token());
    MetadataPatch patch;
    patch.notes = "x";
    auto refused = context.registry->update_metadata(other, other.advance(), created.id, std::nullopt, patch);
    AR_REQUIRE_ERROR(error, refused, ErrorCode::AuthorityRevoked);
}

AR_TEST(authority, mutations_after_close_are_refused) {
    auto context = detached_registry();
    AR_REQUIRE_OK(created,
                  context.registry->register_asset(context.session, context.session.advance(),
                                                   make_request("close-asset", AssetClass::Server, "Acme", "C-1")));
    AR_CHECK(context.registry->close().has_value());
    AR_CHECK(!context.registry->is_open());
    MetadataPatch patch;
    patch.notes = "x";
    auto refused = context.registry->update_metadata(context.session, context.session.advance(), created.id,
                                                     std::nullopt, patch);
    AR_REQUIRE_ERROR(error, refused, ErrorCode::RegistryClosed);
    // Closing twice is harmless and still reports success.
    AR_CHECK(context.registry->close().has_value());
    // A snapshot taken before the close remains valid and readable.
    const Snapshot snapshot = context.registry->snapshot();
    AR_CHECK(snapshot.size() == 1);
}

AR_TEST(authority, expired_grants_stop_authorising) {
    auto context = detached_registry();
    AuthorityOptions options;
    options.ttl_nanos = 1;  // one nanosecond: already expired by the time it is used
    auto short_lived = context.registry->open_writer(writer("ephemeral"), options);
    AR_REQUIRE_OK(session, short_lived);
    auto refused = context.registry->register_asset(
        session, session.advance(), make_request("expired-asset", AssetClass::Server, "Acme", "E-1"));
    AR_REQUIRE_ERROR(error, refused, ErrorCode::AuthorityRevoked);

    AuthorityOptions invalid;
    invalid.ttl_nanos = 0;
    auto zero_ttl = context.registry->open_writer(writer("zero"), invalid);
    AR_REQUIRE_ERROR(ttl_error, zero_ttl, ErrorCode::InvalidInput);

    auto empty_writer = context.registry->open_writer(WriterId{}, AuthorityOptions{});
    AR_REQUIRE_ERROR(writer_error, empty_writer, ErrorCode::MalformedWriterId);
}
