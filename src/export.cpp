// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Canonical export and strict import.
//
// The export document is the interchange contract for later DCCP consumers. Its
// determinism is a property of the emitter: assets appear in canonical AssetId
// order, references and labels appear in canonical order, object members appear in
// the fixed order documented in the README, and every scalar is rendered in one
// canonical spelling. Two exports of one snapshot at one schema version are
// byte-identical.
//
// Import is the mirror image and is strict: unknown members, missing required
// members, wrong types, out-of-domain enumerations, non-canonical integers, and
// inconsistent state pairs are rejected with a machine-readable code and the
// member path that failed.

#include "asset_registry/export.hpp"

#include <algorithm>
#include <map>

#include "asset_registry/text.hpp"
#include "json_encode.hpp"
#include "json_parse.hpp"

namespace asset_registry {
namespace {

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

void write_labels(internal::JsonWriter& writer, const std::vector<std::pair<std::string, std::string>>& labels) {
    writer.begin_array();
    for (const auto& label : labels) {
        writer.begin_object();
        writer.key("key");
        writer.value_string(label.first);
        writer.key("value");
        writer.value_string(label.second);
        writer.end_object();
    }
    writer.end_array();
}

void write_reference(internal::JsonWriter& writer, const Reference& reference) {
    writer.begin_object();
    writer.key("kind");
    writer.value_string(to_string(reference.kind()));
    writer.key("evidence");
    writer.value_string(to_string(reference.evidence()));
    writer.key("target");
    switch (reference.kind()) {
        case Reference::Kind::Capability:
            writer.value_string(reference.capability().canonical());
            break;
        case Reference::Kind::Location:
            writer.value_string(reference.location().text());
            break;
        case Reference::Kind::Rack:
            writer.value_string(reference.rack().text());
            break;
        case Reference::Kind::ExternalObject: {
            std::string target = reference.external_object().kind();
            target += '/';
            target += reference.external_object().id();
            writer.value_string(target);
            break;
        }
    }
    writer.end_object();
}

void write_metadata_members(internal::JsonWriter& writer, const AssetMetadata& metadata) {
    writer.key("display_name");
    writer.value_string(metadata.display_name);
    writer.key("owner");
    if (metadata.owner.has_value()) {
        writer.value_string(metadata.owner->text());
    } else {
        writer.value_null();
    }
    writer.key("site");
    if (metadata.site.has_value()) {
        writer.value_string(metadata.site->text());
    } else {
        writer.value_null();
    }
    writer.key("notes");
    writer.value_string(metadata.notes);
    writer.key("labels");
    write_labels(writer, metadata.labels);
}

void write_serial(internal::JsonWriter& writer, const SerialIdentity& identity) {
    writer.key("manufacturer");
    writer.value_string(identity.manufacturer().name());
    writer.key("serial");
    writer.value_string(identity.serial().text());
    writer.key("model");
    if (identity.model().has_value()) {
        writer.value_string(identity.model()->text());
    } else {
        writer.value_null();
    }
}

void write_provenance(internal::JsonWriter& writer, const std::vector<ProvenanceStep>& steps) {
    writer.begin_array();
    for (const ProvenanceStep& step : steps) {
        writer.begin_object();
        writer.key("sequence");
        writer.value_unsigned(step.sequence.value());
        writer.key("epoch");
        writer.value_unsigned(step.epoch.value());
        writer.key("revision");
        writer.value_unsigned(step.revision.value());
        writer.key("action");
        writer.value_string(to_string(step.action));
        writer.key("origin");
        writer.value_string(to_string(step.origin));
        writer.key("actor");
        writer.value_string(step.actor.to_string());
        writer.key("recorded_at");
        if (step.recorded_at.has_value()) {
            writer.value_string(step.recorded_at->to_rfc3339());
        } else {
            writer.value_null();
        }
        writer.key("reason");
        writer.value_string(step.reason);
        writer.key("change");
        writer.value_string(step.change);
        writer.end_object();
    }
    writer.end_array();
}

void write_history(internal::JsonWriter& writer, const std::vector<AssetIncarnation>& history) {
    writer.begin_array();
    for (const AssetIncarnation& incarnation : history) {
        writer.begin_object();
        writer.key("generation");
        writer.value_unsigned(incarnation.generation.value());
        writer.key("asset_class");
        writer.value_string(to_string(incarnation.asset_class));
        writer.key("final_revision");
        writer.value_unsigned(incarnation.final_revision.value());
        writer.key("closed_at");
        writer.value_unsigned(incarnation.closed_at.value());
        writer.key("closed_time");
        if (incarnation.closed_time.has_value()) {
            writer.value_string(incarnation.closed_time->to_rfc3339());
        } else {
            writer.value_null();
        }
        writer.key("serial_identity");
        writer.begin_object();
        write_serial(writer, incarnation.serial_identity);
        writer.end_object();
        writer.key("metadata");
        writer.begin_object();
        write_metadata_members(writer, incarnation.metadata);
        writer.end_object();
        writer.key("references");
        writer.begin_array();
        for (const Reference& reference : incarnation.references) {
            write_reference(writer, reference);
        }
        writer.end_array();
        writer.key("state");
        writer.begin_object();
        writer.key("lifecycle");
        writer.value_string(to_string(incarnation.state.lifecycle));
        writer.key("installation");
        writer.value_string(to_string(incarnation.state.installation));
        writer.end_object();
        writer.end_object();
    }
    writer.end_array();
}

void write_asset(internal::JsonWriter& writer, const AssetRecord& record, bool include_provenance) {
    writer.begin_object();
    writer.key("asset_id");
    writer.value_string(record.id.to_string());
    writer.key("asset_class");
    writer.value_string(to_string(record.asset_class));
    writer.key("generation");
    writer.value_unsigned(record.generation.value());
    writer.key("revision");
    writer.value_unsigned(record.revision.value());
    writer.key("last_sequence");
    writer.value_unsigned(record.last_sequence.value());
    writer.key("serial_identity");
    writer.begin_object();
    write_serial(writer, record.serial_identity);
    writer.end_object();
    writer.key("serial_key");
    writer.value_string(record.serial_identity.canonical_key());
    writer.key("metadata");
    writer.begin_object();
    write_metadata_members(writer, record.metadata);
    writer.end_object();
    writer.key("state");
    writer.begin_object();
    writer.key("lifecycle");
    writer.value_string(to_string(record.state.lifecycle));
    writer.key("installation");
    writer.value_string(to_string(record.state.installation));
    writer.end_object();
    writer.key("references");
    writer.begin_array();
    for (const Reference& reference : record.references) {
        write_reference(writer, reference);
    }
    writer.end_array();
    writer.key("supersedes");
    if (record.supersedes.has_value()) {
        writer.begin_object();
        writer.key("asset_id");
        writer.value_string(record.supersedes->predecessor.to_string());
        writer.key("generation");
        writer.value_unsigned(record.supersedes->predecessor_generation.value());
        writer.key("final_revision");
        writer.value_unsigned(record.supersedes->predecessor_final_revision.value());
        writer.key("cause");
        writer.value_string(to_string(record.supersedes->cause));
        writer.key("linked_at");
        writer.value_unsigned(record.supersedes->linked_at.value());
        writer.key("note");
        writer.value_string(record.supersedes->note);
        writer.end_object();
    } else {
        writer.value_null();
    }
    if (include_provenance) {
        writer.key("provenance");
        write_provenance(writer, record.provenance);
        writer.key("history");
        write_history(writer, record.history);
    }
    writer.end_object();
}

[[nodiscard]] std::string render_document(const Snapshot& snapshot, const ExportOptions& options,
                                          std::uint64_t record_bound, ExportReport& report) {
    const InventorySummary summary = snapshot.summary();
    internal::JsonWriter writer(options.indent);
    writer.reserve(1024 + static_cast<std::size_t>(snapshot.size()) * 512U);
    writer.begin_object();
    writer.key("document");
    writer.value_string("asset-registry-export");
    writer.key("schema_version");
    writer.value_unsigned(options.schema_version);
    writer.key("registry_epoch");
    writer.value_unsigned(snapshot.epoch().value());
    writer.key("published_sequence");
    writer.value_unsigned(snapshot.published_sequence().value());
    writer.key("asset_count");
    writer.value_unsigned(summary.total_assets);
    writer.key("policy_fingerprint");
    writer.value_string(snapshot.policy().to_canonical_string());
    writer.key("assets");
    writer.begin_array();
    const bool include_provenance = options.format == ExportFormat::CanonicalJson;
    std::uint64_t emitted = 0;
    snapshot.for_each([&](const AssetRecord& record) -> bool {
        write_asset(writer, record, include_provenance);
        ++emitted;
        return true;
    });
    writer.end_array();
    writer.end_object();

    report.records_exported = emitted;
    report.published_sequence = snapshot.published_sequence();
    report.epoch = snapshot.epoch();
    report.schema_version = options.schema_version;
    (void)record_bound;

    std::string document = writer.take();
    if (options.trailing_newline) {
        document.push_back('\n');
    }
    report.bytes_written = document.size();
    return document;
}

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------

class DocumentReader {
public:
    DocumentReader(const internal::JsonValue& root, const RegistryLimits& bounds) : root_(root), bounds_(bounds) {}

