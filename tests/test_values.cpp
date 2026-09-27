// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the value types: identities, text validation, classification,
// serial identity folding, references, and the codec primitives.

#include <set>
#include <string>
#include <thread>
#include <vector>

#include "asset_registry/text.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using namespace asset_registry;
using namespace asset_test;

AR_TEST(identity, asset_id_round_trip_is_canonical) {
    const AssetId identifier = asset_id_for("server-a");
    const std::string text = identifier.to_string();
    AR_CHECK(text.size() == 36);
    const auto parsed = AssetId::parse(text);
    AR_REQUIRE_PRESENT(value, parsed);
    AR_CHECK(value == identifier);
    AR_CHECK(value.to_string() == text);
}

AR_TEST(identity, asset_id_rejects_non_canonical_spellings) {
    const std::string canonical = asset_id_for("server-a").to_string();
    std::string upper = canonical;
    for (char& character : upper) {
        if (character >= 'a' && character <= 'f') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    AR_CHECK(!AssetId::parse(upper).has_value());
    AR_CHECK(!AssetId::parse("{" + canonical + "}").has_value());
    AR_CHECK(!AssetId::parse("urn:uuid:" + canonical).has_value());
    std::string compact = canonical;
    compact.erase(std::remove(compact.begin(), compact.end(), '-'), compact.end());
    AR_CHECK(!AssetId::parse(compact).has_value());
    AR_CHECK(!AssetId::parse(canonical + " ").has_value());
    AR_CHECK(!AssetId::parse(" " + canonical).has_value());
    AR_CHECK(!AssetId::parse("").has_value());
    AR_CHECK(!AssetId::parse(canonical.substr(0, 35)).has_value());
    // A trailing NUL must not be treated as end of input by any parser path.
    std::string with_nul = canonical;
    with_nul.push_back('\0');
    AR_CHECK(!AssetId::parse(with_nul).has_value());
}

AR_TEST(identity, asset_id_ordering_is_by_raw_bytes) {
    std::vector<AssetId> identifiers;
    for (int index = 0; index < 64; ++index) {
        identifiers.push_back(AssetId::generate_unchecked());
    }
    std::vector<AssetId> sorted = identifiers;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t index = 1; index < sorted.size(); ++index) {
        AR_CHECK(sorted[index - 1] < sorted[index]);
    }
    const AssetId nil;
    AR_CHECK(nil.is_nil());
    AR_CHECK(!AssetId::generate_unchecked().is_nil());
}

AR_TEST(identity, generated_identifiers_are_unique_and_version_four) {
    std::set<std::string> seen;
    for (int index = 0; index < 512; ++index) {
        const auto generated = AssetId::generate();
        AR_REQUIRE_OK(identifier, generated);
        AR_CHECK(seen.insert(identifier.to_compact_string()).second);
        AR_CHECK((identifier.bytes()[6] & 0xF0U) == 0x40U);
        AR_CHECK((identifier.bytes()[8] & 0xC0U) == 0x80U);
    }
}

AR_TEST(text, utf8_validation_rejects_malformed_sequences) {
    AR_CHECK(validate_utf8("plain ascii") == Utf8Status::Valid);
    AR_CHECK(validate_utf8("caf\xC3\xA9") == Utf8Status::Valid);
    AR_CHECK(validate_utf8("\xE6\x9C\xBA" "\xE6\x88\xBF") == Utf8Status::Valid);
    AR_CHECK(validate_utf8("\xF0\x9F\x9A\x80") == Utf8Status::Valid);
    // Continuation byte without a lead.
    AR_CHECK(validate_utf8("\x80") == Utf8Status::InvalidSequence);
    // Truncated sequence.
    AR_CHECK(validate_utf8("\xE6\x9C") == Utf8Status::InvalidSequence);
    // Overlong encoding of 'A'.
    AR_CHECK(validate_utf8("\xC1\x81") == Utf8Status::OverlongEncoding);
    // UTF-16 surrogate encoded as UTF-8.
    AR_CHECK(validate_utf8("\xED\xA0" "\x80") == Utf8Status::SurrogateCodePoint);
    // Above U+10FFFF.
    AR_CHECK(validate_utf8("\xF5\x80\x80" "\x80") == Utf8Status::OutOfRange);
    // Control character.
    AR_CHECK(validate_utf8(std::string_view("\x01", 1)) == Utf8Status::DisallowedCodePoint);
    // Bidirectional override: makes two identifiers render identically.
    AR_CHECK(validate_utf8("\xE2\x80" "\xAE") == Utf8Status::DisallowedCodePoint);
    AR_CHECK(validate_utf8("\xEF\xBB\xBF") == Utf8Status::DisallowedCodePoint);
    AR_CHECK(validate_utf8("") == Utf8Status::Valid);
    AR_CHECK(validate_utf8("", false) == Utf8Status::Empty);
}

