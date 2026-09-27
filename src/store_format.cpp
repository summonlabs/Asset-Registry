// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Internal: durable payload codec.
//
// Encoding rules that make the format a contract rather than an implementation
// detail:
//   * Records are emitted in strictly ascending canonical AssetId order.
//   * Reference and label sequences are emitted in ascending canonical order.
//   * Enumerations are encoded by numeric value with the domain checked on
//     decode; an out-of-domain value is a hard failure, never a fallback.
//   * Every list is preceded by its element count, and the decoder validates the
//     count against the remaining payload before allocating.
//
// Decoding applies the same record predicate the mutation API applies, so a
// payload that this build could not have produced is rejected rather than loaded.

#include "store_format.hpp"

#include <algorithm>
#include <cstring>

#include "asset_registry/text.hpp"
#include "crc32.hpp"
#include "tlv.hpp"

namespace asset_registry::internal {
namespace {

constexpr char kMagic[8] = {'A', 'S', 'S', 'E', 'T', 'R', 'G', 'Y'};

/// Number of members the payload root list carries: sequence, epoch, last writer,
/// predecessor sequence, predecessor checksum, policy, bounds, records, and writers.
/// Every one of them is mandatory, so the root list holds exactly this many elements
/// and a root that declares any other number is not a payload this build produced.
constexpr std::uint64_t kPayloadRootMembers = 9;

constexpr std::size_t kOffsetMagic = 0;
constexpr std::size_t kOffsetFormat = 8;
constexpr std::size_t kOffsetSchema = 12;
constexpr std::size_t kOffsetSequence = 16;
constexpr std::size_t kOffsetEpoch = 24;
constexpr std::size_t kOffsetPayloadBytes = 32;
constexpr std::size_t kOffsetHeaderCrc = 40;

[[nodiscard]] std::string tlv_failure(std::string_view where, TlvStatus status) {
    std::string message("durable payload is not decodable at ");
    message += where;
    message += ": ";
    message += to_string(status);
    return message;
}

/// Reads the fixed sequence of members a record-level container declares. The count
/// comes from the encoded list header, so a container that is missing a member, has
/// one too many, or nests a differently shaped element is rejected here rather than
/// silently producing a partially populated value.
[[nodiscard]] bool at_end(const TlvReader& reader) {
    return !reader.ok();
}

[[nodiscard]] std::string tlv_failure(std::string_view where, const TlvReader& reader) {
    std::string message = tlv_failure(where, reader.status());
    message += " (offset ";
    message += std::to_string(reader.offset());
    message += ", depth ";
    message += std::to_string(reader.depth());
    message += ", tag ";
    message += std::to_string(reader.tag());
    message += ")";
    return message;
}

[[nodiscard]] std::string record_failure(std::size_t index, const std::string& detail) {
    std::string message("durable payload asset record ");
    message += std::to_string(index);
    message += " is invalid: ";
    message += detail;
    return message;
}

// ---------------------------------------------------------------------------
// Scalar codecs
// ---------------------------------------------------------------------------

void encode_asset_id(TlvWriter& writer, std::uint64_t tag_value, const AssetId& id) {
    writer.append_bytes(tag_value, id.bytes().data(), id.bytes().size());
}

[[nodiscard]] bool decode_asset_id(TlvReader& reader, std::uint64_t tag_value, AssetId& id) {
    if (!reader.expect(tag_value)) {
        return false;
    }
    std::vector<std::uint8_t> bytes;
    if (!reader.read_bytes(bytes)) {
        return false;
    }
    if (bytes.size() != 16) {
        return false;
    }
    AssetId::bytes_type raw{};
    std::copy(bytes.begin(), bytes.end(), raw.begin());
    id = AssetId::from_bytes(raw);
    return true;
}

[[nodiscard]] bool decode_enum(TlvReader& reader, std::uint64_t tag_value, std::uint64_t maximum,
                               std::uint64_t& value) {
    if (!reader.expect(tag_value)) {
        return false;
    }
    return reader.read_unsigned_checked(maximum, value);
}

void encode_optional_string(TlvWriter& writer, std::uint64_t present_tag, std::uint64_t value_tag,
                            const std::optional<std::string>& value) {
    writer.append_bool(present_tag, value.has_value());
    if (value.has_value()) {
        writer.append_string(value_tag, *value);
    }
}

[[nodiscard]] bool decode_optional_string(TlvReader& reader, std::uint64_t present_tag, std::uint64_t value_tag,
                                          std::optional<std::string>& value) {
    bool present = false;
    if (!reader.next() || !reader.expect(present_tag) || !reader.read_bool(present)) {
        return false;
    }
    value.reset();
    if (!present) {
        return true;
    }
    if (!reader.next() || !reader.expect(value_tag)) {
        return false;
    }
    std::string text;
    if (!reader.read_string(text)) {
        return false;
    }
    value = std::move(text);
    return true;
}

/// Encodes a serial identity as manufacturer / serial / optional model.
void encode_serial_identity(TlvWriter& writer, std::uint64_t manufacturer_tag, std::uint64_t serial_tag,
                            std::uint64_t model_present_tag, std::uint64_t model_tag,
                            const SerialIdentity& identity) {
    writer.append_string(manufacturer_tag, identity.manufacturer().name());
    writer.append_string(serial_tag, identity.serial().text());
    encode_optional_string(writer, model_present_tag, model_tag,
                           identity.model().has_value() ? std::optional<std::string>(identity.model()->text())
                                                        : std::nullopt);
}

[[nodiscard]] bool decode_serial_identity(TlvReader& reader, std::uint64_t manufacturer_tag, std::uint64_t serial_tag,
                                          std::uint64_t model_present_tag, std::uint64_t model_tag,
                                          SerialIdentity& identity) {
    std::string manufacturer_text;
    std::string serial_text;
    if (!reader.next() || !reader.expect(manufacturer_tag) || !reader.read_string(manufacturer_text)) {
        return false;
    }
    if (!reader.next() || !reader.expect(serial_tag) || !reader.read_string(serial_text)) {
        return false;
    }
    std::optional<std::string> model_text;
    if (!decode_optional_string(reader, model_present_tag, model_tag, model_text)) {
        return false;
    }
    const auto manufacturer = ManufacturerIdentity::create(manufacturer_text);
    const auto serial = SerialNumber::create(serial_text);
    if (!manufacturer.has_value() || !serial.has_value()) {
        return false;
    }
    std::optional<ModelIdentity> model;
    if (model_text.has_value()) {
        model = ModelIdentity::create(*model_text);
        if (!model.has_value()) {
            return false;
        }
    }
    const auto combined = SerialIdentity::create(*manufacturer, *serial, model);
    if (!combined.has_value()) {
        return false;
    }
    identity = *combined;
    return true;
}

// ---------------------------------------------------------------------------
// Reference codec
// ---------------------------------------------------------------------------

void encode_reference(TlvWriter& writer, const Reference& reference) {
    writer.open_list(tag::kReference, 4);
    writer.append_unsigned(tag::kReferenceKind, static_cast<std::uint64_t>(reference.kind()));
    writer.append_unsigned(tag::kReferenceEvidence, static_cast<std::uint64_t>(reference.evidence()));
    switch (reference.kind()) {
        case Reference::Kind::Capability:
            writer.append_string(tag::kReferenceNamespace, reference.capability().name_space().text());
            writer.append_string(tag::kReferenceTarget, reference.capability().path());
            break;
        case Reference::Kind::Location:
            writer.append_string(tag::kReferenceTarget, reference.location().text());
            break;
        case Reference::Kind::Rack:
            writer.append_string(tag::kReferenceTarget, reference.rack().text());
            break;
        case Reference::Kind::ExternalObject:
            writer.append_string(tag::kReferenceExternalKind, reference.external_object().kind());
            writer.append_string(tag::kReferenceTarget, reference.external_object().id());
            break;
    }
    writer.close_list();
}

[[nodiscard]] bool decode_reference(TlvReader& reader, Reference& reference) {
    std::uint64_t count = 0;
    if (!reader.read_list(8, count)) {
        return false;
    }
    if (count < 3) {
        return false;
    }
    std::uint64_t kind = 0;
    if (!reader.next() || !decode_enum(reader, tag::kReferenceKind, 3, kind)) {
        return false;
    }
    std::uint64_t evidence = 0;
    if (!reader.next() || !decode_enum(reader, tag::kReferenceEvidence, 2, evidence)) {
        return false;
    }

    std::string target;
    std::string name_space;
    std::string external_kind;
    bool have_target = false;
    bool have_namespace = false;
    bool have_external_kind = false;
    for (std::uint64_t seen = 2; seen < count; ++seen) {
        if (!reader.next()) {
            return false;
        }
        switch (reader.tag()) {
            case tag::kReferenceTarget:
                if (have_target || !reader.read_string(target)) {
                    return false;
                }
                have_target = true;
                break;
            case tag::kReferenceNamespace:
                if (have_namespace || !reader.read_string(name_space)) {
                    return false;
                }
                have_namespace = true;
                break;
            case tag::kReferenceExternalKind:
                if (have_external_kind || !reader.read_string(external_kind)) {
                    return false;
                }
                have_external_kind = true;
                break;
            default:
                return false;
        }
    }
    reader.leave_list();

    const auto decoded_evidence = static_cast<ReferenceEvidence>(evidence);
    switch (static_cast<Reference::Kind>(kind)) {
        case Reference::Kind::Capability: {
            if (!have_target || !have_namespace || have_external_kind) {
                return false;
            }
            const auto name_space_id = IdentityNamespace::create(name_space);
            if (!name_space_id.has_value()) {
                return false;
            }
            const auto capability = CapabilityReference::create(*name_space_id, target);
            if (!capability.has_value()) {
                return false;
            }
            reference = Reference::capability(*capability, decoded_evidence);
            return true;
        }
        case Reference::Kind::Location: {
            if (!have_target || have_namespace || have_external_kind) {
                return false;
            }
            const auto location = LocationId::create(target);
            if (!location.has_value()) {
                return false;
            }
            reference = Reference::location(*location, decoded_evidence);
            return true;
        }
        case Reference::Kind::Rack: {
            if (!have_target || have_namespace || have_external_kind) {
                return false;
            }
            const auto rack = RackId::create(target);
            if (!rack.has_value()) {
                return false;
            }
            reference = Reference::rack(*rack, decoded_evidence);
            return true;
        }
        case Reference::Kind::ExternalObject: {
            if (!have_target || !have_external_kind || have_namespace) {
                return false;
            }
            const auto external = ExternalObjectReference::create(external_kind, target);
            if (!external.has_value()) {
                return false;
            }
            reference = Reference::external_object(*external, decoded_evidence);
            return true;
        }
    }
    return false;
}

void encode_labels(TlvWriter& writer, std::uint64_t list_tag, std::uint64_t element_tag,
                   const std::vector<std::pair<std::string, std::string>>& labels) {
    writer.open_list(list_tag, labels.size());
    for (const auto& label : labels) {
        writer.open_list(element_tag, 2);
        writer.append_string(tag::kLabelKey, label.first);
        writer.append_string(tag::kLabelValue, label.second);
        writer.close_list();
    }
    writer.close_list();
}

[[nodiscard]] bool decode_labels(TlvReader& reader, std::uint64_t list_tag, std::uint64_t element_tag,
                                 std::uint32_t max_labels, std::size_t max_label_bytes,
                                 std::vector<std::pair<std::string, std::string>>& labels) {
    if (!reader.next() || !reader.expect(list_tag)) {
        return false;
    }
    std::uint64_t count = 0;
    if (!reader.read_list(max_labels, count)) {
        return false;
    }
    labels.clear();
    labels.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        if (!reader.next() || !reader.expect(element_tag)) {
            return false;
        }
        std::uint64_t inner = 0;
        if (!reader.read_list(2, inner) || inner != 2) {
            return false;
        }
        if (!reader.next() || !reader.expect(tag::kLabelKey)) {
            return false;
        }
        std::string key;
        if (!reader.read_string(key)) {
            return false;
        }
        if (!reader.next() || !reader.expect(tag::kLabelValue)) {
            return false;
        }
        std::string value;
        if (!reader.read_string(value)) {
            return false;
        }
        if (!is_label_key(key) || value.size() > max_label_bytes) {
            return false;
        }
        labels.emplace_back(std::move(key), std::move(value));
    }
    reader.leave_list();
    return true;
}

void encode_references(TlvWriter& writer, std::uint64_t list_tag, const std::vector<Reference>& references) {
    writer.open_list(list_tag, references.size());
    for (const Reference& reference : references) {
        encode_reference(writer, reference);
    }
    writer.close_list();
}

[[nodiscard]] bool decode_references(TlvReader& reader, std::uint64_t list_tag, std::uint32_t max_references,
                                     std::vector<Reference>& references) {
    if (!reader.next() || !reader.expect(list_tag)) {
        return false;
    }
    std::uint64_t count = 0;
    if (!reader.read_list(max_references, count)) {
        return false;
    }
    references.clear();
    references.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        if (!reader.next()) {
            return false;
        }
        Reference reference;
        if (!decode_reference(reader, reference)) {
            return false;
        }
        references.push_back(std::move(reference));
    }
    reader.leave_list();
    return true;
}