    [[nodiscard]] Outcome<std::vector<ImportedAsset>> read() {
        if (root_.kind != internal::JsonValue::Kind::Object) {
            return fail("the document root must be an object");
        }
        // The marker is a required member: a document that omits it is not an asset
        // registry export, and accepting it would let any object with a plausible
        // schema_version and assets array be imported as one.
        const internal::JsonValue* marker = root_.find("document");
        if (marker == nullptr) {
            return fail("the document carries no document marker");
        }
        if (marker->kind != internal::JsonValue::Kind::String || marker->text != "asset-registry-export") {
            return fail("the document marker does not identify an asset registry export");
        }
        const internal::JsonValue* version = root_.find("schema_version");
        if (version == nullptr) {
            return fail("the document carries no schema_version member");
        }
        if (version->kind != internal::JsonValue::Kind::UInt) {
            return fail("schema_version must be an integer");
        }
        if (version->unsigned_value != kExportSchemaVersion) {
            return make_error(ErrorCode::ImportFormatUnsupported,
                              "export schema version " + std::to_string(version->unsigned_value) +
                                  " is not supported by this build (expected " +
                                  std::to_string(kExportSchemaVersion) + ")");
        }
        const internal::JsonValue* assets = root_.find("assets");
        if (assets == nullptr) {
            return fail("the document carries no assets member");
        }
        if (assets->kind != internal::JsonValue::Kind::Array) {
            return fail("assets must be an array");
        }
        if (assets->elements.size() > bounds_.max_import_records) {
            return make_error(ErrorCode::ImportTruncated,
                              "the document carries more records than the configured import bound");
        }

        std::vector<ImportedAsset> records;
        records.reserve(assets->elements.size());
        for (std::size_t index = 0; index < assets->elements.size(); ++index) {
            index_ = index;
            auto record = read_asset(*assets->elements[index]);
            if (!record) {
                return record.error();
            }
            records.push_back(std::move(record).value());
        }
        return records;
    }

