// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Export and import tests: canonical export determinism, import validation,
// conflict policies, and round-trip fidelity.

#include <set>
#include <string>
#include <vector>

#include "test_harness.hpp"
#include "test_support.hpp"

using namespace asset_registry;
using namespace asset_test;

namespace {

/// Builds a small inventory with references, labels, lineage, and provenance.
void seed_rich_inventory(AssetRegistry& registry, WriterSession& session) {
    RegisterAssetRequest first = make_request("export-original", AssetClass::Server, "Acme", "EXP-1");
    first.metadata.owner = owner("org.example.platform");
    first.metadata.site = location("site-a.hall-2");
    first.metadata.notes = "notes with \"quotes\" and \\ backslash";
    first.metadata.labels = {{"tier", "gold"}, {"rack.unit", "7"}};
    first.references = {capability_ref("asi:accelerator.scheduling", ReferenceEvidence::Verified),
                        Reference::location(location("site-a.hall-2.room-4"), ReferenceEvidence::Verified)};
    auto registered = registry.register_asset(session, session.advance(), first);
    AR_CHECK(registered.has_value());
    if (!registered.has_value()) {
        return;
    }
    MetadataPatch patch;
    // A second revision, so the record carries provenance beyond its registration. The
    // notes are deliberately not patched: the export case asserts that the quotes and the
    // backslash in them are escaped, and a patch would replace the value under test.
    patch.site = location("site-a.hall-2.room-4");
    AR_CHECK(registry.update_metadata(session, session.advance(), registered.value().id, std::nullopt, patch)
                 .has_value());
    AR_CHECK(registry.transition_installation(session, session.advance(), registered.value().id, std::nullopt,
                                              InstallationState::NotInstalled)
                 .has_value());
    AR_CHECK(registry.transition_installation(session, session.advance(), registered.value().id, std::nullopt,
                                              InstallationState::Staged)
                 .has_value());
    AR_CHECK(registry.transition_installation(session, session.advance(), registered.value().id, std::nullopt,
                                              InstallationState::Installed)
                 .has_value());
    AR_CHECK(registry.transition_lifecycle(session, session.advance(), registered.value().id, std::nullopt,
                                           LifecycleState::Provisioned)
                 .has_value());
    AR_CHECK(registry.transition_lifecycle(session, session.advance(), registered.value().id, std::nullopt,
                                           LifecycleState::Active)
                 .has_value());

    RegisterAssetRequest second = make_request("export-retired", AssetClass::Switch, "Acme", "EXP-2");
    second.metadata.display_name = "switch with unicode \xE6\x9C\xBA\xE6\x88\xBF";
    second.references = {Reference::rack(rack("site-a.hall-2.row-1.rack-3"), ReferenceEvidence::Unverified)};
    AR_CHECK(registry.register_asset(session, session.advance(), second).has_value());
    AR_CHECK(registry.transition_lifecycle(session, session.advance(), second.id, std::nullopt,
                                           LifecycleState::Decommissioned)
                 .has_value());
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

AR_TEST(export_document, canonical_export_is_byte_identical_across_runs) {
    auto context = detached_registry();
    seed_rich_inventory(*context.registry, context.session);

    ExportOptions options;
    ExportReport first_report;
    auto first = context.registry->export_document(options, first_report);
    AR_REQUIRE_OK(primary, first);
    ExportReport second_report;
    auto second = context.registry->export_document(options, second_report);
    AR_REQUIRE_OK(repeat, second);
    AR_CHECK(primary == repeat);
    AR_CHECK(first_report.records_exported == 2);
    AR_CHECK(first_report.bytes_written == primary.size());
    AR_CHECK(first_report.published_sequence == context.registry->published_sequence());
    AR_CHECK(first_report.schema_version == kExportSchemaVersion);

    // The document names its schema and carries the fixed member order.
    AR_CHECK(contains(primary, "\"document\": \"asset-registry-export\""));
    AR_CHECK(contains(primary, "\"schema_version\": 1"));
    AR_CHECK(contains(primary, "\"assets\": ["));
    AR_CHECK(contains(primary, "\"serial_key\":"));
    AR_CHECK(contains(primary, "\"provenance\": ["));
    AR_CHECK(contains(primary, "\"history\": ["));
    // Escaping is deterministic and correct.
    AR_CHECK(contains(primary, "notes with \\\"quotes\\\" and \\\\ backslash"));
    // Unicode survives verbatim rather than being escaped into a different form.
    AR_CHECK(contains(primary, "\xE6\x9C\xBA\xE6\x88\xBF"));
    AR_CHECK(primary.back() == '\n');

    // Indentation is a documented option, and zero produces a single line per
    // record rather than changing the content.
    ExportOptions compact = options;
    compact.indent = 0;
    ExportReport compact_report;
    auto compact_document = context.registry->export_document(compact, compact_report);
    AR_REQUIRE_OK(flat, compact_document);
    AR_CHECK(flat.size() < primary.size());
    AR_CHECK(compact_report.records_exported == 2);
}

AR_TEST(export_document, records_appear_in_canonical_identity_order) {
    auto context = detached_registry();
    std::vector<AssetId> identifiers;
    for (int index = 0; index < 16; ++index) {
        auto registered = context.registry->register_asset(
            context.session, context.session.advance(),
            make_request("ordered-" + std::to_string(index), AssetClass::Server, "Acme",
                         "ORD-" + std::to_string(index)));
        AR_CHECK(registered.has_value());
        if (registered.has_value()) {
            identifiers.push_back(registered.value().id);
        }
    }
    std::sort(identifiers.begin(), identifiers.end());
    ExportOptions options;
    options.indent = 0;
    ExportReport report;
    auto document = context.registry->export_document(options, report);
    AR_REQUIRE_OK(text, document);
    std::size_t cursor = 0;
    for (const AssetId& identifier : identifiers) {
        const std::size_t found = text.find("\"asset_id\":\"" + identifier.to_string() + "\"", cursor);
        AR_CHECK_MSG(found != std::string::npos, identifier.to_string());
        if (found == std::string::npos) {
            break;
        }
        cursor = found;
    }
}

AR_TEST(export_document, unsupported_schema_version_is_refused) {
    auto context = detached_registry();
    ExportOptions options;
    options.schema_version = 99;
    ExportReport report;
    auto refused = context.registry->export_document(options, report);
    AR_REQUIRE_ERROR(error, refused, ErrorCode::ExportFormatUnsupported);
    AR_CHECK(contains(error.message(), "99"));
}

AR_TEST(export_document, newline_delimited_format_emits_one_record_per_line) {
    auto context = detached_registry();
    for (int index = 0; index < 3; ++index) {
        AR_CHECK(context.registry
                     ->register_asset(context.session, context.session.advance(),
                                      make_request("ndjson-" + std::to_string(index), AssetClass::Server, "Acme",
                                                   "ND-" + std::to_string(index)))
                     .has_value());
    }
    ExportOptions options;
    options.format = ExportFormat::NewlineDelimitedJson;
    ExportReport report;
    auto document = context.registry->export_document(options, report);
    AR_REQUIRE_OK(text, document);
    std::size_t lines = 0;
    for (const char character : text) {
        if (character == '\n') {
            ++lines;
        }
    }
    AR_CHECK(lines == 3);
    AR_CHECK(report.records_exported == 3);
}

AR_TEST(import_document, export_round_trips_through_a_second_registry) {
    auto source = detached_registry();
    seed_rich_inventory(*source.registry, source.session);
    ExportOptions options;
    ExportReport report;
    auto document = source.registry->export_document(options, report);
    AR_REQUIRE_OK(text, document);
    const Snapshot original = source.registry->snapshot();

    auto target = detached_registry();
    auto parsed = target.registry->parse_import_document(text, ImportOptions{});
    AR_REQUIRE_OK(records, parsed);
    AR_CHECK(records.size() == 2);

    ImportOptions import_options;
    import_options.source = "round-trip-test";
    auto imported = target.registry->import_assets(target.session, target.session.advance(), records,
                                                   import_options);
    AR_REQUIRE_OK(import_report, imported);
    AR_CHECK(import_report.committed);
    AR_CHECK(import_report.records_registered == 2);
    AR_CHECK(import_report.records_rejected() == 0);

    const Snapshot round_tripped = target.registry->snapshot();
    AR_CHECK(round_tripped.size() == original.size());
    for (const AssetId& identifier : original.ids()) {
        const auto before = original.find(identifier);
        const auto after = round_tripped.find(identifier);
        AR_REQUIRE_PRESENT(before_view, before);
        AR_REQUIRE_PRESENT(after_view, after);
        AR_CHECK(before_view.asset_class == after_view.asset_class);
        AR_CHECK(before_view.generation == after_view.generation);
        AR_CHECK(before_view.serial_identity == after_view.serial_identity);
        AR_CHECK(before_view.metadata == after_view.metadata);
        AR_CHECK(before_view.references == after_view.references);
        AR_CHECK(before_view.state == after_view.state);
        AR_CHECK(before_view.supersedes.has_value() == after_view.supersedes.has_value());
        // Provenance is rebuilt with the import as its origin, which is the honest
        // record of what actually happened.
        AR_CHECK(after_view.provenance->size() == 1);
        AR_CHECK(after_view.provenance->front().origin == ProvenanceOrigin::Import);
    }
}

AR_TEST(import_document, unknown_and_missing_members_are_rejected) {
    auto context = detached_registry();
    const RegistryLimits bounds = default_limits();

    const char* unknown_member = R"({"document":"asset-registry-export","schema_version":1,
        "assets":[{"asset_id":"00000000-0000-4000-8000-000000000001","asset_class":"server",
        "serial_identity":{"manufacturer":"Acme","serial":"S-1"},"metadata":{"display_name":"x"},
        "state":{"lifecycle":"planned","installation":"unknown"},"surprise":true}]})";
    auto refused = parse_import_json(unknown_member, bounds);
    AR_REQUIRE_ERROR(error, refused, ErrorCode::ImportRecordInvalid);
    AR_CHECK(contains(error.message(), "unknown member"));