// ---------------------------------------------------------------------------
// Metadata codec
// ---------------------------------------------------------------------------

void encode_metadata(TlvWriter& writer, const AssetMetadata& metadata, std::uint64_t display_name_tag,
                     std::uint64_t owner_present_tag, std::uint64_t owner_tag, std::uint64_t site_present_tag,
                     std::uint64_t site_tag, std::uint64_t notes_tag, std::uint64_t labels_tag,
                     std::uint64_t label_tag) {
    writer.append_string(display_name_tag, metadata.display_name);
    encode_optional_string(writer, owner_present_tag, owner_tag,
                           metadata.owner.has_value() ? std::optional<std::string>(metadata.owner->text())
                                                      : std::nullopt);
    encode_optional_string(writer, site_present_tag, site_tag,
                           metadata.site.has_value() ? std::optional<std::string>(metadata.site->text())
                                                     : std::nullopt);
    writer.append_string(notes_tag, metadata.notes);
    encode_labels(writer, labels_tag, label_tag, metadata.labels);
}

// ---------------------------------------------------------------------------
// Provenance codec
// ---------------------------------------------------------------------------

void encode_provenance(TlvWriter& writer, const std::vector<ProvenanceStep>& steps) {
    writer.open_list(tag::kRecordProvenance, steps.size());
    for (const ProvenanceStep& step : steps) {
        writer.open_list(tag::kProvenanceStep, 10);
        writer.append_unsigned(tag::kProvenanceSequence, step.sequence.value());
        writer.append_unsigned(tag::kProvenanceEpoch, step.epoch.value());
        writer.append_unsigned(tag::kProvenanceRevision, step.revision.value());
        writer.append_unsigned(tag::kProvenanceAction, static_cast<std::uint64_t>(step.action));
        writer.append_unsigned(tag::kProvenanceOrigin, static_cast<std::uint64_t>(step.origin));
        writer.append_string(tag::kProvenanceActor, step.actor.to_string());
        writer.append_bool(tag::kProvenanceTimestampPresent, step.recorded_at.has_value());
        if (step.recorded_at.has_value()) {
            writer.append_string(tag::kProvenanceTimestamp, step.recorded_at->to_rfc3339());
        }
        writer.append_string(tag::kProvenanceReason, step.reason);
        writer.append_string(tag::kProvenanceChange, step.change);
        writer.close_list();
    }
    writer.close_list();
}

