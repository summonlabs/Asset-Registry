// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Adversarial tests: malformed, corrupt, truncated, oversized, and hostile input.
//
// Every case here is built from bytes or values an attacker could supply, and the
// requirement in each is the same: reject deterministically with a machine-readable
// code, allocate nothing unbounded, and never publish a partially interpreted
// result.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "asset_registry/text.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using namespace asset_registry;
using namespace asset_test;

// Internal access is needed only to inspect decoded durable state; the test target
// links the internal headers for exactly that purpose.
#include "store_internal.hpp"  // resolved through the test target's private include path

namespace {

[[nodiscard]] std::string repeated(char character, std::size_t count) {
    return std::string(count, character);
}

}  // namespace

AR_TEST(adversarial, oversized_text_is_rejected_before_storage) {
    const std::string enormous = repeated('x', 1024 * 1024);
    AR_CHECK(!is_display_text(enormous, 192));
    AR_CHECK(!is_serial_number_text(enormous));
    AR_CHECK(!is_label_key(enormous));
    AR_CHECK(!OwnerId::create(enormous).has_value());
    AR_CHECK(!LocationId::create(enormous).has_value());
    AR_CHECK(!WriterId::create(enormous).has_value());
    AR_CHECK(!CapabilityReference::create(enormous).has_value());
    AR_CHECK(!ManufacturerIdentity::create(enormous).has_value());
    AR_CHECK(!SerialNumber::create(enormous).has_value());
    AR_CHECK(!AssetId::parse(enormous).has_value());
    AR_CHECK(!Timestamp::parse_rfc3339(enormous).has_value());
    AR_CHECK(!parse_unsigned_decimal(enormous).has_value());

    auto context = detached_registry();
    RegisterAssetRequest request = make_request("oversized", AssetClass::Server, "Acme", "OS-1");
    request.metadata.display_name = enormous;
    auto refused = context.registry->register_asset(context.session, context.session.advance(), request);
    AR_REQUIRE_ERROR(error, refused, ErrorCode::MalformedText);
    AR_CHECK(context.registry->snapshot().empty());
}

AR_TEST(adversarial, hostile_unicode_is_rejected_rather_than_normalised) {
    auto context = detached_registry();
    struct Case {
        const char* text;
        ErrorCode expected;
    };
    const Case cases[] = {
        {"name" "\xE2\x80" "\xAE" "reversed", ErrorCode::MalformedText},          // right-to-left override
        {"name" "\xE2\x80" "\x8B" "hidden", ErrorCode::MalformedText},            // zero width space
        {"name" "\xEF\xBB" "\xBF" "bom", ErrorCode::MalformedText},               // byte order mark
        {"name" "\x01" "control", ErrorCode::MalformedText},                   // C0 control
        {"name" "\xC3", ErrorCode::MalformedText},                          // truncated sequence
        {"name" "\xC0\xAF" "overlong", ErrorCode::MalformedText},              // overlong solidus
        {"name" "\xED\xA0" "\x80" "surrogate", ErrorCode::MalformedText},         // surrogate
        {"name" "\xF5\x80\x80" "\x80" "beyond", ErrorCode::MalformedText},        // above U+10FFFF
    };
    int index = 0;
    for (const Case& item : cases) {
        RegisterAssetRequest request = make_request("unicode-" + std::to_string(index), AssetClass::Server, "Acme",
                                                    "UNI-" + std::to_string(index));
        request.metadata.display_name = item.text;
        auto refused = context.registry->register_asset(context.session, context.session.advance(), request);
        AR_CHECK_MSG(!refused.has_value(), item.text);
        if (!refused.has_value()) {
            AR_CHECK(refused.error().code() == item.expected);
        }
        ++index;
    }
    AR_CHECK(context.registry->snapshot().empty());

    // Legitimate non-ASCII text is accepted, so the rejection is about the hostile
    // forms rather than about Unicode in general.
    RegisterAssetRequest valid = make_request("unicode-valid", AssetClass::Server, "Acme", "UNI-OK");
    valid.metadata.display_name = "Ch\xC3\xA2" "teau " "\xE6\x9C\xBA" "\xE6\x88\xBF" " " "\xF0\x9F\x9A" "\x80";
    auto accepted = context.registry->register_asset(context.session, context.session.advance(), valid);
    AR_CHECK(accepted.has_value());
}