    /// Reads exactly one asset object. Used by the inspection CLI, which accepts a
    /// single record document and must apply the same rules as a full export.
    [[nodiscard]] Outcome<ImportedAsset> read_one() {
        index_ = 0;
        if (root_.kind != internal::JsonValue::Kind::Object) {
            return fail("an asset document must be an object");
        }
        return read_asset(root_);
    }

private:
    /// Builds the rejection for the current record. Returned as an Error so the
    /// caller's Outcome type converts implicitly.
    [[nodiscard]] Error fail(std::string message) const {
        std::string full = "asset ";
        full += std::to_string(index_);
        full += ": ";
        full += message;
        return Error(ErrorCode::ImportRecordInvalid, std::move(full));
    }

    [[nodiscard]] static const internal::JsonValue* member(const internal::JsonValue& object, std::string_view name) {
        return object.find(name);
    }

    [[nodiscard]] Outcome<std::string> required_string(const internal::JsonValue& object, std::string_view name) {
        const internal::JsonValue* value = member(object, name);
        if (value == nullptr) {
            return fail("required member is missing: " + std::string(name));
        }
        if (value->kind != internal::JsonValue::Kind::String) {
            return fail("member must be a string: " + std::string(name));
        }
        return value->text;
    }

    [[nodiscard]] Outcome<std::optional<std::string>> optional_string(const internal::JsonValue& object,
                                                                     std::string_view name) {
        const internal::JsonValue* value = member(object, name);
        if (value == nullptr || value->kind == internal::JsonValue::Kind::Null) {
            return std::optional<std::string>{};
        }
        if (value->kind != internal::JsonValue::Kind::String) {
            return fail("member must be a string or null: " + std::string(name));
        }
        return std::optional<std::string>(value->text);
    }