[[nodiscard]] bool decode_provenance(TlvReader& reader, std::uint64_t list_tag, std::uint32_t max_steps,
                                     std::vector<ProvenanceStep>& steps) {
    if (!reader.next() || !reader.expect(list_tag)) {
        return false;
    }
    std::uint64_t count = 0;
    if (!reader.read_list(max_steps, count)) {
        return false;
    }
    steps.clear();
    steps.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        if (!reader.next() || !reader.expect(tag::kProvenanceStep)) {
            return false;
        }
        std::uint64_t inner = 0;
        if (!reader.read_list(10, inner)) {
            return false;
        }
        ProvenanceStep step;
        std::uint64_t sequence = 0;
        std::uint64_t epoch = 0;
        std::uint64_t revision = 0;
        std::uint64_t action = 0;
        std::uint64_t origin = 0;
        std::string actor;
        bool timestamp_present = false;
        std::string timestamp;
        std::string reason;
        std::string change;
        if (!reader.next() || !reader.expect(tag::kProvenanceSequence) ||
            !reader.read_unsigned(sequence)) {
            return false;
        }
        if (!reader.next() || !reader.expect(tag::kProvenanceEpoch) || !reader.read_unsigned(epoch)) {
            return false;
        }
        if (!reader.next() || !reader.expect(tag::kProvenanceRevision) || !reader.read_unsigned(revision)) {
            return false;
        }
        if (!reader.next() || !decode_enum(reader, tag::kProvenanceAction, 14, action)) {
            return false;
        }
        if (!reader.next() || !decode_enum(reader, tag::kProvenanceOrigin, 3, origin)) {
            return false;
        }
        if (!reader.next() || !reader.expect(tag::kProvenanceActor) || !reader.read_string(actor)) {
            return false;
        }
        if (!reader.next() || !reader.expect(tag::kProvenanceTimestampPresent) ||
            !reader.read_bool(timestamp_present)) {
            return false;
        }
        if (timestamp_present) {
            if (!reader.next() || !reader.expect(tag::kProvenanceTimestamp) || !reader.read_string(timestamp)) {
                return false;
            }
        }
        if (!reader.next() || !reader.expect(tag::kProvenanceReason) || !reader.read_string(reason)) {
            return false;
        }
        if (!reader.next() || !reader.expect(tag::kProvenanceChange) || !reader.read_string(change)) {
            return false;
        }

        step.sequence = TransactionSequence(sequence);
        step.epoch = RegistryEpoch(epoch);
        step.revision = AssetRevision(revision);
        step.action = static_cast<ProvenanceAction>(action);
        step.origin = static_cast<ProvenanceOrigin>(origin);
        if (actor.empty() || actor == "system") {
            step.actor = ActorRef::system();
        } else {
            const auto writer = WriterId::create(actor);
            if (!writer.has_value()) {
                return false;
            }
            step.actor = ActorRef::writer(*writer);
        }
        if (timestamp_present) {
            const auto parsed = Timestamp::parse_rfc3339(timestamp);
            if (!parsed.has_value()) {
                return false;
            }
            step.recorded_at = *parsed;
        }
        step.reason = std::move(reason);
        step.change = std::move(change);
        steps.push_back(std::move(step));
    }
    reader.leave_list();
    return true;
}

// ---------------------------------------------------------------------------
// Incarnation codec
// ---------------------------------------------------------------------------

[[nodiscard]] bool decode_incarnation(TlvReader& reader, const RegistryLimits& limits, AssetIncarnation& incarnation) {
    std::uint64_t count = 0;
    if (!reader.read_list(24, count)) {
        return false;
    }
    std::uint64_t generation = 0;
    std::uint64_t asset_class = 0;
    std::uint64_t lifecycle = 0;
    std::uint64_t installation = 0;
    std::uint64_t revision = 0;
    std::uint64_t closed_at = 0;
    bool closed_time_present = false;
    std::string closed_time;

    if (!reader.next() || !decode_enum(reader, tag::kIncarnationGeneration, 0xFFFFFFFFULL, generation)) {
        return false;
    }
    if (!reader.next() || !decode_enum(reader, tag::kIncarnationClass, 23, asset_class)) {
        return false;
    }
    // A member helper advances to its own first element, so no next() is taken here: doing both
    // would step over the manufacturer and read the serial where the manufacturer belongs.
    SerialIdentity identity;
    if (!decode_serial_identity(reader, tag::kIncarnationManufacturer, tag::kIncarnationSerial,
                                tag::kIncarnationModelPresent, tag::kIncarnationModel, identity)) {
        return false;
    }
    std::string display_name;
    if (!reader.next() || !reader.expect(tag::kIncarnationDisplayName) || !reader.read_string(display_name)) {
        return false;
    }
    std::optional<std::string> owner;
    if (!decode_optional_string(reader, tag::kIncarnationOwnerPresent, tag::kIncarnationOwner, owner)) {
        return false;
    }
    std::optional<std::string> site;
    if (!decode_optional_string(reader, tag::kIncarnationSitePresent, tag::kIncarnationSite, site)) {
        return false;
    }
    std::string notes;
    if (!reader.next() || !reader.expect(tag::kIncarnationNotes) || !reader.read_string(notes)) {
        return false;
    }
    std::vector<std::pair<std::string, std::string>> labels;
    if (!decode_labels(reader, tag::kIncarnationLabels, tag::kLabel, limits.max_labels_per_asset,
                       limits.max_label_value_bytes, labels)) {
        return false;
    }
    std::vector<Reference> references;
    if (!decode_references(reader, tag::kIncarnationReferences, limits.max_references_per_asset, references)) {
        return false;
    }
    if (!reader.next() || !decode_enum(reader, tag::kIncarnationLifecycle, 6, lifecycle)) {
        return false;
    }
    if (!reader.next() || !decode_enum(reader, tag::kIncarnationInstallation, 5, installation)) {
        return false;
    }
    if (!reader.next() || !reader.expect(tag::kIncarnationRevision) || !reader.read_unsigned(revision)) {
        return false;
    }
    if (!reader.next() || !reader.expect(tag::kIncarnationClosedAt) || !reader.read_unsigned(closed_at)) {
        return false;
    }
    if (!reader.next() || !reader.expect(tag::kIncarnationClosedTimePresent) ||
        !reader.read_bool(closed_time_present)) {
        return false;
    }
    if (closed_time_present) {
        if (!reader.next() || !reader.expect(tag::kIncarnationClosedTime) || !reader.read_string(closed_time)) {
            return false;
        }
    }
    reader.leave_list();

    incarnation.generation = AssetGeneration(static_cast<std::uint32_t>(generation));
    incarnation.asset_class = static_cast<AssetClass>(asset_class);
    incarnation.serial_identity = std::move(identity);
    incarnation.metadata.display_name = std::move(display_name);
    if (owner.has_value()) {
        const auto parsed = OwnerId::create(*owner);
        if (!parsed.has_value()) {
            return false;
        }
        incarnation.metadata.owner = *parsed;
    }
    if (site.has_value()) {
        const auto parsed = LocationId::create(*site);
        if (!parsed.has_value()) {
            return false;
        }
        incarnation.metadata.site = *parsed;
    }
    incarnation.metadata.notes = std::move(notes);
    incarnation.metadata.labels = std::move(labels);
    incarnation.references = std::move(references);
    incarnation.state.lifecycle = static_cast<LifecycleState>(lifecycle);
    incarnation.state.installation = static_cast<InstallationState>(installation);
    incarnation.final_revision = AssetRevision(revision);
    incarnation.closed_at = TransactionSequence(closed_at);
    if (closed_time_present) {
        const auto parsed = Timestamp::parse_rfc3339(closed_time);
        if (!parsed.has_value()) {
            return false;
        }
        incarnation.closed_time = *parsed;
    }
    return true;
}