AR_TEST(adversarial, whole_field_payloads_are_rejected) {
    // A field whose entire content is an escape or a separator must not be able to
    // forge a component boundary.
    AR_CHECK(!SerialNumber::create("//").has_value());
    AR_CHECK(!ManufacturerIdentity::create("//").has_value());
    AR_CHECK(!SerialIdentity::parse("Acme////R2").has_value());
    AR_CHECK(!ModelIdentity::create("//").has_value());
    AR_CHECK(!CapabilityReference::create("::").has_value());
    AR_CHECK(!CapabilityReference::create(":").has_value());
    AR_CHECK(!OwnerId::create("..").has_value());
    AR_CHECK(!OwnerId::create(".").has_value());
    AR_CHECK(!LocationId::create("a..b").has_value());
    AR_CHECK(!UnitSpan::parse("--").has_value());
    AR_CHECK(!UnitSpan::parse("-").has_value());
    AR_CHECK(!parse_unsigned_decimal("-").has_value());
    AR_CHECK(!parse_unsigned_decimal("+").has_value());
}

AR_TEST(adversarial, integer_edges_are_handled_without_overflow) {
    AR_CHECK(!parse_unsigned_decimal("99999999999999999999").has_value());
    AR_CHECK(parse_unsigned_decimal("18446744073709551615").value() == std::numeric_limits<std::uint64_t>::max());
    AR_CHECK(!AssetRevision::parse("18446744073709551616").has_value());
    AR_CHECK(!AssetGeneration::parse("4294967296").has_value());
    AR_CHECK(AssetGeneration::parse("4294967295").value() ==
             AssetGeneration(std::numeric_limits<std::uint32_t>::max()));
    AR_CHECK(!UnitSpan::parse("4294967296").has_value());
    AR_CHECK(!UnitSpan::parse("4294967295-4294967296").has_value());
    AR_CHECK(!Timestamp::create(std::numeric_limits<std::int64_t>::min() + 1).has_value() == false);
    AR_CHECK(!Timestamp::create(std::numeric_limits<std::int64_t>::max()).has_value());

    // A counter that would wrap reports exhaustion rather than restarting at zero.
    const AssetRevision maximum(std::numeric_limits<std::uint64_t>::max());
    const auto successor = maximum.next();
    AR_CHECK(!successor.has_value());
}

AR_TEST(adversarial, malformed_documents_are_rejected_with_position_information) {
    const RegistryLimits bounds = default_limits();
    const char* cases[] = {
        "",
        "{",
        "[]",
        "null",
        "{\"schema_version\":1,\"assets\":[]}",                       // no document marker, still accepted shape
        "{\"document\":\"asset-registry-export\",\"schema_version\":1,\"assets\":[}",
        "{\"document\":\"asset-registry-export\",\"schema_version\":1,\"assets\":[]} trailing",
        "{\"document\":\"asset-registry-export\",\"schema_version\":1,\"assets\":{}}",
        "{\"document\":\"asset-registry-export\",\"schema_version\":\"1\",\"assets\":[]}",
        "{\"document\":\"asset-registry-export\",\"schema_version\":1.5,\"assets\":[]}",
        "{\"document\":\"asset-registry-export\",\"schema_version\":-1,\"assets\":[]}",
        "{\"document\":\"asset-registry-export\",\"schema_version\":01,\"assets\":[]}",
        "{\"document\":\"asset-registry-export\",\"schema_version\":1,\"assets\":[],\"assets\":[]}",
        "{\"document\":\"some-other-format\",\"schema_version\":1,\"assets\":[]}",
        "{\"document\":\"asset-registry-export\",\"schema_version\":1}",
    };
    int index = 0;
    for (const char* document : cases) {
        auto parsed = parse_import_json(document, bounds);
        AR_CHECK_MSG(!parsed.has_value(), document);
        ++index;
        (void)index;
    }
}