    Outcome<void> reject_unknown_members(const internal::JsonValue& object,
                                         std::initializer_list<std::string_view> allowed) {
        for (const internal::JsonMember& entry : object.members) {
            bool known = false;
            for (const std::string_view name : allowed) {
                if (entry.name == name) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                return Outcome<void>(fail("unknown member: " + entry.name));
            }
        }
        return Outcome<void>{};
    }

    [[nodiscard]] Outcome<AssetMetadata> read_metadata(const internal::JsonValue& object) {
        if (object.kind != internal::JsonValue::Kind::Object) {
            return fail("metadata must be an object");
        }
        if (const auto failure = reject_unknown_members(object, {"display_name", "owner", "site", "notes", "labels"});
            !failure) {
            return failure.error();
        }
        AssetMetadata metadata;
        auto display_name = required_string(object, "display_name");
        if (!display_name) {
            return display_name.error();
        }
        if (!is_display_text(display_name.value(), bounds_.max_display_name_bytes)) {
            return fail("display_name is empty, too long, or is not structurally valid UTF-8");
        }
        metadata.display_name = std::move(display_name).value();

        auto owner = optional_string(object, "owner");
        if (!owner) {
            return owner.error();
        }
        if (owner.value().has_value()) {
            const auto parsed = OwnerId::create(*owner.value());
            if (!parsed.has_value()) {
                return fail("owner is not a valid organisation identifier");
            }
            metadata.owner = *parsed;
        }
        auto site = optional_string(object, "site");
        if (!site) {
            return site.error();
        }
        if (site.value().has_value()) {
            const auto parsed = LocationId::create(*site.value());
            if (!parsed.has_value()) {
                return fail("site is not a valid location identifier");
            }
            metadata.site = *parsed;
        }
        auto notes = optional_string(object, "notes");
        if (!notes) {
            return notes.error();
        }
        if (notes.value().has_value()) {
            if (notes.value()->size() > limits::kMaxNotesBytes ||
                validate_utf8(*notes.value(), true) != Utf8Status::Valid) {
                return fail("notes are too long or are not structurally valid UTF-8");
            }
            metadata.notes = *notes.value();
        }

        const internal::JsonValue* labels = member(object, "labels");
        if (labels != nullptr && labels->kind != internal::JsonValue::Kind::Null) {
            if (labels->kind != internal::JsonValue::Kind::Array) {
                return fail("labels must be an array");
            }
            if (labels->elements.size() > bounds_.max_labels_per_asset) {
                return fail("labels exceed the configured bound");
            }
            for (const internal::JsonValuePtr& element : labels->elements) {
                if (element->kind != internal::JsonValue::Kind::Object) {
                    return fail("each label must be an object");
                }
                if (const auto failure = reject_unknown_members(*element, {"key", "value"}); !failure) {
                    return failure.error();
                }
                auto key = required_string(*element, "key");
                if (!key) {
                    return key.error();
                }
                if (!is_label_key(key.value())) {
                    return fail("label key violates label syntax: " + key.value());
                }
                auto value = required_string(*element, "value");
                if (!value) {
                    return value.error();
                }
                if (value.value().size() > bounds_.max_label_value_bytes ||
                    validate_utf8(value.value(), true) != Utf8Status::Valid) {
                    return fail("label value violates text rules for key: " + key.value());
                }
                metadata.labels.emplace_back(std::move(key).value(), std::move(value).value());
            }
        }
        return metadata;
    }