    const char* missing_display_name = R"({"document":"asset-registry-export","schema_version":1,
        "assets":[{"asset_id":"00000000-0000-4000-8000-000000000001","asset_class":"server",
        "serial_identity":{"manufacturer":"Acme","serial":"S-1"},"metadata":{},
        "state":{"lifecycle":"planned","installation":"unknown"}}]})";
    auto missing = parse_import_json(missing_display_name, bounds);
    AR_REQUIRE_ERROR(missing_error, missing, ErrorCode::ImportRecordInvalid);
    AR_CHECK(contains(missing_error.message(), "display_name"));

    const char* bad_version = R"({"document":"asset-registry-export","schema_version":7,"assets":[]})";
    auto version = parse_import_json(bad_version, bounds);
    AR_REQUIRE_ERROR(version_error, version, ErrorCode::ImportFormatUnsupported);

    const char* missing_assets = R"({"document":"asset-registry-export","schema_version":1})";
    auto no_assets = parse_import_json(missing_assets, bounds);
    AR_REQUIRE_ERROR(assets_error, no_assets, ErrorCode::ImportRecordInvalid);

    const char* bad_class = R"({"document":"asset-registry-export","schema_version":1,
        "assets":[{"asset_id":"00000000-0000-4000-8000-000000000001","asset_class":"quantum_flux",
        "serial_identity":{"manufacturer":"Acme","serial":"S-1"},"metadata":{"display_name":"x"},
        "state":{"lifecycle":"planned","installation":"unknown"}}]})";
    auto class_error_result = parse_import_json(bad_class, bounds);
    AR_REQUIRE_ERROR(class_error, class_error_result, ErrorCode::ImportRecordInvalid);
    AR_CHECK(contains(class_error.message(), "asset_class"));

    const char* bad_state = R"({"document":"asset-registry-export","schema_version":1,
        "assets":[{"asset_id":"00000000-0000-4000-8000-000000000001","asset_class":"server",
        "serial_identity":{"manufacturer":"Acme","serial":"S-1"},"metadata":{"display_name":"x"},
        "state":{"lifecycle":"flourishing","installation":"unknown"}}]})";
    auto state_result = parse_import_json(bad_state, bounds);
    AR_REQUIRE_ERROR(state_error, state_result, ErrorCode::ImportRecordInvalid);
    AR_CHECK(contains(state_error.message(), "lifecycle"));
}