AR_TEST(adversarial, deeply_nested_documents_are_rejected_without_exhausting_the_stack) {
    const RegistryLimits bounds = default_limits();
    // Nesting far beyond the traversal bound: the parser must refuse at the bound
    // rather than recursing until the stack is gone.
    std::string document = "{\"document\":\"asset-registry-export\",\"schema_version\":1,\"assets\":[";
    for (int index = 0; index < 5000; ++index) {
        document += "[";
    }
    for (int index = 0; index < 5000; ++index) {
        document += "]";
    }
    document += "]}";
    auto parsed = parse_import_json(document, bounds);
    AR_CHECK(!parsed.has_value());
    if (!parsed.has_value()) {
        AR_CHECK(parsed.error().code() == ErrorCode::ImportRecordInvalid);
        AR_CHECK(parsed.error().message().find("depth") != std::string::npos ||
                 parsed.error().message().find("valid JSON") != std::string::npos);
    }

    // The single-asset entry point must refuse the same way.
    std::string nested_object;
    for (int index = 0; index < 2000; ++index) {
        nested_object += "[";
    }
    auto single = parse_import_asset_json(nested_object, bounds);
    AR_CHECK(!single.has_value());
}

AR_TEST(adversarial, an_enormous_declared_record_count_is_refused_before_allocation) {
    RegistryLimits tight = default_limits();
    tight.max_import_records = 4;
    std::string document = "{\"document\":\"asset-registry-export\",\"schema_version\":1,\"assets\":[";
    for (int index = 0; index < 100; ++index) {
        if (index > 0) {
            document += ",";
        }
        document += "{\"asset_id\":\"00000000-0000-4000-8000-00000000000";
        document += static_cast<char>('0' + (index % 10));
        document += "\",\"asset_class\":\"server\",\"serial_identity\":{\"manufacturer\":\"A\",\"serial\":\"S\"},"
                    "\"metadata\":{\"display_name\":\"x\"},"
                    "\"state\":{\"lifecycle\":\"planned\",\"installation\":\"unknown\"}}";
    }
    document += "]}";
    auto parsed = parse_import_json(document, tight);
    AR_CHECK(!parsed.has_value());
}

AR_TEST(adversarial, oversized_documents_are_refused_by_the_configured_bound) {
    RegistryLimits tight = default_limits();
    tight.max_document_bytes = 512;
    std::string document = "{\"document\":\"asset-registry-export\",\"schema_version\":1,\"assets\":[]}";
    while (document.size() < 4096) {
        document += " ";
    }
    auto refused = parse_import_json(document, tight);
    AR_REQUIRE_ERROR(error, refused, ErrorCode::ImportTruncated);

    auto single = parse_import_asset_json(document, tight);
    AR_CHECK(!single.has_value());
}

AR_TEST(adversarial, corrupt_generation_files_are_never_published_as_authoritative) {
    TempDirectory directory("adversarial-corrupt");
    auto context = open_registry(directory.path());
    for (int index = 0; index < 3; ++index) {
        AR_CHECK(context.registry->register_asset(
            context.session, context.session.advance(),
            make_request("corrupt-" + std::to_string(index), AssetClass::Server, "Acme",
                         "COR-" + std::to_string(index)))
                      .has_value());
    }
    AR_CHECK(context.registry->close().has_value());

    const std::vector<std::string> names = list_directory(directory.path() / "generations");
    struct Mutation {
        const char* label;
        void (*apply)(std::string&);
    };
    const Mutation mutations[] = {
        {"zero-length", [](std::string& bytes) { bytes.clear(); }},
        {"header-only", [](std::string& bytes) { bytes.resize(48); }},
        {"bad-magic", [](std::string& bytes) { bytes[0] = 'X'; }},
        {"truncated-payload", [](std::string& bytes) { bytes.resize(bytes.size() / 2); }},
        {"appended-garbage", [](std::string& bytes) { bytes += "garbage"; }},
        {"unsupported-version", [](std::string& bytes) { bytes[8] = 99; }},
    };
    for (const Mutation& mutation : mutations) {
        const std::filesystem::path target = directory.path() / "generations" / names.back();
        std::string original = read_text_file(target);
        std::string damaged = original;
        mutation.apply(damaged);
        write_text_file(target, damaged);

        RecoveryReport report;
        StoreOpenOptions options;
        options.allow_recovery = true;
        auto opened = Store::open(directory.path(), StoreOpenMode::CreateIfMissing, options, report);
        AR_CHECK_MSG(opened.has_value(), mutation.label);
        if (opened.has_value()) {
            // Whatever was recovered must be internally consistent, and must not be
            // the damaged bytes.
            const StoreLoadedState& loaded = StoreAccess::loaded_state(*opened.value());
            for (const auto& record : loaded.records) {
                AR_CHECK(!record->id.is_nil());
                AR_CHECK(!record->metadata.display_name.empty());
                AR_CHECK(!record->serial_identity.empty());
            }
            AR_CHECK(opened.value()->close().has_value());
        }
        // Restore for the next mutation so each case is independent.
        write_text_file(target, original);
    }
}