void encode_incarnation(TlvWriter& writer, const AssetIncarnation& incarnation) {
    writer.open_list(tag::kIncarnation, 22);
    writer.append_unsigned(tag::kIncarnationGeneration, incarnation.generation.value());
    writer.append_unsigned(tag::kIncarnationClass, static_cast<std::uint64_t>(incarnation.asset_class));
    encode_serial_identity(writer, tag::kIncarnationManufacturer, tag::kIncarnationSerial,
                           tag::kIncarnationModelPresent, tag::kIncarnationModel, incarnation.serial_identity);
    encode_metadata(writer, incarnation.metadata, tag::kIncarnationDisplayName, tag::kIncarnationOwnerPresent,
                    tag::kIncarnationOwner, tag::kIncarnationSitePresent, tag::kIncarnationSite,
                    tag::kIncarnationNotes, tag::kIncarnationLabels, tag::kLabel);
    encode_references(writer, tag::kIncarnationReferences, incarnation.references);
    writer.append_unsigned(tag::kIncarnationLifecycle, static_cast<std::uint64_t>(incarnation.state.lifecycle));
    writer.append_unsigned(tag::kIncarnationInstallation, static_cast<std::uint64_t>(incarnation.state.installation));
    writer.append_unsigned(tag::kIncarnationRevision, incarnation.final_revision.value());
    writer.append_unsigned(tag::kIncarnationClosedAt, incarnation.closed_at.value());
    writer.append_bool(tag::kIncarnationClosedTimePresent, incarnation.closed_time.has_value());
    if (incarnation.closed_time.has_value()) {
        writer.append_string(tag::kIncarnationClosedTime, incarnation.closed_time->to_rfc3339());
    }
    writer.close_list();
}

// ---------------------------------------------------------------------------
// Record codec
// ---------------------------------------------------------------------------

void encode_record(TlvWriter& writer, const AssetRecord& record) {
    writer.open_list(tag::kRecord, 26);
    encode_asset_id(writer, tag::kRecordId, record.id);
    writer.append_unsigned(tag::kRecordClass, static_cast<std::uint64_t>(record.asset_class));
    writer.append_unsigned(tag::kRecordGeneration, record.generation.value());
    writer.append_unsigned(tag::kRecordRevision, record.revision.value());
    writer.append_unsigned(tag::kRecordLastSequence, record.last_sequence.value());
    encode_serial_identity(writer, tag::kRecordManufacturer, tag::kRecordSerial, tag::kRecordModelPresent,
                           tag::kRecordModel, record.serial_identity);
    encode_metadata(writer, record.metadata, tag::kRecordDisplayName, tag::kRecordOwnerPresent, tag::kRecordOwner,
                    tag::kRecordSitePresent, tag::kRecordSite, tag::kRecordNotes, tag::kRecordLabels, tag::kLabel);
    encode_references(writer, tag::kRecordReferences, record.references);
    writer.append_unsigned(tag::kRecordLifecycle, static_cast<std::uint64_t>(record.state.lifecycle));
    writer.append_unsigned(tag::kRecordInstallation, static_cast<std::uint64_t>(record.state.installation));
    writer.append_bool(tag::kRecordSupersedesPresent, record.supersedes.has_value());
    if (record.supersedes.has_value()) {
        const ReplacementLink& link = *record.supersedes;
        writer.open_list(tag::kSupersedes, 6);
        encode_asset_id(writer, tag::kSupersedePredecessor, link.predecessor);
        writer.append_unsigned(tag::kSupersedePredecessorGeneration, link.predecessor_generation.value());
        writer.append_unsigned(tag::kSupersedePredecessorRevision, link.predecessor_final_revision.value());
        writer.append_unsigned(tag::kSupersedeCause, static_cast<std::uint64_t>(link.cause));
        writer.append_unsigned(tag::kSupersedeLinkedAt, link.linked_at.value());
        writer.append_string(tag::kSupersedeNote, link.note);
        writer.close_list();
    }
    encode_provenance(writer, record.provenance);
    writer.open_list(tag::kRecordHistory, record.history.size());
    for (const AssetIncarnation& incarnation : record.history) {
        encode_incarnation(writer, incarnation);
    }
    writer.close_list();
    writer.close_list();
}