AR_TEST(import_document, import_is_atomic_and_reports_per_record_rejections) {
    auto context = detached_registry();
    std::vector<ImportedAsset> records;

    ImportedAsset good;
    good.id = asset_id_for("import-good");
    good.asset_class = AssetClass::Server;
    good.serial_identity = serial("Acme", "IMP-1");
    good.metadata.display_name = "import-good";
    good.state.lifecycle = LifecycleState::Planned;
    good.state.installation = InstallationState::Unknown;
    records.push_back(good);

    ImportedAsset inconsistent = good;
    inconsistent.id = asset_id_for("import-inconsistent");
    inconsistent.serial_identity = serial("Acme", "IMP-2");
    inconsistent.state.lifecycle = LifecycleState::Active;
    inconsistent.state.installation = InstallationState::Unknown;
    records.push_back(inconsistent);

    ImportedAsset unknown_class = good;
    unknown_class.id = asset_id_for("import-unknown-class");
    unknown_class.serial_identity = serial("Acme", "IMP-3");
    unknown_class.asset_class = AssetClass::Unknown;
    records.push_back(unknown_class);

    ImportOptions options;
    options.on_error = ImportErrorPolicy::RejectRecord;
    auto report = context.registry->import_assets(context.session, context.session.advance(), records, options);
    AR_REQUIRE_OK(result, report);
    AR_CHECK(result.committed);
    AR_CHECK(result.records_seen == 3);
    AR_CHECK(result.records_registered == 1);
    AR_CHECK(result.records_rejected() == 2);
    AR_CHECK(result.rejections[0].record_index == 1);
    AR_CHECK(result.rejections[0].code == ErrorCode::LifecycleInvariantViolation);
    AR_CHECK(result.rejections[1].record_index == 2);
    AR_CHECK(result.rejections[1].code == ErrorCode::UnknownAssetClass);
    AR_CHECK(result.rejections[1].claimed_id == asset_id_for("import-unknown-class").to_string());
    AR_CHECK(context.registry->snapshot().size() == 1);
}