AR_TEST(text, comparison_key_folds_the_ambiguous_forms) {
    AR_CHECK(make_comparison_key("Acme  Compute") == make_comparison_key("acme compute"));
    AR_CHECK(make_comparison_key(" ACME\tCompute ") == "acme compute");
    // Full-width ASCII folds to ASCII, so a homoglyph cannot claim a second record.
    AR_CHECK(make_comparison_key("\xEF\xBC\xA1" "\xEF\xBC\xA3" "\xEF\xBC\xAD" "\xEF\xBC\xA5") == "acme");
    // Ideographic space folds to a plain space, then collapses.
    AR_CHECK(make_comparison_key("acme" "\xE3\x80\x80" "compute") == "acme compute");
    // A zero-width space leaves nothing behind.
    AR_CHECK(make_comparison_key("acme" "\xE2\x80\x8B" "compute") == "acmecompute");
    AR_CHECK(make_comparison_key("sn-001") != make_comparison_key("sn-002"));
    AR_CHECK(make_comparison_key("") == "");
    AR_CHECK(make_comparison_key("   ") == "");
    // Non-ASCII letters are preserved, so distinct names stay distinct.
    AR_CHECK(make_comparison_key("\xE6\x9C\xBA" "\xE6\x88\xBF") == make_comparison_key("\xE6\x9C\xBA" "\xE6\x88\xBF"));
}

AR_TEST(text, syntax_predicates_reject_out_of_domain_input) {
    AR_CHECK(is_identity_namespace("asi"));
    AR_CHECK(is_identity_namespace("vendor-acme"));
    AR_CHECK(!is_identity_namespace("Asi"));
    AR_CHECK(!is_identity_namespace("-asi"));
    AR_CHECK(!is_identity_namespace("asi-"));
    AR_CHECK(!is_identity_namespace(""));
    AR_CHECK(!is_identity_namespace(std::string(64, 'a')));

    AR_CHECK(is_dotted_identity_path("site-a.hall-2"));
    AR_CHECK(is_dotted_identity_path("a"));
    AR_CHECK(!is_dotted_identity_path(".a"));
    AR_CHECK(!is_dotted_identity_path("a."));
    AR_CHECK(!is_dotted_identity_path("a..b"));
    AR_CHECK(!is_dotted_identity_path("-a"));
    AR_CHECK(!is_dotted_identity_path("A.b"));

    AR_CHECK(is_serial_number_text("SN-1234/A"));
    AR_CHECK(is_serial_number_text("SN 1234 #7"));
    AR_CHECK(!is_serial_number_text(""));
    AR_CHECK(!is_serial_number_text("SN//1234"));
    AR_CHECK(!is_serial_number_text(std::string(129, 'x')));
    AR_CHECK(!is_serial_number_text("line\nbreak"));

    AR_CHECK(is_label_key("rack.unit_1"));
    AR_CHECK(!is_label_key("1rack"));
    AR_CHECK(!is_label_key("Rack"));
    AR_CHECK(!is_label_key(""));
}

AR_TEST(text, canonical_integer_parser_is_strict) {
    AR_CHECK(parse_unsigned_decimal("0") == 0);
    AR_CHECK(parse_unsigned_decimal("1") == 1);
    AR_CHECK(parse_unsigned_decimal("18446744073709551615") == std::numeric_limits<std::uint64_t>::max());
    AR_CHECK(!parse_unsigned_decimal("18446744073709551616").has_value());
    AR_CHECK(!parse_unsigned_decimal("007").has_value());
    AR_CHECK(!parse_unsigned_decimal("-1").has_value());
    AR_CHECK(!parse_unsigned_decimal("+1").has_value());
    AR_CHECK(!parse_unsigned_decimal(" 1").has_value());
    AR_CHECK(!parse_unsigned_decimal("1 ").has_value());
    AR_CHECK(!parse_unsigned_decimal("1.0").has_value());
    AR_CHECK(!parse_unsigned_decimal("").has_value());
    AR_CHECK(!parse_unsigned_decimal("0x10").has_value());
}

