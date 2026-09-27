// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Durable store tests: commit, reopen, recovery, integrity, corruption,
// locking, and epoch fencing.
//
// Every test in this file exercises real files through the real store: nothing is
// simulated, and the reopen paths go through the same open() a new process uses.

#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "test_harness.hpp"
#include "test_support.hpp"

using namespace asset_registry;
using namespace asset_test;

namespace {

/// Commits a known inventory and closes, so a reopen test starts from real bytes.
void seed_store(const std::filesystem::path& directory, int count) {
    auto context = open_registry(directory);
    for (int index = 0; index < count; ++index) {
        const std::string label = "durable-" + std::to_string(index);
        auto registered = context.registry->register_asset(
            context.session, context.session.advance(),
            make_request(label, AssetClass::Server, "Acme", "SN-" + std::to_string(index)));
        AR_CHECK(registered.has_value());
    }
    AR_CHECK(context.registry->close().has_value());
}


}  // namespace

AR_TEST(durability, committed_state_survives_close_and_reopen) {
    TempDirectory directory("durable-reopen");
    {
        auto context = open_registry(directory.path());
        for (int index = 0; index < 5; ++index) {
            auto registered = context.registry->register_asset(
                context.session, context.session.advance(),
                make_request("durable-" + std::to_string(index), AssetClass::Server, "Acme",
                             "SN-" + std::to_string(index)));
            AR_CHECK(registered.has_value());
        }
        AR_CHECK(context.registry->snapshot().size() == 5);
        // The store directory holds the documented layout.
        AR_CHECK(path_exists(directory.file("lock")));
        AR_CHECK(path_exists(directory.file("meta")));
        AR_CHECK(path_exists(directory.file("CURRENT")));
        AR_CHECK(path_exists(directory.path() / "generations"));
        AR_CHECK(!list_directory(directory.path() / "generations").empty());
        AR_CHECK(context.registry->close().has_value());
    }
    // Reopening is a new incarnation: a strictly greater epoch, the same inventory.
    {
        auto context = open_registry(directory.path());
        AR_CHECK(context.registry->authority_mode() == AuthorityMode::RegistryAuthority);
        AR_CHECK(context.report.action == RecoveryAction::None || context.report.action == RecoveryAction::Initialised);
        AR_CHECK(context.report.epoch.value() >= 2);
        const Snapshot snapshot = context.registry->snapshot();
        AR_CHECK(snapshot.size() == 5);
        for (int index = 0; index < 5; ++index) {
            const std::string label = "durable-" + std::to_string(index);
            const auto view = snapshot.find(asset_id_for(label));
            AR_CHECK_MSG(view.has_value(), label);
            if (!view.has_value()) {
                continue;
            }
            AR_CHECK(view->metadata.display_name == label);
            AR_CHECK(view->serial_identity.serial().text() == "SN-" + std::to_string(index));
            AR_CHECK(view->revision == AssetRevision(1));
            AR_CHECK(view->provenance->size() == 1);
            AR_CHECK(view->provenance->front().action == ProvenanceAction::Registered);
        }
        AR_CHECK(context.registry->close().has_value());
    }
}