AR_TEST(import_document, reject_batch_publishes_nothing) {
    auto context = detached_registry();
    ImportedAsset good;
    good.id = asset_id_for("batch-good");
    good.asset_class = AssetClass::Server;
    good.serial_identity = serial("Acme", "BATCH-1");
    good.metadata.display_name = "batch-good";
    good.state.lifecycle = LifecycleState::Planned;
    good.state.installation = InstallationState::Unknown;

    ImportedAsset bad = good;
    bad.id = asset_id_for("batch-bad");
    bad.serial_identity = serial("Acme", "BATCH-2");
    bad.asset_class = AssetClass::Unknown;

    ImportOptions options;
    options.on_error = ImportErrorPolicy::RejectBatch;
    auto report = context.registry->import_assets(context.session, context.session.advance(), {good, bad}, options);
    AR_REQUIRE_ERROR(error, report, ErrorCode::ImportRecordInvalid);
    AR_CHECK(context.registry->snapshot().empty());
    AR_CHECK(context.registry->published_sequence().value() == 0);
}

AR_TEST(import_document, conflict_policies_behave_as_documented) {
    auto context = detached_registry();
    auto& registry = *context.registry;
    ImportedAsset record;
    record.id = asset_id_for("conflict-asset");
    record.asset_class = AssetClass::Server;
    record.serial_identity = serial("Acme", "CONF-1");
    record.metadata.display_name = "conflict-asset";
    record.state.lifecycle = LifecycleState::Planned;
    record.state.installation = InstallationState::Unknown;

    ImportOptions reject;
    reject.conflict = ImportConflictPolicy::Reject;
    auto first = registry.import_assets(context.session, context.session.advance(), {record}, reject);
    AR_REQUIRE_OK(first_result, first);
    AR_CHECK(first_result.records_registered == 1);

    auto second = registry.import_assets(context.session, context.session.advance(), {record}, reject);
    AR_REQUIRE_OK(second_result, second);
    AR_CHECK(second_result.committed == false);
    AR_CHECK(second_result.records_rejected() == 1);
    AR_CHECK(second_result.rejections.front().code == ErrorCode::DuplicateAssetId);

    ImportOptions skip;
    skip.conflict = ImportConflictPolicy::SkipExisting;
    auto third = registry.import_assets(context.session, context.session.advance(), {record}, skip);
    AR_REQUIRE_OK(third_result, third);
    AR_CHECK(third_result.records_skipped == 1);
    AR_CHECK(third_result.records_registered == 0);

    ImportedAsset changed = record;
    changed.metadata.notes = "changed by import";
    ImportOptions update;
    update.conflict = ImportConflictPolicy::UpdateExisting;
    auto fourth = registry.import_assets(context.session, context.session.advance(), {changed}, update);
    AR_REQUIRE_OK(fourth_result, fourth);
    AR_CHECK(fourth_result.committed);
    AR_CHECK(fourth_result.records_updated == 1);
    const auto view = registry.snapshot().find(record.id);
    AR_CHECK(view.has_value());
    AR_CHECK(view->metadata.notes == "changed by import");
    AR_CHECK(view->revision == AssetRevision(2));
    AR_CHECK(view->provenance->back().origin == ProvenanceOrigin::Import);
}