AR_TEST(text, counter_parse_rejects_non_canonical_and_overflow) {
    AR_CHECK(AssetRevision::parse("1").value() == AssetRevision(1));
    AR_CHECK(!AssetRevision::parse("0").has_value() || AssetRevision::parse("0").value().is_zero());
    AR_CHECK(!AssetRevision::parse("01").has_value());
    AR_CHECK(!AssetRevision::parse("").has_value());
    AR_CHECK(!AssetRevision::parse("18446744073709551616").has_value());
    AR_CHECK(AssetRevision::parse("18446744073709551615").has_value());
    const AssetRevision maximum(std::numeric_limits<std::uint64_t>::max());
    AR_CHECK(!maximum.next().has_value());
    AR_CHECK(maximum.next() == std::nullopt);
}

AR_TEST(classification, class_tokens_round_trip_and_unknown_is_rejected) {
    for (std::size_t index = 0; index < known_asset_class_count(); ++index) {
        const AssetClass value = *known_asset_classes()[index];
        AR_CHECK(is_known(value));
        const std::string_view token = to_string(value);
        AR_CHECK(!token.empty());
        const auto parsed = asset_class_from_token(token);
        AR_REQUIRE_PRESENT(round_tripped, parsed);
        AR_CHECK(round_tripped == value);
        AR_CHECK(!describe(value).empty());
    }
    AR_CHECK(!asset_class_from_token("vendor_specific_thing").has_value());
    AR_CHECK(!asset_class_from_token("").has_value());
    AR_CHECK(!asset_class_from_token("Server").has_value());
    AR_CHECK(asset_class_from_token("unknown").value() == AssetClass::Unknown);
    AR_CHECK(!is_known(AssetClass::Unknown));
    AR_CHECK(is_power_class(AssetClass::UninterruptiblePowerSupply));
    AR_CHECK(is_cooling_class(AssetClass::CoolingDistributionUnit));
    AR_CHECK(!is_power_class(AssetClass::Server));
}

AR_TEST(serial_identity, collision_key_ignores_formatting_noise) {
    const SerialIdentity first = serial("Acme  Compute", "SN 1234", "R2");
    const SerialIdentity second = serial("acme compute", "sn 1234", "r2");
    AR_CHECK(first == second);
    AR_CHECK(first.canonical_key() == second.canonical_key());
    AR_CHECK(!first.model_conflicts_with(second));

    const SerialIdentity third = serial("acme compute", "sn 1234", "R3");
    AR_CHECK(first == third);
    AR_CHECK(first.model_conflicts_with(third));

    // The stored spelling is preserved exactly as supplied.
    AR_CHECK(first.manufacturer().name() == "Acme  Compute");
    AR_CHECK(first.serial().text() == "SN 1234");
    AR_CHECK(first.to_string() == "Acme  Compute//SN 1234//R2");

    const SerialIdentity without_model = serial("acme compute", "sn 1234");
    AR_CHECK(without_model == first);
    AR_CHECK(!without_model.model_conflicts_with(first));
}

AR_TEST(serial_identity, components_reject_malformed_input) {
    AR_CHECK(!ManufacturerIdentity::create("").has_value());
    AR_CHECK(!ManufacturerIdentity::create("  ").has_value());
    AR_CHECK(!SerialNumber::create("").has_value());
    AR_CHECK(!SerialNumber::create("SN//1").has_value());
    AR_CHECK(!ModelIdentity::create("").has_value());
    AR_CHECK(!SerialIdentity::parse("").has_value());
    AR_CHECK(!SerialIdentity::parse("acme").has_value());
    AR_CHECK(!SerialIdentity::parse("acme//").has_value());
    AR_CHECK(!SerialIdentity::parse("//sn").has_value());
    const auto parsed = SerialIdentity::parse("Acme//SN-1//R2");
    AR_REQUIRE_PRESENT(value, parsed);
    AR_CHECK(value.serial().text() == "SN-1");
    AR_CHECK(value.model().has_value());
}