    [[nodiscard]] Outcome<SerialIdentity> read_serial(const internal::JsonValue& object) {
        if (object.kind != internal::JsonValue::Kind::Object) {
            return fail("serial_identity must be an object");
        }
        if (const auto failure = reject_unknown_members(object, {"manufacturer", "serial", "model"}); !failure) {
            return failure.error();
        }
        auto manufacturer = required_string(object, "manufacturer");
        if (!manufacturer) {
            return manufacturer.error();
        }
        auto serial = required_string(object, "serial");
        if (!serial) {
            return serial.error();
        }
        auto model = optional_string(object, "model");
        if (!model) {
            return model.error();
        }
        const auto manufacturer_id = ManufacturerIdentity::create(manufacturer.value());
        const auto serial_id = SerialNumber::create(serial.value());
        if (!manufacturer_id.has_value()) {
            return fail("manufacturer is empty, too long, or is not structurally valid UTF-8");
        }
        if (!serial_id.has_value()) {
            return fail("serial is empty, too long, or is not structurally valid UTF-8");
        }
        std::optional<ModelIdentity> model_id;
        if (model.value().has_value()) {
            model_id = ModelIdentity::create(*model.value());
            if (!model_id.has_value()) {
                return fail("model is empty, too long, or is not structurally valid UTF-8");
            }
        }
        const auto identity = SerialIdentity::create(*manufacturer_id, *serial_id, model_id);
        if (!identity.has_value()) {
            return fail("serial identity could not be constructed");
        }
        return identity.value();
    }

    [[nodiscard]] Outcome<Reference> read_reference(const internal::JsonValue& object) {
        if (object.kind != internal::JsonValue::Kind::Object) {
            return fail("each reference must be an object");
        }
        if (const auto failure = reject_unknown_members(object, {"kind", "evidence", "target"}); !failure) {
            return failure.error();
        }
        auto kind = required_string(object, "kind");
        if (!kind) {
            return kind.error();
        }
        auto evidence_text = required_string(object, "evidence");
        if (!evidence_text) {
            return evidence_text.error();
        }
        const auto evidence = reference_evidence_from_token(evidence_text.value());
        if (!evidence.has_value()) {
            return fail("reference evidence is outside the permitted domain: " + evidence_text.value());
        }
        auto target = required_string(object, "target");
        if (!target) {
            return target.error();
        }
        if (kind.value() == "capability") {
            const auto parsed = CapabilityReference::create(target.value());
            if (!parsed.has_value()) {
                return fail("capability reference is malformed: " + target.value());
            }
            return Reference::capability(*parsed, *evidence);
        }
        if (kind.value() == "location") {
            const auto parsed = LocationId::create(target.value());
            if (!parsed.has_value()) {
                return fail("location reference is malformed: " + target.value());
            }
            return Reference::location(*parsed, *evidence);
        }
        if (kind.value() == "rack") {
            const auto parsed = RackId::create(target.value());
            if (!parsed.has_value()) {
                return fail("rack reference is malformed: " + target.value());
            }
            return Reference::rack(*parsed, *evidence);
        }
        if (kind.value() == "external_object") {
            const std::size_t separator = target.value().find('/');
            if (separator == std::string::npos) {
                return fail("external object references are written as kind/id: " + target.value());
            }
            const auto parsed =
                ExternalObjectReference::create(target.value().substr(0, separator), target.value().substr(separator + 1));
            if (!parsed.has_value()) {
                return fail("external object reference is malformed: " + target.value());
            }
            return Reference::external_object(*parsed, *evidence);
        }
        return fail("reference kind is outside the permitted domain: " + kind.value());
    }