[[nodiscard]] bool decode_record(TlvReader& reader, const RegistryLimits& limits, AssetRecord& record) {
    std::uint64_t count = 0;
    if (!reader.read_list(32, count)) {
        return false;
    }
    if (!reader.next() || !decode_asset_id(reader, tag::kRecordId, record.id)) {
        return false;
    }
    std::uint64_t asset_class = 0;
    if (!reader.next() || !decode_enum(reader, tag::kRecordClass, 23, asset_class)) {
        return false;
    }
    record.asset_class = static_cast<AssetClass>(asset_class);
    std::uint64_t generation = 0;
    if (!reader.next() || !decode_enum(reader, tag::kRecordGeneration, 0xFFFFFFFFULL, generation)) {
        return false;
    }
    record.generation = AssetGeneration(static_cast<std::uint32_t>(generation));
    std::uint64_t revision = 0;
    if (!reader.next() || !reader.expect(tag::kRecordRevision) || !reader.read_unsigned(revision)) {
        return false;
    }
    record.revision = AssetRevision(revision);
    std::uint64_t last_sequence = 0;
    if (!reader.next() || !reader.expect(tag::kRecordLastSequence) || !reader.read_unsigned(last_sequence)) {
        return false;
    }
    record.last_sequence = TransactionSequence(last_sequence);

    // A member helper advances to its own first element, so no next() is taken here: doing both
    // would step over the manufacturer and read the serial where the manufacturer belongs.
    if (!decode_serial_identity(reader, tag::kRecordManufacturer, tag::kRecordSerial, tag::kRecordModelPresent,
                                tag::kRecordModel, record.serial_identity)) {
        return false;
    }
    if (!reader.next() || !reader.expect(tag::kRecordDisplayName) ||
        !reader.read_string(record.metadata.display_name)) {
        return false;
    }
    std::optional<std::string> owner;
    if (!decode_optional_string(reader, tag::kRecordOwnerPresent, tag::kRecordOwner, owner)) {
        return false;
    }
    if (owner.has_value()) {
        const auto parsed = OwnerId::create(*owner);
        if (!parsed.has_value()) {
            return false;
        }
        record.metadata.owner = *parsed;
    }
    std::optional<std::string> site;
    if (!decode_optional_string(reader, tag::kRecordSitePresent, tag::kRecordSite, site)) {
        return false;
    }
    if (site.has_value()) {
        const auto parsed = LocationId::create(*site);
        if (!parsed.has_value()) {
            return false;
        }
        record.metadata.site = *parsed;
    }
    if (!reader.next() || !reader.expect(tag::kRecordNotes) || !reader.read_string(record.metadata.notes)) {
        return false;
    }
    if (!decode_labels(reader, tag::kRecordLabels, tag::kLabel, limits.max_labels_per_asset,
                       limits.max_label_value_bytes, record.metadata.labels)) {
        return false;
    }
    if (!decode_references(reader, tag::kRecordReferences, limits.max_references_per_asset, record.references)) {
        return false;
    }
    std::uint64_t lifecycle = 0;
    if (!reader.next() || !decode_enum(reader, tag::kRecordLifecycle, 6, lifecycle)) {
        return false;
    }
    record.state.lifecycle = static_cast<LifecycleState>(lifecycle);
    std::uint64_t installation = 0;
    if (!reader.next() || !decode_enum(reader, tag::kRecordInstallation, 5, installation)) {
        return false;
    }
    record.state.installation = static_cast<InstallationState>(installation);

    bool supersedes_present = false;
    if (!reader.next() || !reader.expect(tag::kRecordSupersedesPresent) ||
        !reader.read_bool(supersedes_present)) {
        return false;
    }
    if (supersedes_present) {
        if (!reader.next() || !reader.expect(tag::kSupersedes)) {
            return false;
        }
        std::uint64_t link_count = 0;
        if (!reader.read_list(6, link_count) || link_count != 6) {
            return false;
        }
        ReplacementLink link;
        if (!reader.next() || !decode_asset_id(reader, tag::kSupersedePredecessor, link.predecessor)) {
            return false;
        }
        std::uint64_t predecessor_generation = 0;
        if (!reader.next() || !decode_enum(reader, tag::kSupersedePredecessorGeneration, 0xFFFFFFFFULL,
                                           predecessor_generation)) {
            return false;
        }
        link.predecessor_generation = AssetGeneration(static_cast<std::uint32_t>(predecessor_generation));
        std::uint64_t predecessor_revision = 0;
        if (!reader.next() || !reader.expect(tag::kSupersedePredecessorRevision) ||
            !reader.read_unsigned(predecessor_revision)) {
            return false;
        }
        link.predecessor_final_revision = AssetRevision(predecessor_revision);
        std::uint64_t cause = 0;
        if (!reader.next() || !decode_enum(reader, tag::kSupersedeCause, 7, cause)) {
            return false;
        }
        link.cause = static_cast<ReplacementCause>(cause);
        std::uint64_t linked_at = 0;
        if (!reader.next() || !reader.expect(tag::kSupersedeLinkedAt) || !reader.read_unsigned(linked_at)) {
            return false;
        }
        link.linked_at = TransactionSequence(linked_at);
        if (!reader.next() || !reader.expect(tag::kSupersedeNote) || !reader.read_string(link.note)) {
            return false;
        }
        record.supersedes = std::move(link);
    }

    if (!decode_provenance(reader, tag::kRecordProvenance, limits.max_provenance_per_asset, record.provenance)) {
        return false;
    }
    if (!reader.next() || !reader.expect(tag::kRecordHistory)) {
        return false;
    }
    std::uint64_t history_count = 0;
    if (!reader.read_list(limits.max_generations_per_asset, history_count)) {
        return false;
    }
    record.history.clear();
    record.history.reserve(static_cast<std::size_t>(history_count));
    for (std::uint64_t index = 0; index < history_count; ++index) {
        if (!reader.next() || !reader.expect(tag::kIncarnation)) {
            return false;
        }
        AssetIncarnation incarnation;
        if (!decode_incarnation(reader, limits, incarnation)) {
            return false;
        }
        record.history.push_back(std::move(incarnation));
    }
    reader.leave_list();
    reader.leave_list();
    return true;
}

// ---------------------------------------------------------------------------
// Writer and idempotency codec
// ---------------------------------------------------------------------------

void encode_writer(TlvWriter& writer, const StoredWriter& stored) {
    writer.open_list(tag::kWriter, 4);
    writer.append_string(tag::kWriterId, stored.writer.text());
    writer.append_unsigned(tag::kWriterHighWater, stored.high_water.value());
    writer.append_unsigned(tag::kWriterLowWater, stored.low_water.value());
    writer.open_list(tag::kWriterRecords, stored.records.size());
    for (const StoredIdempotency& record : stored.records) {
        writer.open_list(tag::kIdempotency, 7);
        writer.append_unsigned(tag::kIdempotencySequence, record.sequence.value());
        writer.append_bytes(tag::kIdempotencyFingerprint, record.fingerprint.data(), record.fingerprint.size());
        writer.append_string(tag::kIdempotencyKey, record.key);
        writer.append_unsigned(tag::kIdempotencyOutcomeKind, static_cast<std::uint64_t>(record.outcome_kind));
        if (record.outcome_kind == StoredOutcomeKind::Error) {
            writer.append_string(tag::kIdempotencyErrorName, error_code_name(record.error_code));
            writer.append_string(tag::kIdempotencyErrorMessage, record.error_message);
        } else {
            writer.append_unsigned(tag::kIdempotencyRevision, record.revision);
            if (record.outcome_kind == StoredOutcomeKind::RegisteredAsset) {
                encode_asset_id(writer, tag::kIdempotencyAsset, record.asset);
            }
        }
        writer.close_list();
    }
    writer.close_list();
    writer.close_list();
}