AR_TEST(durability, reopen_restores_revisions_states_references_and_lineage_exactly) {
    TempDirectory directory("durable-rich");
    const AssetId original = asset_id_for("rich-original");
    const AssetId replacement = asset_id_for("rich-replacement");
    std::uint64_t expected_sequence = 0;
    {
        RegistryPolicy policy;
        policy.serial_collision = SerialCollisionPolicy::AllowReplacementOfTerminal;
        auto context = open_registry(directory.path(), policy);
        auto& registry = *context.registry;

        RegisterAssetRequest request = make_request("rich-original", AssetClass::Server, "Acme", "RICH-1");
        request.metadata.owner = owner("org.example.platform");
        request.metadata.site = location("site-a.hall-2");
        request.metadata.labels = {{"tier", "gold"}};
        request.references = {capability_ref("asi:accelerator.scheduling", ReferenceEvidence::Verified)};
        AR_CHECK(registry.register_asset(context.session, context.session.advance(), request).has_value());

        MetadataPatch patch;
        patch.notes = "second revision";
        AR_CHECK(registry.update_metadata(context.session, context.session.advance(), original, std::nullopt, patch)
                     .has_value());
        AR_CHECK(registry.transition_installation(context.session, context.session.advance(), original, std::nullopt,
                                                  InstallationState::NotInstalled)
                     .has_value());
        AR_CHECK(registry.transition_lifecycle(context.session, context.session.advance(), original, std::nullopt,
                                               LifecycleState::Decommissioned)
                     .has_value());

        RegisterAssetRequest replacement_request =
            make_request("rich-replacement", AssetClass::Server, "Acme", "RICH-2");
        replacement_request.supersedes = original;
        replacement_request.supersession_cause = ReplacementCause::Upgrade;
        replacement_request.supersession_note = "capacity upgrade";
        AR_CHECK(registry.register_asset(context.session, context.session.advance(), replacement_request)
                     .has_value());
        expected_sequence = registry.published_sequence().value();
        AR_CHECK(registry.close().has_value());
    }
    {
        RegistryPolicy policy;
        policy.serial_collision = SerialCollisionPolicy::AllowReplacementOfTerminal;
        auto context = open_registry(directory.path(), policy);
        const Snapshot snapshot = context.registry->snapshot();
        AR_CHECK(snapshot.size() == 2);
        AR_CHECK(snapshot.published_sequence().value() == expected_sequence);

        const auto original_view = snapshot.find(original);
        AR_REQUIRE_PRESENT(before, original_view);
        // Four mutations were applied to the original — registration, a metadata
        // update, an installation transition, and a lifecycle transition — and every
        // one of them advances the record by exactly one revision, which is what the
        // four retained provenance steps record.
        AR_CHECK(before.revision == AssetRevision(4));
        AR_CHECK(before.metadata.notes == "second revision");
        AR_CHECK(before.metadata.owner.has_value());
        AR_CHECK(*before.metadata.owner == owner("org.example.platform"));
        AR_CHECK(before.metadata.labels.size() == 1);
        AR_CHECK(before.references.size() == 1);
        AR_CHECK(before.references.front().evidence() == ReferenceEvidence::Verified);
        AR_CHECK(before.state.lifecycle == LifecycleState::Decommissioned);
        AR_CHECK(before.state.installation == InstallationState::NotInstalled);
        AR_CHECK(before.provenance->size() == 4);
        AR_CHECK(before.provenance->front().action == ProvenanceAction::Registered);
        AR_CHECK(before.provenance->back().action == ProvenanceAction::LifecycleTransitioned);

        const auto replacement_view = snapshot.find(replacement);
        AR_REQUIRE_PRESENT(after, replacement_view);
        AR_CHECK(after.supersedes.has_value());
        AR_CHECK(after.supersedes->predecessor == original);
        AR_CHECK(after.supersedes->cause == ReplacementCause::Upgrade);
        AR_CHECK(after.supersedes->predecessor_final_revision == AssetRevision(4));
        AR_CHECK(after.supersedes->note == "capacity upgrade");

        const auto chain = snapshot.lineage(replacement);
        AR_REQUIRE_OK(lineage, chain);
        AR_CHECK(lineage.predecessors.size() == 1);
        AR_CHECK(lineage.predecessors.front() == original);
        AR_CHECK(context.registry->close().has_value());
    }
}

AR_TEST(durability, every_epoch_increase_fences_previous_authority) {
    TempDirectory directory("durable-epoch");
    auto first = open_registry(directory.path());
    const RegistryEpoch first_epoch = first.registry->epoch();
    AR_CHECK(first.registry->close().has_value());

    auto second = open_registry(directory.path(), default_policy(), default_limits(), "second-writer");
    const RegistryEpoch second_epoch = second.registry->epoch();
    AR_CHECK(second_epoch > first_epoch);
    // A token minted by the earlier incarnation is not live in this one.
    AR_CHECK(!second.registry->store()->is_token_live(first.session.token()));
    AR_CHECK(second.registry->store()->is_token_live(second.session.token()));

    // Using it is refused with the epoch-specific code.
    auto refused = second.registry->register_asset(
        first.session, first.session.advance(),
        make_request("fenced-asset", AssetClass::Server, "Acme", "FENCE-1"));
    AR_REQUIRE_ERROR(error, refused, ErrorCode::StaleAuthorityEpoch);
    AR_CHECK(second.registry->snapshot().empty());
    AR_CHECK(second.registry->close().has_value());
}