    [[nodiscard]] Outcome<AssetState> read_state(const internal::JsonValue& object) {
        if (object.kind != internal::JsonValue::Kind::Object) {
            return fail("state must be an object");
        }
        if (const auto failure = reject_unknown_members(object, {"lifecycle", "installation"}); !failure) {
            return failure.error();
        }
        auto lifecycle_text = required_string(object, "lifecycle");
        if (!lifecycle_text) {
            return lifecycle_text.error();
        }
        auto installation_text = required_string(object, "installation");
        if (!installation_text) {
            return installation_text.error();
        }
        const auto lifecycle = lifecycle_state_from_token(lifecycle_text.value());
        if (!lifecycle.has_value()) {
            return fail("lifecycle state is outside the permitted domain: " + lifecycle_text.value());
        }
        const auto installation = installation_state_from_token(installation_text.value());
        if (!installation.has_value()) {
            return fail("installation state is outside the permitted domain: " + installation_text.value());
        }
        AssetState state;
        state.lifecycle = *lifecycle;
        state.installation = *installation;
        return state;
    }

    [[nodiscard]] Outcome<ImportedAsset> read_asset(const internal::JsonValue& object) {
        if (object.kind != internal::JsonValue::Kind::Object) {
            return fail("each asset must be an object");
        }
        if (const auto failure = reject_unknown_members(
                object, {"asset_id", "asset_class", "generation", "revision", "last_sequence", "serial_identity",
                         "serial_key", "metadata", "state", "references", "supersedes", "provenance", "history"});
            !failure) {
            return failure.error();
        }
        ImportedAsset imported;
        auto id_text = required_string(object, "asset_id");
        if (!id_text) {
            return id_text.error();
        }
        const auto id = AssetId::parse(id_text.value());
        if (!id.has_value()) {
            return fail("asset_id is not in canonical 8-4-4-4-12 lowercase hexadecimal form: " + id_text.value());
        }
        imported.id = *id;
        if (imported.id.is_nil()) {
            return fail("asset_id is the nil identifier");
        }

        auto class_text = required_string(object, "asset_class");
        if (!class_text) {
            return class_text.error();
        }
        const auto asset_class = asset_class_from_token(class_text.value());
        if (!asset_class.has_value() || !is_known(*asset_class)) {
            return fail("asset_class is outside the permitted domain: " + class_text.value());
        }
        imported.asset_class = *asset_class;

        const internal::JsonValue* serial = member(object, "serial_identity");
        if (serial == nullptr) {
            return fail("required member is missing: serial_identity");
        }
        auto identity = read_serial(*serial);
        if (!identity) {
            return identity.error();
        }
        imported.serial_identity = std::move(identity).value();

        if (const internal::JsonValue* key = member(object, "serial_key"); key != nullptr) {
            if (key->kind != internal::JsonValue::Kind::String) {
                return fail("serial_key must be a string");
            }
            if (key->text != imported.serial_identity.canonical_key()) {
                return fail("serial_key does not match the supplied serial identity");
            }
        }

        const internal::JsonValue* metadata = member(object, "metadata");
        if (metadata == nullptr) {
            return fail("required member is missing: metadata");
        }
        auto read_metadata_value = read_metadata(*metadata);
        if (!read_metadata_value) {
            return read_metadata_value.error();
        }
        imported.metadata = std::move(read_metadata_value).value();

        const internal::JsonValue* state = member(object, "state");
        if (state == nullptr) {
            return fail("required member is missing: state");
        }
        auto read_state_value = read_state(*state);
        if (!read_state_value) {
            return read_state_value.error();
        }
        imported.state = read_state_value.value();

        const internal::JsonValue* references = member(object, "references");
        if (references != nullptr && references->kind != internal::JsonValue::Kind::Null) {
            if (references->kind != internal::JsonValue::Kind::Array) {
                return fail("references must be an array");
            }
            if (references->elements.size() > bounds_.max_references_per_asset) {
                return fail("references exceed the configured bound");
            }
            for (const internal::JsonValuePtr& element : references->elements) {
                auto reference = read_reference(*element);
                if (!reference) {
                    return reference.error();
                }
                imported.references.push_back(std::move(reference).value());
            }
        }

        const internal::JsonValue* supersedes = member(object, "supersedes");
        if (supersedes != nullptr && supersedes->kind != internal::JsonValue::Kind::Null) {
            if (supersedes->kind != internal::JsonValue::Kind::Object) {
                return fail("supersedes must be an object or null");
            }
            if (const auto failure = reject_unknown_members(*supersedes,
                                                            {"asset_id", "generation", "final_revision", "cause",
                                                             "linked_at", "note"});
                !failure) {
                return failure.error();
            }
            auto predecessor_text = required_string(*supersedes, "asset_id");
            if (!predecessor_text) {
                return predecessor_text.error();
            }
            const auto predecessor = AssetId::parse(predecessor_text.value());
            if (!predecessor.has_value()) {
                return fail("supersedes.asset_id is not a canonical identifier");
            }
            imported.supersedes = *predecessor;
            if (const internal::JsonValue* cause = member(*supersedes, "cause"); cause != nullptr) {
                if (cause->kind != internal::JsonValue::Kind::String) {
                    return fail("supersedes.cause must be a string");
                }
                const auto parsed = replacement_cause_from_token(cause->text);
                if (!parsed.has_value()) {
                    return fail("supersedes.cause is outside the permitted domain: " + cause->text);
                }
                imported.supersession_cause = *parsed;
            }
        }

        const internal::JsonValue* observed = member(object, "observed_at");
        if (observed != nullptr && observed->kind == internal::JsonValue::Kind::String) {
            const auto parsed = Timestamp::parse_rfc3339(observed->text);
            if (!parsed.has_value()) {
                return fail("observed_at is not an RFC 3339 UTC timestamp");
            }
            imported.observed_at = *parsed;
        }
        return imported;
    }