AR_TEST(import_document, duplicate_serials_and_self_supersession_within_a_batch_are_rejected) {
    auto context = detached_registry();
    ImportedAsset first;
    first.id = asset_id_for("batch-serial-a");
    first.asset_class = AssetClass::Server;
    first.serial_identity = serial("Acme", "BATCHDUP-1");
    first.metadata.display_name = "batch-serial-a";
    first.state.lifecycle = LifecycleState::Planned;
    first.state.installation = InstallationState::Unknown;

    ImportedAsset second = first;
    second.id = asset_id_for("batch-serial-b");
    second.metadata.display_name = "batch-serial-b";

    ImportOptions options;
    auto report = context.registry->import_assets(context.session, context.session.advance(), {first, second},
                                                  options);
    AR_REQUIRE_OK(result, report);
    AR_CHECK(result.records_registered == 1);
    AR_CHECK(result.records_rejected() == 1);
    AR_CHECK(result.rejections.front().code == ErrorCode::DuplicateSerialIdentity);

    auto other = detached_registry();
    ImportedAsset self = first;
    self.supersedes = self.id;
    auto self_report = other.registry->import_assets(other.session, other.session.advance(), {self}, options);
    AR_REQUIRE_OK(self_result, self_report);
    AR_CHECK(self_result.records_rejected() == 1);
    AR_CHECK(self_result.rejections.front().code == ErrorCode::SelfReplacementForbidden);
}