[[nodiscard]] bool decode_writer(TlvReader& reader, std::uint32_t max_records, StoredWriter& stored) {
    std::uint64_t count = 0;
    if (!reader.read_list(4, count) || count != 4) {
        return false;
    }
    if (!reader.next() || !reader.expect(tag::kWriterId)) {
        return false;
    }
    std::string writer_text;
    if (!reader.read_string(writer_text)) {
        return false;
    }
    const auto writer = WriterId::create(writer_text);
    if (!writer.has_value()) {
        return false;
    }
    stored.writer = *writer;
    std::uint64_t high_water = 0;
    if (!reader.next() || !reader.expect(tag::kWriterHighWater) || !reader.read_unsigned(high_water)) {
        return false;
    }
    stored.high_water = MutationSequence(high_water);
    std::uint64_t low_water = 0;
    if (!reader.next() || !reader.expect(tag::kWriterLowWater) || !reader.read_unsigned(low_water)) {
        return false;
    }
    stored.low_water = MutationSequence(low_water);
    if (!reader.next() || !reader.expect(tag::kWriterRecords)) {
        return false;
    }
    std::uint64_t records = 0;
    if (!reader.read_list(max_records, records)) {
        return false;
    }
    stored.records.clear();
    stored.records.reserve(static_cast<std::size_t>(records));
    for (std::uint64_t index = 0; index < records; ++index) {
        if (!reader.next() || !reader.expect(tag::kIdempotency)) {
            return false;
        }
        std::uint64_t inner = 0;
        if (!reader.read_list(7, inner) || inner < 4) {
            return false;
        }
        StoredIdempotency record;
        std::uint64_t sequence = 0;
        if (!reader.next() || !reader.expect(tag::kIdempotencySequence) || !reader.read_unsigned(sequence)) {
            return false;
        }
        record.sequence = MutationSequence(sequence);
        if (!reader.next() || !reader.expect(tag::kIdempotencyFingerprint)) {
            return false;
        }
        std::vector<std::uint8_t> fingerprint;
        if (!reader.read_bytes(fingerprint) || fingerprint.size() != record.fingerprint.size()) {
            return false;
        }
        std::copy(fingerprint.begin(), fingerprint.end(), record.fingerprint.begin());
        if (!reader.next() || !reader.expect(tag::kIdempotencyKey) || !reader.read_string(record.key)) {
            return false;
        }
        std::uint64_t outcome_kind = 0;
        if (!reader.next() || !decode_enum(reader, tag::kIdempotencyOutcomeKind, 2, outcome_kind)) {
            return false;
        }
        record.outcome_kind = static_cast<StoredOutcomeKind>(outcome_kind);
        bool ok = true;
        for (std::uint64_t extra = 4; extra < inner && ok; ++extra) {
            if (!reader.next()) {
                ok = false;
                break;
            }
            switch (reader.tag()) {
                case tag::kIdempotencyRevision: {
                    std::uint64_t revision = 0;
                    ok = reader.read_unsigned(revision);
                    record.revision = revision;
                    break;
                }
                case tag::kIdempotencyAsset:
                    ok = decode_asset_id(reader, tag::kIdempotencyAsset, record.asset);
                    break;
                case tag::kIdempotencyErrorName: {
                    std::string name;
                    ok = reader.read_string(name);
                    if (ok) {
                        const auto code = error_code_from_name(name);
                        ok = code.has_value();
                        if (ok) {
                            record.error_code = *code;
                        }
                    }
                    break;
                }
                case tag::kIdempotencyErrorMessage:
                    ok = reader.read_string(record.error_message);
                    break;
                default:
                    ok = false;
                    break;
            }
        }
        if (!ok) {
            return false;
        }
        stored.records.push_back(std::move(record));
    }
    reader.leave_list();
    reader.leave_list();
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

std::string encode_generation_header(const GenerationHeader& header) {
    std::string bytes(kHeaderBytes, '\0');
    std::memcpy(bytes.data() + kOffsetMagic, kMagic, sizeof(kMagic));
    const auto put32 = [&bytes](std::size_t offset, std::uint32_t value) {
        for (std::size_t index = 0; index < 4; ++index) {
            bytes[offset + index] = static_cast<char>((value >> ((3U - index) * 8U)) & 0xFFU);
        }
    };
    const auto put64 = [&bytes](std::size_t offset, std::uint64_t value) {
        for (std::size_t index = 0; index < 8; ++index) {
            bytes[offset + index] = static_cast<char>((value >> ((7U - index) * 8U)) & 0xFFU);
        }
    };
    put32(kOffsetFormat, header.format_version);
    put32(kOffsetSchema, header.schema_version);
    put64(kOffsetSequence, header.sequence);
    put64(kOffsetEpoch, header.epoch);
    put64(kOffsetPayloadBytes, header.payload_bytes);
    put32(kOffsetHeaderCrc, crc32(std::string_view(bytes.data(), kOffsetHeaderCrc)));
    return bytes;
}

Outcome<GenerationHeader> decode_generation_header(std::string_view file_bytes) {
    if (file_bytes.size() < kMinimumGenerationBytes) {
        return make_error(ErrorCode::StoreCorrupt,
                          "generation file is shorter than the minimum valid length: " +
                              std::to_string(file_bytes.size()) + " bytes");
    }
    if (std::memcmp(file_bytes.data() + kOffsetMagic, kMagic, sizeof(kMagic)) != 0) {
        return make_error(ErrorCode::StoreCorrupt, "generation file magic does not match the asset registry format");
    }
    const auto get32 = [file_bytes](std::size_t offset) {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            value = (value << 8U) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(file_bytes[offset + index]));
        }
        return value;
    };
    const auto get64 = [file_bytes](std::size_t offset) {
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < 8; ++index) {
            value = (value << 8U) | static_cast<std::uint64_t>(static_cast<std::uint8_t>(file_bytes[offset + index]));
        }
        return value;
    };

    const std::uint32_t stored_header_crc = get32(kOffsetHeaderCrc);
    const std::uint32_t computed_header_crc = crc32(file_bytes.substr(0, kOffsetHeaderCrc));
    if (stored_header_crc != computed_header_crc) {
        return make_error(ErrorCode::StoreCorrupt, "generation file header checksum does not match its contents");
    }

    GenerationHeader header;
    header.format_version = get32(kOffsetFormat);
    header.schema_version = get32(kOffsetSchema);
    header.sequence = get64(kOffsetSequence);
    header.epoch = get64(kOffsetEpoch);
    header.payload_bytes = get64(kOffsetPayloadBytes);

    if (header.format_version != kStoreFormatVersion) {
        return make_error(ErrorCode::StoreVersionUnsupported,
                          "generation file format version " + std::to_string(header.format_version) +
                              " is not supported by this build (expected " + std::to_string(kStoreFormatVersion) +
                              ")");
    }
    if (header.schema_version != kPayloadSchemaVersion) {
        return make_error(ErrorCode::StoreVersionUnsupported,
                          "generation payload schema version " + std::to_string(header.schema_version) +
                              " is not supported by this build (expected " + std::to_string(kPayloadSchemaVersion) +
                              ")");
    }
    if (header.payload_bytes > file_bytes.size() - kHeaderBytes - kTrailerBytes) {
        return make_error(ErrorCode::StoreIntegrityFailed,
                          "generation file declares " + std::to_string(header.payload_bytes) +
                              " payload bytes but the file holds fewer");
    }
    const std::uint64_t expected_total = kHeaderBytes + header.payload_bytes + kTrailerBytes;
    if (file_bytes.size() != expected_total) {
        return make_error(ErrorCode::StoreIntegrityFailed,
                          "generation file length " + std::to_string(file_bytes.size()) +
                              " does not match the declared length " + std::to_string(expected_total) +
                              "; the file is truncated or carries trailing bytes");
    }
    return header;
}

// ---------------------------------------------------------------------------
// Payload
// ---------------------------------------------------------------------------