AR_TEST(durability, a_second_writer_is_refused_the_store_lock) {
    TempDirectory directory("durable-lock");
    auto first = open_registry(directory.path());
    RecoveryReport report;
    auto second = AssetRegistry::open(directory.path(), StoreOpenMode::CreateIfMissing, StoreOpenOptions{}, report);
    AR_REQUIRE_ERROR(error, second, ErrorCode::StoreLocked);

    // Read-only access is still available while a writer holds the store.
    RecoveryReport read_report;
    auto reader = AssetRegistry::open(directory.path(), StoreOpenMode::ReadOnly, StoreOpenOptions{}, read_report);
    AR_REQUIRE_OK(read_only, reader);
    AR_CHECK(read_only->authority_mode() == AuthorityMode::RegistryAuthority);
    // It observes committed state only.
    AR_CHECK(read_only->snapshot().size() == 0);
    AR_CHECK(read_only->close().has_value());

    AR_CHECK(first.registry->close().has_value());
    // After release the lock is available again.
    RecoveryReport third_report;
    auto third = AssetRegistry::open(directory.path(), StoreOpenMode::CreateIfMissing, StoreOpenOptions{},
                                     third_report);
    AR_CHECK(third.has_value());
    if (third.has_value()) {
        AR_CHECK(third.value()->close().has_value());
    }
}

AR_TEST(recovery, truncated_authoritative_generation_rolls_back_to_the_last_valid_one) {
    TempDirectory directory("durable-truncate");
    auto context = open_registry(directory.path());
    for (int index = 0; index < 3; ++index) {
        AR_CHECK(context.registry->register_asset(
            context.session, context.session.advance(),
            make_request("keep-" + std::to_string(index), AssetClass::Server, "Acme",
                         "KEEP-" + std::to_string(index)))
                      .has_value());
    }
    const TransactionSequence committed = context.registry->published_sequence();
    AR_CHECK(context.registry->close().has_value());

    const std::vector<std::string> names = list_directory(directory.path() / "generations");
    AR_CHECK(names.size() >= 2);
    // Truncate the authoritative generation in place.
    const std::filesystem::path newest = directory.path() / "generations" / names.back();
    const std::string original = read_text_file(newest);
    AR_CHECK(!original.empty());
    write_text_file(newest, original.substr(0, original.size() / 2));

    auto reopened = open_registry(directory.path());
    AR_CHECK(reopened.report.action == RecoveryAction::RolledBackToLastValid ||
             reopened.report.action == RecoveryAction::DamagedGenerationQuarantined);
    AR_CHECK(reopened.report.store_modified);
    AR_CHECK(!reopened.report.detail.empty());
    const Snapshot snapshot = reopened.registry->snapshot();
    // The store recovered to a state it can prove: the inventory is a prefix of
    // what was committed, and never a fabricated or merged state.
    AR_CHECK(snapshot.size() <= 3);
    AR_CHECK(snapshot.published_sequence() <= committed);
    AR_CHECK(!snapshot.published_sequence().is_zero());
    for (const AssetId& identifier : snapshot.ids()) {
        const auto view = snapshot.find(identifier);
        AR_CHECK(view.has_value());
        AR_CHECK(view->provenance->size() == 1);
    }
    AR_CHECK(reopened.registry->close().has_value());
}