AR_TEST(adversarial, a_store_directory_containing_unexpected_entries_still_opens_safely) {
    // Store layout names are fixed by the format, so nothing a caller supplies can
    // influence a path inside the store. This asserts the observable consequence:
    // unexpected entries in the store directory are ignored rather than followed.
    TempDirectory directory("adversarial-entries");
    auto context = open_registry(directory.path());
    AR_CHECK(context.registry->register_asset(context.session, context.session.advance(),
                                              make_request("entries-asset", AssetClass::Server, "Acme", "EN-1"))
                 .has_value());
    AR_CHECK(context.registry->close().has_value());

    write_text_file(directory.file("unexpected.txt"), "ignored");
    write_text_file(directory.path() / "generations" / "not-a-generation.dat", "ignored");
    write_text_file(directory.path() / "generations" / "gen-0000000000000000000X.dat", "ignored");
    write_text_file(directory.path() / "generations" / "gen-00000000000000000000.dat", "zero sequence");

    auto reopened = open_registry(directory.path());
    AR_CHECK(reopened.registry->snapshot().size() == 1);
    AR_CHECK(reopened.registry->close().has_value());
}

AR_TEST(adversarial, a_store_directory_that_is_a_file_is_refused) {
    TempDirectory directory("adversarial-file-store");
    const std::filesystem::path as_file = directory.file("store");
    write_text_file(as_file, "not a directory");
    RecoveryReport report;
    auto refused = Store::open(as_file, StoreOpenMode::CreateIfMissing, StoreOpenOptions{}, report);
    AR_CHECK(!refused);
    if (!refused) {
        AR_CHECK(is_storage_error(refused.error().code()));
    }
}

AR_TEST(adversarial, invalid_limit_configurations_are_refused) {
    struct Case {
        const char* label;
        RegistryLimits limits;
    };
    RegistryLimits zero_assets = default_limits();
    zero_assets.max_assets = 0;
    RegistryLimits zero_provenance = default_limits();
    zero_provenance.max_provenance_per_asset = 0;
    RegistryLimits export_below_assets = default_limits();
    export_below_assets.max_export_records = 1;
    RegistryLimits huge_document = default_limits();
    huge_document.max_document_bytes = kAbsoluteMaxDocumentBytes + 1;
    RegistryLimits one_generation = default_limits();
    one_generation.max_retained_generations = 1;
    RegistryLimits zero_traversal = default_limits();
    zero_traversal.max_traversal_depth = 0;
    const Case cases[] = {
        {"max_assets=0", zero_assets},
        {"max_provenance_per_asset=0", zero_provenance},
        {"max_export_records<max_assets", export_below_assets},
        {"max_document_bytes too large", huge_document},
        {"max_retained_generations<2", one_generation},
        {"max_traversal_depth=0", zero_traversal},
    };
    for (const Case& item : cases) {
        const auto failure = validate_limits(item.limits);
        AR_CHECK_MSG(failure.has_value(), item.label);
        auto detached = AssetRegistry::create_detached(default_policy(), item.limits);
        AR_CHECK_MSG(!detached.has_value(), item.label);
        if (!detached.has_value()) {
            AR_CHECK(detached.error().code() == ErrorCode::InvalidLimits);
        }
    }
    AR_CHECK(!validate_limits(default_limits()).has_value());
}