Outcome<std::string> encode_store_payload(const StorePayload& payload, std::uint64_t max_bytes) {
    if (payload.records.size() > payload.limits.max_assets) {
        return make_error(ErrorCode::CapacityExceeded,
                          "refusing to encode " + std::to_string(payload.records.size()) +
                              " asset records; the configured bound is " +
                              std::to_string(payload.limits.max_assets));
    }
    TlvWriter writer;
    writer.reserve(1024 + payload.records.size() * 256U);

    writer.open_list(tag::kPayload, 0);
    writer.append_unsigned(tag::kSequence, payload.sequence.value());
    writer.append_unsigned(tag::kEpoch, payload.epoch.value());
    writer.append_string(tag::kLastWriter, payload.last_writer);
    writer.append_unsigned(tag::kPredecessorSequence, payload.predecessor_sequence.value());
    writer.append_unsigned(tag::kPredecessorCrc, payload.predecessor_crc);

    writer.open_list(tag::kPolicy, 6);
    writer.append_unsigned(tag::kPolicySerialCollision, static_cast<std::uint64_t>(payload.policy.serial_collision));
    writer.append_unsigned(tag::kPolicyIdentityReuse, static_cast<std::uint64_t>(payload.policy.identity_reuse));
    writer.append_unsigned(tag::kPolicyModelConflict, static_cast<std::uint64_t>(payload.policy.model_conflict));
    writer.append_unsigned(tag::kPolicyRevisionCheck, static_cast<std::uint64_t>(payload.policy.revision_check));
    writer.append_bool(tag::kPolicyRequireTerminalSupersede, payload.policy.require_terminal_before_supersede);
    writer.open_list(tag::kPolicyConsistency, 4);
    writer.append_bool(tag::kConsistencyInstalledForActive, payload.policy.consistency.require_installed_for_active);
    writer.append_bool(tag::kConsistencyInstalledForMaintenance,
                       payload.policy.consistency.require_installed_for_maintenance);
    writer.append_bool(tag::kConsistencyStagedForProvisioned,
                       payload.policy.consistency.require_staged_for_provisioned);
    writer.append_bool(tag::kConsistencyTerminalForSuperseded,
                       payload.policy.consistency.require_terminal_for_superseded);
    writer.close_list();
    writer.close_list();

    writer.open_list(tag::kLimits, 16);
    writer.append_unsigned(tag::kLimitsMaxAssets, payload.limits.max_assets);
    writer.append_unsigned(tag::kLimitsMaxReferences, payload.limits.max_references_per_asset);
    writer.append_unsigned(tag::kLimitsMaxLabels, payload.limits.max_labels_per_asset);
    writer.append_unsigned(tag::kLimitsMaxLabelBytes, payload.limits.max_label_value_bytes);
    writer.append_unsigned(tag::kLimitsMaxProvenance, payload.limits.max_provenance_per_asset);
    writer.append_unsigned(tag::kLimitsMaxGenerations, payload.limits.max_generations_per_asset);
    writer.append_unsigned(tag::kLimitsMaxDisplayName, payload.limits.max_display_name_bytes);
    writer.append_unsigned(tag::kLimitsMaxDocumentBytes, payload.limits.max_document_bytes);
    writer.append_unsigned(tag::kLimitsMaxImportRecords, payload.limits.max_import_records);
    writer.append_unsigned(tag::kLimitsMaxExportRecords, payload.limits.max_export_records);
    writer.append_unsigned(tag::kLimitsMaxRegistryBytes, payload.limits.max_registry_bytes);
    writer.append_unsigned(tag::kLimitsMaxIdempotency, payload.limits.max_idempotency_records_per_writer);
    writer.append_unsigned(tag::kLimitsMaxWriters, payload.limits.max_tracked_writers);
    writer.append_unsigned(tag::kLimitsMaxOrphanTemporaries, payload.limits.max_orphan_temporaries);
    writer.append_unsigned(tag::kLimitsMaxRetainedGenerations, payload.limits.max_retained_generations);
    writer.append_unsigned(tag::kLimitsMaxTraversalDepth, payload.limits.max_traversal_depth);
    writer.close_list();

    writer.open_list(tag::kRecords, payload.records.size());
    for (const auto& record : payload.records) {
        if (record == nullptr) {
            return make_error(ErrorCode::InternalInvariantViolation, "payload carries a null asset record");
        }
        encode_record(writer, *record);
    }
    writer.close_list();

    writer.open_list(tag::kWriters, payload.writers.size());
    for (const StoredWriter& stored : payload.writers) {
        encode_writer(writer, stored);
    }
    writer.close_list();
    writer.close_list();

    if (!writer.balanced()) {
        return make_error(ErrorCode::InternalInvariantViolation, "payload encoder left an element open");
    }
    if (writer.size() > max_bytes) {
        return make_error(ErrorCode::CapacityExceeded,
                          "encoded inventory is " + std::to_string(writer.size()) +
                              " bytes, which exceeds the configured document bound of " +
                              std::to_string(max_bytes) + " bytes");
    }
    return writer.take();
}