AR_TEST(recovery, bit_flip_in_a_payload_is_detected_and_not_published) {
    TempDirectory directory("durable-bitflip");
    auto context = open_registry(directory.path());
    for (int index = 0; index < 3; ++index) {
        AR_CHECK(context.registry->register_asset(
            context.session, context.session.advance(),
            make_request("flip-" + std::to_string(index), AssetClass::Server, "Acme",
                         "FLIP-" + std::to_string(index)))
                      .has_value());
    }
    AR_CHECK(context.registry->close().has_value());

    const std::vector<std::string> names = list_directory(directory.path() / "generations");
    const std::filesystem::path newest = directory.path() / "generations" / names.back();
    std::string bytes = read_text_file(newest);
    AR_CHECK(bytes.size() > 100);
    // Flip one bit deep inside the payload, leaving the header intact.
    bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x40);
    write_text_file(newest, bytes);

    auto reopened = open_registry(directory.path());
    AR_CHECK(reopened.report.content_rolled_back || reopened.report.action == RecoveryAction::RolledBackToLastValid);
    AR_CHECK(reopened.registry->snapshot().size() <= 3);
    AR_CHECK(reopened.registry->close().has_value());
}

AR_TEST(recovery, a_corrupt_header_is_detected_by_the_header_checksum) {
    TempDirectory directory("durable-header");
    auto context = open_registry(directory.path());
    // Two commits, so a fully valid generation exists to recover to. The case is
    // about the header checksum rejecting the damaged file, not about a store whose
    // only generation is damaged: that store has nothing trustworthy to publish and
    // is refused, which is the conservative outcome.
    for (int index = 0; index < 2; ++index) {
        AR_CHECK(context.registry->register_asset(
            context.session, context.session.advance(),
            make_request("header-asset-" + std::to_string(index), AssetClass::Server, "Acme",
                         "HDR-" + std::to_string(index)))
                      .has_value());
    }
    AR_CHECK(context.registry->close().has_value());

    const std::vector<std::string> names = list_directory(directory.path() / "generations");
    AR_CHECK(names.size() >= 2);
    const std::filesystem::path newest = directory.path() / "generations" / names.back();
    std::string bytes = read_text_file(newest);
    // Corrupt the declared payload length in the header.
    bytes[35] = static_cast<char>(bytes[35] ^ 0x01);
    write_text_file(newest, bytes);

    auto reopened = open_registry(directory.path());
    AR_CHECK(reopened.report.action == RecoveryAction::RolledBackToLastValid ||
             reopened.report.action == RecoveryAction::DamagedGenerationQuarantined);
    AR_CHECK(reopened.registry->close().has_value());
}

AR_TEST(recovery, recovery_disabled_reports_the_failure_and_leaves_the_store_untouched) {
    TempDirectory directory("durable-no-recovery");
    auto context = open_registry(directory.path());
    AR_CHECK(context.registry->register_asset(context.session, context.session.advance(),
                                              make_request("strict-asset", AssetClass::Server, "Acme", "STR-1"))
                 .has_value());
    AR_CHECK(context.registry->close().has_value());

    const std::vector<std::string> names = list_directory(directory.path() / "generations");
    const std::filesystem::path newest = directory.path() / "generations" / names.back();
    const std::string before = read_text_file(newest);
    write_text_file(newest, before.substr(0, before.size() / 3));

    StoreOpenOptions options;
    options.allow_recovery = false;
    RecoveryReport report;
    auto refused = Store::open(directory.path(), StoreOpenMode::CreateIfMissing, options, report);
    AR_CHECK(!refused);
    if (!refused) {
        AR_CHECK(is_storage_error(refused.error().code()));
    }
    // The store was not rewritten by the refused open.
    AR_CHECK(read_text_file(newest).size() == before.size() / 3);
}

AR_TEST(recovery, an_empty_directory_is_initialised_and_reports_it) {
    TempDirectory directory("durable-fresh");
    RecoveryReport report;
    StoreOpenOptions options;
    auto opened = Store::open(directory.path(), StoreOpenMode::CreateIfMissing, options, report);
    AR_REQUIRE_OK(store, opened);
    AR_CHECK(report.action == RecoveryAction::Initialised);
    AR_CHECK(report.store_modified);
    AR_CHECK(report.epoch.value() == 1);
    AR_CHECK(path_exists(directory.file("meta")));
    AR_CHECK(path_exists(directory.file("CURRENT")));
    AR_CHECK(store->close().has_value());

    // A second open of the same empty store is no longer an initialisation.
    RecoveryReport second_report;
    auto again = Store::open(directory.path(), StoreOpenMode::CreateIfMissing, options, second_report);
    AR_REQUIRE_OK(second, again);
    AR_CHECK(second_report.epoch.value() == 2);
    AR_CHECK(second_report.action == RecoveryAction::None);
    AR_CHECK(second->close().has_value());
}