AR_TEST(import_document, unverified_references_can_be_dropped_and_the_drop_is_reported) {
    auto context = detached_registry();
    ImportedAsset record;
    record.id = asset_id_for("unverified-asset");
    record.asset_class = AssetClass::Server;
    record.serial_identity = serial("Acme", "UNV-1");
    record.metadata.display_name = "unverified-asset";
    record.state.lifecycle = LifecycleState::Planned;
    record.state.installation = InstallationState::Unknown;
    // The references are stored in ascending canonical order, which is what makes the
    // record's serialised form stable, so the canonically first reference is the one at
    // the front of the stored vector. It is the unverified one, so the case can state that
    // an unverified reference is retained rather than dropped.
    record.references = {capability_ref("asi:a", ReferenceEvidence::Unverified),
                         capability_ref("asi:b", ReferenceEvidence::Verified),
                         Reference::location(location("site-a"), ReferenceEvidence::Unverified)};

    ImportOptions keep;
    auto kept = context.registry->import_assets(context.session, context.session.advance(), {record}, keep);
    AR_REQUIRE_OK(kept_result, kept);
    AR_CHECK(kept_result.references_dropped == 0);
    const auto view = context.registry->snapshot().find(record.id);
    AR_CHECK(view.has_value());
    AR_CHECK(view->references.size() == 3);
    AR_CHECK(view->references.front().evidence() == ReferenceEvidence::Unverified);

    auto drop_context = detached_registry();
    ImportOptions drop;
    drop.drop_unverified_references = true;
    auto dropped = drop_context.registry->import_assets(drop_context.session, drop_context.session.advance(),
                                                       {record}, drop);
    AR_REQUIRE_OK(dropped_result, dropped);
    AR_CHECK(dropped_result.references_dropped == 2);
    const auto dropped_view = drop_context.registry->snapshot().find(record.id);
    AR_CHECK(dropped_view.has_value());
    AR_CHECK(dropped_view->references.size() == 1);
    AR_CHECK(dropped_view->references.front().evidence() == ReferenceEvidence::Verified);
}

AR_TEST(import_document, single_asset_document_uses_the_same_rules) {
    const RegistryLimits bounds = default_limits();
    const char* object = R"({"asset_id":"00000000-0000-4000-8000-00000000000a","asset_class":"server",
        "serial_identity":{"manufacturer":"Acme","serial":"ONE-1"},
        "metadata":{"display_name":"one","owner":null,"site":null,"notes":"","labels":[]},
        "state":{"lifecycle":"planned","installation":"unknown"},"references":[],"supersedes":null})";
    auto parsed = parse_import_asset_json(object, bounds);
    AR_REQUIRE_OK(record, parsed);
    AR_CHECK(record.asset_class == AssetClass::Server);
    AR_CHECK(record.metadata.display_name == "one");
    AR_CHECK(!record.metadata.owner.has_value());

    auto malformed = parse_import_asset_json("{\"asset_class\":\"server\"}", bounds);
    AR_CHECK(!malformed);

    auto not_object = parse_import_asset_json("[1,2,3]", bounds);
    AR_CHECK(!not_object);
}

AR_TEST(import_document, import_is_idempotent_under_a_replayed_key) {
    auto context = detached_registry();
    ImportedAsset record;
    record.id = asset_id_for("replay-import");
    record.asset_class = AssetClass::Server;
    record.serial_identity = serial("Acme", "REPLAY-1");
    record.metadata.display_name = "replay-import";
    record.state.lifecycle = LifecycleState::Planned;
    record.state.installation = InstallationState::Unknown;

    MutationEnvelope envelope = context.session.advance("import-key-1", "batch");
    auto first = context.registry->import_assets(context.session, envelope, {record}, ImportOptions{});
    AR_REQUIRE_OK(applied, first);
    AR_CHECK(applied.committed);
    const TransactionSequence sequence = context.registry->published_sequence();

    auto replayed = context.registry->import_assets(context.session, envelope, {record}, ImportOptions{});
    AR_REQUIRE_OK(replay_result, replayed);
    AR_CHECK(replay_result.committed);
    AR_CHECK(replay_result.committed_sequence == sequence);
    AR_CHECK(context.registry->snapshot().size() == 1);
    AR_CHECK(context.registry->published_sequence() == sequence);
}