    const internal::JsonValue& root_;
    const RegistryLimits& bounds_;
    std::size_t index_ = 0;
};

}  // namespace

std::string_view to_string(ExportFormat format) noexcept {
    switch (format) {
        case ExportFormat::CanonicalJson:
            return "canonical_json";
        case ExportFormat::CompactJson:
            return "compact_json";
        case ExportFormat::NewlineDelimitedJson:
            return "newline_delimited_json";
    }
    return "canonical_json";
}

std::optional<ExportFormat> export_format_from_token(std::string_view token) noexcept {
    if (token == "canonical_json") {
        return ExportFormat::CanonicalJson;
    }
    if (token == "compact_json") {
        return ExportFormat::CompactJson;
    }
    if (token == "newline_delimited_json") {
        return ExportFormat::NewlineDelimitedJson;
    }
    return std::nullopt;
}

std::string_view to_string(ImportConflictPolicy policy) noexcept {
    switch (policy) {
        case ImportConflictPolicy::Reject:
            return "reject";
        case ImportConflictPolicy::SkipExisting:
            return "skip_existing";
        case ImportConflictPolicy::UpdateExisting:
            return "update_existing";
    }
    return "reject";
}

std::optional<ImportConflictPolicy> import_conflict_policy_from_token(std::string_view token) noexcept {
    if (token == "reject") {
        return ImportConflictPolicy::Reject;
    }
    if (token == "skip_existing") {
        return ImportConflictPolicy::SkipExisting;
    }
    if (token == "update_existing") {
        return ImportConflictPolicy::UpdateExisting;
    }
    return std::nullopt;
}

std::string_view to_string(ImportErrorPolicy policy) noexcept {
    switch (policy) {
        case ImportErrorPolicy::RejectRecord:
            return "reject_record";
        case ImportErrorPolicy::RejectBatch:
            return "reject_batch";
    }
    return "reject_record";
}

std::optional<ImportErrorPolicy> import_error_policy_from_token(std::string_view token) noexcept {
    if (token == "reject_record") {
        return ImportErrorPolicy::RejectRecord;
    }
    if (token == "reject_batch") {
        return ImportErrorPolicy::RejectBatch;
    }
    return std::nullopt;
}

Outcome<std::string> export_snapshot(const Snapshot& snapshot, const ExportOptions& options, ExportReport& report) {
    if (!snapshot.valid()) {
        return make_error(ErrorCode::InvalidInput, "cannot export an uninitialised snapshot");
    }
    if (options.schema_version != kExportSchemaVersion) {
        return make_error(ErrorCode::ExportFormatUnsupported,
                          "export schema version " + std::to_string(options.schema_version) +
                              " is not implemented by this build (supported: " +
                              std::to_string(kExportSchemaVersion) + ")");
    }
    if (options.indent > 16) {
        return make_error(ErrorCode::InvalidInput, "export indentation must be between 0 and 16 spaces");
    }
    const std::uint64_t bound = options.max_records == 0 ? snapshot.bounds().max_export_records : options.max_records;
    if (snapshot.size() > bound) {
        return make_error(ErrorCode::CapacityExceeded,
                          "the snapshot holds " + std::to_string(snapshot.size()) +
                              " assets, which exceeds the export bound of " + std::to_string(bound));
    }
    report = ExportReport{};
    report.format = options.format;
    if (options.format == ExportFormat::NewlineDelimitedJson) {
        // One asset per line, in canonical AssetId order, so a consumer can stream
        // the document without holding it.
        std::string document;
        std::uint64_t emitted = 0;
        internal::JsonWriter line(0);
        snapshot.for_each([&](const AssetRecord& record) -> bool {
            line.reset();
            write_asset(line, record, false);
            document += line.str();
            document.push_back('\n');
            ++emitted;
            return true;
        });
        report.records_exported = emitted;
        report.published_sequence = snapshot.published_sequence();
        report.epoch = snapshot.epoch();
        report.bytes_written = document.size();
        return document;
    }
    const std::string document = render_document(snapshot, options, bound, report);
    return document;
}

Outcome<std::vector<ImportedAsset>> parse_import_json(std::string_view document, const RegistryLimits& bounds) {
    if (document.empty()) {
        return make_error(ErrorCode::ImportTruncated, "the import document is empty");
    }
    if (document.size() > bounds.max_document_bytes) {
        return make_error(ErrorCode::ImportTruncated,
                          "the import document exceeds the configured byte bound of " +
                              std::to_string(bounds.max_document_bytes));
    }
    auto parsed = internal::parse_json(document, bounds.max_traversal_depth, bounds.max_import_records * 64ULL + 64ULL);
    if (!parsed) {
        return parsed.error();
    }
    DocumentReader reader(*parsed.value(), bounds);
    return reader.read();
}

Outcome<ImportedAsset> parse_import_asset_json(std::string_view object, const RegistryLimits& bounds) {
    if (object.empty()) {
        return make_error(ErrorCode::ImportTruncated, "the asset document is empty");
    }
    if (object.size() > bounds.max_document_bytes) {
        return make_error(ErrorCode::ImportTruncated,
                          "the asset document exceeds the configured byte bound of " +
                              std::to_string(bounds.max_document_bytes));
    }
    auto parsed = internal::parse_json(object, bounds.max_traversal_depth, 4096);
    if (!parsed) {
        return parsed.error();
    }
    DocumentReader reader(*parsed.value(), bounds);
    return reader.read_one();
}

}  // namespace asset_registry