AR_TEST(recovery, a_directory_that_is_not_a_store_is_not_mistaken_for_one) {
    TempDirectory directory("durable-not-a-store");
    write_text_file(directory.file("random.txt"), "not a store");
    RecoveryReport report;
    auto opened = Store::open(directory.path(), StoreOpenMode::ReadOnly, StoreOpenOptions{}, report);
    AR_REQUIRE_ERROR(error, opened, ErrorCode::StoreNotFound);

    // A read-only open also refuses to create one.
    auto created = Store::open(directory.path(), StoreOpenMode::ReadOnly, StoreOpenOptions{}, report);
    AR_CHECK(!created);
    auto missing = Store::open(directory.path() / "does-not-exist", StoreOpenMode::ReadOnly, StoreOpenOptions{},
                              report);
    AR_REQUIRE_ERROR(missing_error, missing, ErrorCode::StoreNotFound);
}

AR_TEST(recovery, a_hand_edited_current_pointer_is_caught_by_chain_verification) {
    TempDirectory directory("durable-pointer");
    auto context = open_registry(directory.path());
    for (int index = 0; index < 4; ++index) {
        AR_CHECK(context.registry->register_asset(
            context.session, context.session.advance(),
            make_request("chain-" + std::to_string(index), AssetClass::Server, "Acme",
                         "CHAIN-" + std::to_string(index)))
                      .has_value());
    }
    AR_CHECK(context.registry->close().has_value());

    // Point CURRENT at the oldest retained generation, simulating an operator who
    // restored an old pointer by hand. Every individual file is intact, so only
    // the recorded ancestry can detect it.
    const std::vector<std::string> names = list_directory(directory.path() / "generations");
    AR_CHECK(names.size() >= 3);
    const std::string oldest = names.front();
    std::string pointer = read_text_file(directory.file("CURRENT"));
    const std::size_t equals = pointer.find("sequence=");
    AR_CHECK(equals != std::string::npos);
    std::string digits;
    for (std::size_t index = equals + 9; index < pointer.size(); ++index) {
        if (pointer[index] < '0' || pointer[index] > '9') {
            break;
        }
        digits.push_back(pointer[index]);
    }
    // The generation file name carries the sequence with fixed width.
    const std::string target = oldest.substr(4, 20);
    std::size_t first_nonzero = target.find_first_not_of('0');
    if (first_nonzero == std::string::npos) {
        first_nonzero = target.size() - 1;
    }
    write_text_file(directory.file("CURRENT"),
                    "asset-registry-current/1\nsequence=" + target.substr(first_nonzero) + "\n");

    StoreOpenOptions options;
    options.deep_verify_chain = true;
    RecoveryReport report;
    auto refused = Store::open(directory.path(), StoreOpenMode::CreateIfMissing, options, report);
    AR_CHECK(!refused);
    if (!refused) {
        AR_CHECK(refused.error().code() == ErrorCode::StoreIntegrityFailed ||
                 refused.error().code() == ErrorCode::StoreCorrupt);
        AR_CHECK(refused.error().message().find("ancestry") != std::string::npos ||
                 refused.error().message().find("predecessor") != std::string::npos);
    }
}

AR_TEST(durability, staging_files_from_an_interrupted_commit_are_cleaned_up) {
    TempDirectory directory("durable-orphans");
    auto context = open_registry(directory.path());
    AR_CHECK(context.registry->register_asset(context.session, context.session.advance(),
                                              make_request("orphan-asset", AssetClass::Server, "Acme", "ORP-1"))
                 .has_value());
    AR_CHECK(context.registry->close().has_value());

    // Simulate an interrupted commit by leaving a staging file behind.
    const std::filesystem::path staged = directory.path() / "tmp" / "gen-00000000000000000099.dat.stage.1";
    write_text_file(staged, "partial write that never became a generation");
    AR_CHECK(path_exists(staged));

    auto reopened = open_registry(directory.path());
    AR_CHECK(list_directory(directory.path() / "tmp").empty());
    AR_CHECK(reopened.report.detail.find("staging file") != std::string::npos);
    AR_CHECK(reopened.registry->close().has_value());
}