AR_TEST(accounting, a_mixed_batch_closes_its_books) {
    // Every outcome a batch can produce, in one call: a registration, an update, a skip,
    // and a rejection. The report must account for each record exactly once, and the
    // inventory and the export must agree with what the calls reported.
    auto context = detached_registry();
    auto& registry = *context.registry;

    ImportedAsset kept;
    kept.id = asset_id_for("accounting-kept");
    kept.asset_class = AssetClass::Server;
    kept.serial_identity = serial("Acme", "ACC-1");
    kept.metadata.display_name = "accounting-kept";
    kept.state.lifecycle = LifecycleState::Planned;
    kept.state.installation = InstallationState::Unknown;

    ImportedAsset same = kept;
    same.id = asset_id_for("accounting-same");
    same.serial_identity = serial("Acme", "ACC-2");
    same.metadata.display_name = "accounting-same";

    auto seeded = registry.import_assets(context.session, context.session.advance(), {kept, same}, ImportOptions{});
    AR_REQUIRE_OK(seed_report, seeded);
    AR_CHECK(seed_report.records_seen == 2);
    AR_CHECK(seed_report.records_registered == 2);

    ImportedAsset changed = kept;
    changed.metadata.notes = "changed by import";

    ImportedAsset unchanged = same;

    ImportedAsset distinct = kept;
    distinct.id = asset_id_for("accounting-distinct");
    distinct.serial_identity = serial("Acme", "ACC-3");
    distinct.metadata.display_name = "accounting-distinct";

    ImportedAsset invalid = kept;
    invalid.id = asset_id_for("accounting-invalid");
    invalid.serial_identity = serial("Acme", "ACC-4");
    invalid.asset_class = AssetClass::Unknown;

    ImportOptions options;
    options.conflict = ImportConflictPolicy::UpdateExisting;
    auto imported = registry.import_assets(context.session, context.session.advance(),
                                           {changed, unchanged, distinct, invalid}, options);
    AR_REQUIRE_OK(result, imported);
    AR_CHECK(result.committed);
    AR_CHECK(result.records_seen == 4);
    AR_CHECK(result.records_updated == 1);
    AR_CHECK(result.records_skipped == 1);
    AR_CHECK(result.records_registered == 1);
    AR_CHECK(result.records_rejected() == 1);
    AR_CHECK(result.rejections.front().code == ErrorCode::UnknownAssetClass);
    // The books close: nothing the batch saw is unaccounted for, and nothing is counted
    // twice.
    AR_CHECK(result.records_seen == result.records_registered + result.records_updated + result.records_skipped +
                                        result.records_rejected());

    // One batch applies each identity once. A batch that names the same asset twice is
    // refused per record rather than applying one record twice in a single transaction,
    // which would give two provenance steps one sequence.
    ImportedAsset changed_again = kept;
    changed_again.metadata.notes = "changed again by import";
    auto repeated =
        registry.import_assets(context.session, context.session.advance(), {changed_again, changed_again}, options);
    AR_REQUIRE_OK(repeat_report, repeated);
    AR_CHECK(repeat_report.records_seen == 2);
    AR_CHECK(repeat_report.records_updated == 1);
    AR_CHECK(repeat_report.records_rejected() == 1);
    AR_CHECK(repeat_report.rejections.front().record_index == 1);
    AR_CHECK(repeat_report.rejections.front().code == ErrorCode::DuplicateAssetId);
    AR_CHECK(repeat_report.records_seen == repeat_report.records_registered + repeat_report.records_updated +
                                               repeat_report.records_skipped + repeat_report.records_rejected());

    // The inventory agrees with the records the calls reported producing.
    const Snapshot snapshot = registry.snapshot();
    AR_CHECK(snapshot.size() == 3);
    const InventorySummary summary = snapshot.summary();
    AR_CHECK(summary.total_assets == static_cast<std::uint64_t>(snapshot.size()));
    std::uint64_t by_class = 0;
    for (const auto& entry : summary.by_class) {
        by_class += entry.second;
    }
    AR_CHECK(by_class == summary.total_assets);
    std::uint64_t by_lifecycle = 0;
    for (const auto& entry : summary.by_lifecycle) {
        by_lifecycle += entry.second;
    }
    AR_CHECK(by_lifecycle == summary.total_assets);

    // The export accounts for exactly the live inventory, byte for byte.
    ExportReport export_report;
    auto document = registry.export_document(ExportOptions{}, export_report);
    AR_REQUIRE_OK(text, document);
    AR_CHECK(export_report.records_exported == summary.total_assets);
    AR_CHECK(export_report.bytes_written == text.size());
    AR_CHECK(export_report.published_sequence == registry.published_sequence());
}