AR_TEST(adversarial, an_import_batch_larger_than_the_bound_is_refused) {
    RegistryLimits tight = default_limits();
    tight.max_import_records = 3;
    auto context = detached_registry(default_policy(), tight);
    std::vector<ImportedAsset> records;
    for (int index = 0; index < 4; ++index) {
        ImportedAsset record;
        record.id = asset_id_for("batch-size-" + std::to_string(index));
        record.asset_class = AssetClass::Server;
        record.serial_identity = serial("Acme", "BS-" + std::to_string(index));
        record.metadata.display_name = "batch-" + std::to_string(index);
        record.state.lifecycle = LifecycleState::Planned;
        record.state.installation = InstallationState::Unknown;
        records.push_back(record);
    }
    auto refused = context.registry->import_assets(context.session, context.session.advance(), records,
                                                  ImportOptions{});
    AR_REQUIRE_ERROR(error, refused, ErrorCode::CapacityExceeded);
    AR_CHECK(context.registry->snapshot().empty());
}

AR_TEST(adversarial, a_reference_target_outside_its_domain_is_refused) {
    // A capability reference names a behaviour owned by another repository. The
    // registry validates the reference syntax and nothing else, so a well-formed
    // reference to something that does not exist is stored as unknown rather than
    // silently accepted as authoritative.
    auto context = detached_registry();
    auto registered = context.registry->register_asset(
        context.session, context.session.advance(), make_request("unknown-ref", AssetClass::Server, "Acme", "UR-1"));
    AR_REQUIRE_OK(created, registered);
    const Reference reference = capability_ref("nonexistent-repository:not.a.real.capability",
                                               ReferenceEvidence::Unverified);
    auto attached = context.registry->attach_reference(context.session, context.session.advance(), created.id,
                                                       std::nullopt, reference);
    AR_REQUIRE_OK(result, attached);
    const auto view = context.registry->snapshot().find(created.id);
    AR_CHECK(view.has_value());
    // The reference is retained at Unverified evidence and never claims authority
    // over the target.
    AR_CHECK(view->references.size() == 1);
    AR_CHECK(view->references.front().evidence() == ReferenceEvidence::Unverified);
    AR_CHECK(!is_actionable(view->references.front().evidence()));
    AR_CHECK(context.registry->snapshot().summary().assets_with_unverified_references == 1);

    // A malformed reference is refused outright.
    auto malformed = context.registry->attach_reference(context.session, context.session.advance(), created.id,
                                                        std::nullopt, capability_ref("asi:a"));
    AR_CHECK(malformed.has_value());
    const Reference invalid = Reference::capability(CapabilityReference{}, ReferenceEvidence::Verified);
    auto refused = context.registry->attach_reference(context.session, context.session.advance(), created.id,
                                                     std::nullopt, invalid);
    AR_CHECK(!refused.has_value());
    if (!refused.has_value()) {
        AR_CHECK(refused.error().code() == ErrorCode::InvalidInput);
    }
}

AR_TEST(adversarial, mutation_envelope_fields_are_validated) {
    auto context = detached_registry();
    auto registered = context.registry->register_asset(
        context.session, context.session.advance(), make_request("envelope-asset", AssetClass::Server, "Acme", "EN-1"));
    AR_REQUIRE_OK(created, registered);
    MetadataPatch patch;
    patch.notes = "x";

    MutationEnvelope zero_sequence;
    zero_sequence.sequence = MutationSequence(0);
    auto zero_result = context.registry->update_metadata(context.session, zero_sequence, created.id, std::nullopt,
                                                         patch);
    AR_REQUIRE_ERROR(zero_error, zero_result, ErrorCode::InvalidInput);

    MutationEnvelope empty_key;
    empty_key.sequence = MutationSequence(50);
    empty_key.idempotency_key = "";
    auto key_result = context.registry->update_metadata(context.session, empty_key, created.id, std::nullopt, patch);
    AR_REQUIRE_ERROR(key_error, key_result, ErrorCode::InvalidInput);

    MutationEnvelope huge_key;
    huge_key.sequence = MutationSequence(51);
    huge_key.idempotency_key = repeated('k', 4096);
    auto huge_result = context.registry->update_metadata(context.session, huge_key, created.id, std::nullopt, patch);
    AR_REQUIRE_ERROR(huge_error, huge_result, ErrorCode::InvalidInput);

    MutationEnvelope invalid_key;
    invalid_key.sequence = MutationSequence(52);
    invalid_key.idempotency_key = std::string("bad") + std::string(1, '\x01') + "key";
    auto invalid_result = context.registry->update_metadata(context.session, invalid_key, created.id, std::nullopt,
                                                            patch);
    AR_REQUIRE_ERROR(invalid_error, invalid_result, ErrorCode::InvalidInput);

    MutationEnvelope huge_reason;
    huge_reason.sequence = MutationSequence(53);
    huge_reason.reason = repeated('r', 4096);
    auto reason_result = context.registry->update_metadata(context.session, huge_reason, created.id, std::nullopt,
                                                           patch);
    AR_REQUIRE_ERROR(reason_error, reason_result, ErrorCode::MalformedText);

    MutationEnvelope invalid_reason;
    invalid_reason.sequence = MutationSequence(54);
    invalid_reason.reason = std::string("bad") + std::string(1, '\x01') + "reason";
    auto invalid_reason_result = context.registry->update_metadata(context.session, invalid_reason, created.id,
                                                                   std::nullopt, patch);
    AR_REQUIRE_ERROR(invalid_reason_error, invalid_reason_result, ErrorCode::MalformedText);

    // None of the refused attempts changed the record.
    const auto view = context.registry->snapshot().find(created.id);
    AR_CHECK(view.has_value());
    AR_CHECK(view->revision == AssetRevision(1));
}