AR_TEST(durability, retained_generations_are_bounded_and_compaction_preserves_state) {
    TempDirectory directory("durable-compaction");
    RegistryLimits bounds = default_limits();
    bounds.max_retained_generations = 4;
    auto context = open_registry(directory.path(), default_policy(), bounds);
    for (int index = 0; index < 20; ++index) {
        AR_CHECK(context.registry->register_asset(
            context.session, context.session.advance(),
            make_request("compact-" + std::to_string(index), AssetClass::Server, "Acme",
                         "CMP-" + std::to_string(index)))
                      .has_value());
    }
    AR_CHECK(context.registry->store()->retained_generation_count() <= 4);
    AR_CHECK(context.registry->close().has_value());

    auto reopened = open_registry(directory.path(), default_policy(), bounds);
    AR_CHECK(reopened.registry->snapshot().size() == 20);
    AR_CHECK(reopened.registry->published_sequence().value() == 20);
    AR_CHECK(reopened.registry->close().has_value());
}

AR_TEST(durability, writer_sequence_survives_restart_so_retries_replay) {
    TempDirectory directory("durable-retry");
    const AssetId identifier = asset_id_for("retry-asset");
    const std::string key = "restart-retry-key";
    {
        auto context = open_registry(directory.path());
        MutationEnvelope envelope = context.session.advance(key, "first");
        MetadataPatch patch;
        patch.owner = owner("org.example.platform");
        auto applied = context.registry->register_asset(
            context.session, envelope, make_request("retry-asset", AssetClass::Server, "Acme", "RETRY-1"));
        AR_REQUIRE_OK(created, applied);
        AR_CHECK(created.id == identifier);
        (void)0;
        AR_CHECK(context.registry->close().has_value());
    }
    {
        // A new process would mint a new session; the CLI does exactly this. The
        // recorded high-water mark tells the writer where to continue, so the same
        // key and sequence replay rather than applying twice.
        auto context = open_registry(directory.path());
        AR_CHECK(context.session.next_sequence() == MutationSequence(2));
        RegisterAssetRequest request = make_request("retry-asset", AssetClass::Server, "Acme", "RETRY-1");
        MutationEnvelope replay_envelope;
        replay_envelope.sequence = MutationSequence(1);
        replay_envelope.idempotency_key = key;
        auto replayed = context.registry->register_asset(context.session, replay_envelope, request);
        AR_REQUIRE_OK(replay_result, replayed);
        AR_CHECK(replay_result.idempotent_replay);
        AR_CHECK(replay_result.id == identifier);
        AR_CHECK(context.registry->snapshot().size() == 1);
        AR_CHECK(context.registry->close().has_value());
    }
}

AR_TEST(durability, a_failed_commit_leaves_the_published_state_unchanged) {
    TempDirectory directory("durable-failure");
    auto context = open_registry(directory.path());
    AR_CHECK(context.registry->register_asset(context.session, context.session.advance(),
                                              make_request("before-failure", AssetClass::Server, "Acme", "BF-1"))
                 .has_value());
    const Snapshot before = context.registry->snapshot();
    const TransactionSequence sequence_before = before.published_sequence();
    AR_CHECK(context.registry->close().has_value());

    // Reopening read-only cannot commit; the mutation is refused and nothing is
    // published.
    RecoveryReport report;
    auto reader = AssetRegistry::open(directory.path(), StoreOpenMode::ReadOnly, StoreOpenOptions{}, report);
    AR_REQUIRE_OK(read_only, reader);
    auto session = read_only->open_writer(writer("read-only-writer"));
    AR_REQUIRE_ERROR(grant_error, session, ErrorCode::AuthorityRevoked);
    AR_CHECK(read_only->snapshot().published_sequence() == sequence_before);
    AR_CHECK(read_only->close().has_value());
}