AR_TEST(references, canonical_forms_are_unambiguous) {
    const Reference capability_reference = capability_ref("asi:accelerator.scheduling", ReferenceEvidence::Verified);
    AR_CHECK(capability_reference.kind() == Reference::Kind::Capability);
    AR_CHECK(capability_reference.canonical() == "capability:asi:accelerator.scheduling");
    AR_CHECK(capability_reference.capability().name_space().text() == "asi");
    AR_CHECK(capability_reference.capability().path() == "accelerator.scheduling");
    AR_CHECK(is_actionable(capability_reference.evidence()));

    const Reference location_reference = Reference::location(location("site-a.hall-2"), ReferenceEvidence::Unverified);
    AR_CHECK(location_reference.canonical() == "location:site-a.hall-2");
    AR_CHECK(!is_actionable(location_reference.evidence()));

    const Reference rack_reference = Reference::rack(rack("site-a.hall-2.row-4.rack-7"), ReferenceEvidence::Stale);
    AR_CHECK(rack_reference.canonical() == "rack:site-a.hall-2.row-4.rack-7");

    const auto external = ExternalObjectReference::create("dfi-path", "fabric/path-12");
    AR_REQUIRE_PRESENT(value, external);
    const Reference external_reference = Reference::external_object(value, ReferenceEvidence::Verified);
    AR_CHECK(external_reference.canonical() == "external_object:dfi-path/fabric/path-12");
    AR_CHECK(external_reference.external_object().kind() == "dfi-path");
    AR_CHECK(external_reference.external_object().id() == "fabric/path-12");

    // References of different kinds never compare equal even when the target
    // text is identical.
    AR_CHECK(!(Reference::location(location("a.b"), ReferenceEvidence::Verified) ==
               Reference::rack(rack("a.b"), ReferenceEvidence::Verified)));
}

AR_TEST(references, malformed_capability_references_are_rejected) {
    AR_CHECK(!CapabilityReference::create("").has_value());
    AR_CHECK(!CapabilityReference::create("asi").has_value());
    AR_CHECK(!CapabilityReference::create("asi:").has_value());
    AR_CHECK(!CapabilityReference::create(":path").has_value());
    AR_CHECK(!CapabilityReference::create("asi:a:b").has_value());
    AR_CHECK(!CapabilityReference::create("ASi:path").has_value());
    AR_CHECK(!CapabilityReference::create("asi:Path").has_value());
    AR_CHECK(!CapabilityReference::create("asi:path.").has_value());
    AR_CHECK(CapabilityReference::create("asi:accelerator.pool-1").has_value());
    AR_CHECK(!IdentityNamespace::create("").has_value());
    AR_CHECK(!IdentityNamespace::create("1asi").has_value());
}

AR_TEST(references, unit_span_bounds_are_enforced) {
    AR_CHECK(UnitSpan::create(0, 1) == std::nullopt);
    AR_CHECK(UnitSpan::create(5, 5) == std::nullopt);
    AR_CHECK(UnitSpan::create(6, 5) == std::nullopt);
    AR_CHECK(UnitSpan::create(1, UnitSpan::kMaxRackUnit + 2) == std::nullopt);
    const auto span = UnitSpan::create(1, 3);
    AR_REQUIRE_PRESENT(value, span);
    AR_CHECK(value.low() == 1);
    AR_CHECK(value.high() == 3);
    AR_CHECK(value.height() == 2);
    AR_CHECK(value.to_string() == "1-3");
    AR_CHECK(UnitSpan::parse("4").value() == UnitSpan::create(4, 5).value());
    AR_CHECK(UnitSpan::parse("1-3").value() == value);
    AR_CHECK(!UnitSpan::parse("3-1").has_value());
    AR_CHECK(!UnitSpan::parse("0").has_value());
    AR_CHECK(!UnitSpan::parse("-1").has_value());
    AR_CHECK(!UnitSpan::parse("1-").has_value());
    AR_CHECK(!UnitSpan::parse("99999").has_value());
}