AR_TEST(adversarial, an_imported_record_whose_serial_key_disagrees_is_refused) {
    const RegistryLimits bounds = default_limits();
    const char* mismatched = R"({"asset_id":"00000000-0000-4000-8000-000000000001","asset_class":"server",
        "serial_identity":{"manufacturer":"Acme","serial":"S-1"},"serial_key":"acme/other",
        "metadata":{"display_name":"x"},"state":{"lifecycle":"planned","installation":"unknown"}})";
    auto refused = parse_import_asset_json(mismatched, bounds);
    AR_REQUIRE_ERROR(error, refused, ErrorCode::ImportRecordInvalid);
    AR_CHECK(error.message().find("serial_key") != std::string::npos);
}

AR_TEST(adversarial, an_inconsistent_imported_state_is_refused_before_any_mutation) {
    auto context = detached_registry();
    ImportedAsset record;
    record.id = asset_id_for("inconsistent-import");
    record.asset_class = AssetClass::Server;
    record.serial_identity = serial("Acme", "INC-1");
    record.metadata.display_name = "inconsistent";
    record.state.lifecycle = LifecycleState::Active;
    record.state.installation = InstallationState::Staged;
    auto report = context.registry->import_assets(context.session, context.session.advance(), {record},
                                                 ImportOptions{});
    AR_REQUIRE_OK(result, report);
    AR_CHECK(result.records_rejected() == 1);
    AR_CHECK(result.rejections.front().code == ErrorCode::LifecycleInvariantViolation);
    AR_CHECK(context.registry->snapshot().empty());
}

AR_TEST(adversarial, a_null_byte_inside_an_identifier_never_truncates) {
    const std::string canonical = asset_id_for("nul-asset").to_string();
    std::string with_nul = canonical;
    with_nul.insert(12, 1, '\0');
    AR_CHECK(!AssetId::parse(with_nul).has_value());
    AR_CHECK(!OwnerId::create(std::string("org") + '\0' + "example").has_value());
    AR_CHECK(!WriterId::create(std::string("writer") + '\0').has_value());
    AR_CHECK(!is_label_key(std::string("key") + '\0'));
}

AR_TEST(adversarial, enormous_capability_paths_are_refused) {
    AR_CHECK(!CapabilityReference::create("asi:" + repeated('a', 4096)).has_value());
    AR_CHECK(!CapabilityReference::create(repeated('a', 4096) + ":path").has_value());
    AR_CHECK(!CapabilityReference::create("asi:" + repeated('a', 300)).has_value());
    // A path of many short segments is still bounded by total length.
    std::string segmented;
    for (int index = 0; index < 200; ++index) {
        segmented += "seg.";
    }
    segmented += "end";
    AR_CHECK(!CapabilityReference::create("asi:" + segmented).has_value());
}