Outcome<StorePayload> decode_store_payload(std::string_view payload, const DecodeLimits& decode_limits) {
    if (payload.size() > decode_limits.max_bytes) {
        return make_error(ErrorCode::StoreIntegrityFailed,
                          "payload is " + std::to_string(payload.size()) +
                              " bytes, which exceeds the configured bound of " +
                              std::to_string(decode_limits.max_bytes) + " bytes");
    }
    const auto limits_check = validate_limits(decode_limits.limits);
    if (limits_check.has_value()) {
        return make_error(ErrorCode::InvalidLimits, "decoded bounds are self-inconsistent: " + *limits_check);
    }

    TlvReader reader(payload, decode_limits.max_depth);
    reader.begin();
    if (!reader.next() || !reader.expect(tag::kPayload)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload root", reader));
    }
    std::uint64_t root_count = 0;
    if (!reader.read_list(kPayloadRootMembers, root_count) || root_count != kPayloadRootMembers) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload root list", reader));
    }


    StorePayload result;

    std::uint64_t sequence = 0;
    if (!reader.next() || !reader.expect(tag::kSequence) || !reader.read_unsigned(sequence)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload sequence", reader));
    }
    result.sequence = TransactionSequence(sequence);
    std::uint64_t epoch = 0;
    if (!reader.next() || !reader.expect(tag::kEpoch) || !reader.read_unsigned(epoch)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload epoch", reader));
    }
    result.epoch = RegistryEpoch(epoch);
    if (!reader.next() || !reader.expect(tag::kLastWriter) || !reader.read_string(result.last_writer)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload last writer", reader));
    }
    std::uint64_t predecessor_sequence = 0;
    if (!reader.next() || !reader.expect(tag::kPredecessorSequence) ||
        !reader.read_unsigned(predecessor_sequence)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload predecessor sequence", reader.status()));
    }
    result.predecessor_sequence = TransactionSequence(predecessor_sequence);
    std::uint64_t predecessor_crc = 0;
    if (!reader.next() || !reader.expect(tag::kPredecessorCrc) || !reader.read_unsigned(predecessor_crc)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload predecessor checksum", reader.status()));
    }
    result.predecessor_crc = static_cast<std::uint32_t>(predecessor_crc);

    if (!reader.next() || !reader.expect(tag::kPolicy)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload policy", reader.status()));
    }
    std::uint64_t policy_count = 0;
    if (!reader.read_list(7, policy_count) || policy_count != 6) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload policy list", reader.status()));
    }
    std::uint64_t serial_collision = 0;
    if (!reader.next() || !decode_enum(reader, tag::kPolicySerialCollision, 1, serial_collision)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("policy serial collision", reader.status()));
    }
    std::uint64_t identity_reuse = 0;
    if (!reader.next() || !decode_enum(reader, tag::kPolicyIdentityReuse, 1, identity_reuse)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("policy identity reuse", reader.status()));
    }
    std::uint64_t model_conflict = 0;
    if (!reader.next() || !decode_enum(reader, tag::kPolicyModelConflict, 1, model_conflict)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("policy model conflict", reader.status()));
    }
    std::uint64_t revision_check = 0;
    if (!reader.next() || !decode_enum(reader, tag::kPolicyRevisionCheck, 1, revision_check)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("policy revision check", reader.status()));
    }
    if (!reader.next() || !reader.expect(tag::kPolicyRequireTerminalSupersede) ||
        !reader.read_bool(result.policy.require_terminal_before_supersede)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("policy supersede rule", reader.status()));
    }
    if (!reader.next() || !reader.expect(tag::kPolicyConsistency)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("policy consistency", reader.status()));
    }
    std::uint64_t consistency_count = 0;
    if (!reader.read_list(4, consistency_count) || consistency_count != 4) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("policy consistency list", reader.status()));
    }
    if (!reader.next() || !reader.expect(tag::kConsistencyInstalledForActive) ||
        !reader.read_bool(result.policy.consistency.require_installed_for_active)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("consistency rule", reader.status()));
    }
    if (!reader.next() || !reader.expect(tag::kConsistencyInstalledForMaintenance) ||
        !reader.read_bool(result.policy.consistency.require_installed_for_maintenance)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("consistency rule", reader.status()));
    }
    if (!reader.next() || !reader.expect(tag::kConsistencyStagedForProvisioned) ||
        !reader.read_bool(result.policy.consistency.require_staged_for_provisioned)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("consistency rule", reader.status()));
    }
    if (!reader.next() || !reader.expect(tag::kConsistencyTerminalForSuperseded) ||
        !reader.read_bool(result.policy.consistency.require_terminal_for_superseded)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("consistency rule", reader.status()));
    }
    reader.leave_list();
    reader.leave_list();

    result.policy.serial_collision = static_cast<SerialCollisionPolicy>(serial_collision);
    result.policy.identity_reuse = static_cast<IdentityReusePolicy>(identity_reuse);
    result.policy.model_conflict = static_cast<ModelConflictPolicy>(model_conflict);
    result.policy.revision_check = static_cast<RevisionCheckPolicy>(revision_check);

    if (!reader.next() || !reader.expect(tag::kLimits)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload limits", reader.status()));
    }
    std::uint64_t limits_count = 0;
    if (!reader.read_list(16, limits_count) || limits_count != 16) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload limits list", reader.status()));
    }
    const std::uint64_t limit_tags[16] = {
        tag::kLimitsMaxAssets,          tag::kLimitsMaxReferences,        tag::kLimitsMaxLabels,
        tag::kLimitsMaxLabelBytes,      tag::kLimitsMaxProvenance,        tag::kLimitsMaxGenerations,
        tag::kLimitsMaxDisplayName,     tag::kLimitsMaxDocumentBytes,     tag::kLimitsMaxImportRecords,
        tag::kLimitsMaxExportRecords,   tag::kLimitsMaxRegistryBytes,     tag::kLimitsMaxIdempotency,
        tag::kLimitsMaxWriters,         tag::kLimitsMaxOrphanTemporaries, tag::kLimitsMaxRetainedGenerations,
        tag::kLimitsMaxTraversalDepth,
    };
    std::uint64_t limit_values[16] = {};
    for (std::size_t index = 0; index < 16; ++index) {
        if (!reader.next() || !reader.expect(limit_tags[index]) ||
            !reader.read_unsigned(limit_values[index])) {
            return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload limit value", reader.status()));
        }
    }
    reader.leave_list();
    result.limits.max_assets = limit_values[0];
    result.limits.max_references_per_asset = static_cast<std::uint32_t>(limit_values[1]);
    result.limits.max_labels_per_asset = static_cast<std::uint32_t>(limit_values[2]);
    result.limits.max_label_value_bytes = static_cast<std::size_t>(limit_values[3]);
    result.limits.max_provenance_per_asset = static_cast<std::uint32_t>(limit_values[4]);
    result.limits.max_generations_per_asset = static_cast<std::uint32_t>(limit_values[5]);
    result.limits.max_display_name_bytes = static_cast<std::size_t>(limit_values[6]);
    result.limits.max_document_bytes = limit_values[7];
    result.limits.max_import_records = static_cast<std::uint32_t>(limit_values[8]);
    result.limits.max_export_records = static_cast<std::uint32_t>(limit_values[9]);
    result.limits.max_registry_bytes = limit_values[10];
    result.limits.max_idempotency_records_per_writer = static_cast<std::uint32_t>(limit_values[11]);
    result.limits.max_tracked_writers = static_cast<std::uint32_t>(limit_values[12]);
    result.limits.max_orphan_temporaries = static_cast<std::uint32_t>(limit_values[13]);
    result.limits.max_retained_generations = static_cast<std::uint32_t>(limit_values[14]);
    result.limits.max_traversal_depth = static_cast<std::uint32_t>(limit_values[15]);

    const auto stored_limits_check = validate_limits(result.limits);
    if (stored_limits_check.has_value()) {
        return make_error(ErrorCode::StoreCorrupt,
                          "stored bounds are self-inconsistent: " + *stored_limits_check);
    }

    if (!reader.next() || !reader.expect(tag::kRecords)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload records", reader.status()));
    }
    std::uint64_t record_count = 0;
    if (!reader.read_list(result.limits.max_assets, record_count)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload record list", reader.status()));
    }
    result.records.reserve(static_cast<std::size_t>(record_count));
    std::optional<AssetId> previous_id;
    for (std::uint64_t index = 0; index < record_count; ++index) {
        if (!reader.next() || !reader.expect(tag::kRecord)) {
            return make_error(ErrorCode::StoreCorrupt,
                              tlv_failure("payload record " + std::to_string(index), reader.status()));
        }
        auto record = std::make_shared<AssetRecord>();
        if (!decode_record(reader, result.limits, *record)) {
            return make_error(ErrorCode::StoreCorrupt,
                              tlv_failure("payload record " + std::to_string(index), reader));
        }
        // The payload records the policy it was written under, so a record is validated
        // against the rules in force when it was accepted: a store written by a registry
        // configured with a relaxed rule still loads, and is reported as inconsistent by
        // the audit rather than being refused as unreadable.
        const auto validation = validate_asset_record(*record, result.limits, result.policy.consistency);
        if (validation.has_value()) {
            return make_error(ErrorCode::StoreCorrupt,
                              record_failure(static_cast<std::size_t>(index), *validation));
        }
        if (previous_id.has_value() && !(*previous_id < record->id)) {
            return make_error(ErrorCode::StoreCorrupt,
                              record_failure(static_cast<std::size_t>(index),
                                             "asset records are not in strictly ascending canonical order"));
        }
        previous_id = record->id;
        result.records.push_back(std::move(record));
    }
    reader.leave_list();

    if (!reader.next() || !reader.expect(tag::kWriters)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload writers", reader.status()));
    }
    std::uint64_t writer_count = 0;
    if (!reader.read_list(result.limits.max_tracked_writers, writer_count)) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload writer list", reader.status()));
    }
    result.writers.reserve(static_cast<std::size_t>(writer_count));
    for (std::uint64_t index = 0; index < writer_count; ++index) {
        if (!reader.next() || !reader.expect(tag::kWriter)) {
            return make_error(ErrorCode::StoreCorrupt,
                              tlv_failure("payload writer " + std::to_string(index), reader.status()));
        }
        StoredWriter stored;
        if (!decode_writer(reader, result.limits.max_idempotency_records_per_writer, stored)) {
            return make_error(ErrorCode::StoreCorrupt,
                              tlv_failure("payload writer " + std::to_string(index), reader));
        }
        for (std::size_t record_index = 1; record_index < stored.records.size(); ++record_index) {
            if (!(stored.records[record_index - 1].sequence < stored.records[record_index].sequence)) {
                return make_error(ErrorCode::StoreCorrupt,
                                  "idempotency records are not in strictly ascending sequence order");
            }
        }
        result.writers.push_back(std::move(stored));
    }
    reader.leave_list();

    reader.leave_list();
    if (!reader.exhausted()) {
        return make_error(ErrorCode::StoreCorrupt, tlv_failure("payload trailing bytes", reader));
    }
    if (!result.records.empty() && result.sequence.is_zero()) {
        return make_error(ErrorCode::StoreCorrupt, "payload carries asset records with a zero transaction sequence");
    }
    return result;
}

}  // namespace asset_registry::internal