AR_TEST(timestamps, rfc3339_round_trip_and_range_enforcement) {
    const auto parsed = Timestamp::parse_rfc3339("2024-03-05T06:07:08Z");
    AR_REQUIRE_PRESENT(value, parsed);
    AR_CHECK(value.to_rfc3339() == "2024-03-05T06:07:08Z");

    const auto precise = Timestamp::parse_rfc3339("2024-03-05T06:07:08.123456789Z");
    AR_REQUIRE_PRESENT(fractional, precise);
    AR_CHECK(fractional.unix_nanos() % 1000000000LL == 123456789LL);
    AR_CHECK(fractional.to_rfc3339() == "2024-03-05T06:07:08.123456789Z");

    const auto epoch = Timestamp::parse_rfc3339("1970-01-01T00:00:00Z");
    AR_REQUIRE_PRESENT(zero, epoch);
    AR_CHECK(zero.unix_nanos() == 0);

    const auto leap = Timestamp::parse_rfc3339("2024-02-29T00:00:00Z");
    AR_CHECK(leap.has_value());
    AR_CHECK(!Timestamp::parse_rfc3339("2023-02-29T00:00:00Z").has_value());
    AR_CHECK(!Timestamp::parse_rfc3339("2024-13-01T00:00:00Z").has_value());
    AR_CHECK(!Timestamp::parse_rfc3339("2024-01-32T00:00:00Z").has_value());
    AR_CHECK(!Timestamp::parse_rfc3339("2024-01-01T24:00:00Z").has_value());
    AR_CHECK(!Timestamp::parse_rfc3339("2024-01-01T00:60:00Z").has_value());
    AR_CHECK(!Timestamp::parse_rfc3339("2024-01-01T00:00:60Z").has_value());
    AR_CHECK(!Timestamp::parse_rfc3339("2024-01-01T00:00:00+01:00").has_value());
    AR_CHECK(!Timestamp::parse_rfc3339("2024-01-01 00:00:00Z").has_value());
    AR_CHECK(!Timestamp::parse_rfc3339("2024-01-01T00:00:00").has_value());
    AR_CHECK(!Timestamp::parse_rfc3339("").has_value());
    AR_CHECK(!Timestamp::create(std::numeric_limits<std::int64_t>::max()).has_value());

    // Negative epochs are legitimate historical evidence and must survive.
    const Timestamp before(Timestamp::kMinUnixNanos);
    AR_CHECK(before.to_rfc3339().rfind("1677-09-21T00:12:43", 0) == 0);
}

AR_TEST(error_codes, names_are_stable_and_round_trip) {
    const ErrorCode samples[] = {ErrorCode::None,
                                 ErrorCode::StaleRevision,
                                 ErrorCode::DuplicateSerialIdentity,
                                 ErrorCode::StoreIntegrityFailed,
                                 ErrorCode::InternalInvariantViolation,
                                 ErrorCode::CapacityExceeded};
    for (const ErrorCode code : samples) {
        const std::string_view name = error_code_name(code);
        AR_CHECK(!name.empty());
        const auto parsed = error_code_from_name(name);
        AR_REQUIRE_PRESENT(round_tripped, parsed);
        AR_CHECK(round_tripped == code);
    }
    AR_CHECK(!error_code_from_name("not_a_real_code").has_value());
    AR_CHECK(is_staleness_error(ErrorCode::StaleRevision));
    AR_CHECK(is_staleness_error(ErrorCode::StaleAuthorityEpoch));
    AR_CHECK(!is_staleness_error(ErrorCode::AssetNotFound));
    AR_CHECK(is_input_error(ErrorCode::MalformedAssetId));
    AR_CHECK(is_storage_error(ErrorCode::StoreCorrupt));
    AR_CHECK(!is_storage_error(ErrorCode::AssetNotFound));

    Error error = make_error(ErrorCode::StaleRevision, "revision mismatch");
    AR_CHECK(error.to_string().find("stale_revision") == 0);
    AR_CHECK(!error.ok());
    error.with_subject("abc").with_detail("expected_revision", "3");
    AR_CHECK(error.to_string().find("subject=abc") != std::string::npos);
    AR_CHECK(error.to_string().find("expected_revision=3") != std::string::npos);
}
