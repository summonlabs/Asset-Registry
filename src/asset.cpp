// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_registry/asset.hpp"

#include "asset_registry/text.hpp"

#include <string>

namespace asset_registry {
namespace {

[[nodiscard]] std::uint64_t string_bytes(const std::string& value) noexcept {
    // Approximate retained capacity: std::string may hold more than size() bytes.
    return static_cast<std::uint64_t>(value.capacity());
}

[[nodiscard]] std::uint64_t vector_bytes(std::size_t count, std::size_t element_size) noexcept {
    return static_cast<std::uint64_t>(count) * static_cast<std::uint64_t>(element_size);
}

[[nodiscard]] std::string suffix_index(std::size_t index) {
    return " at index " + std::to_string(index);
}

}  // namespace

const std::string* AssetMetadata::find_label(std::string_view key) const noexcept {
    for (const auto& entry : labels) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

std::optional<Reference> AssetRecord::find_reference(const Reference& like) const noexcept {
    for (const Reference& reference : references) {
        if (reference.kind() == like.kind() && reference.canonical() == like.canonical()) {
            return reference;
        }
    }
    return std::nullopt;
}

bool AssetRecord::has_reference(const Reference& reference) const noexcept {
    return find_reference(reference).has_value();
}

std::uint64_t AssetRecord::estimated_bytes() const noexcept {
    std::uint64_t total = sizeof(AssetRecord);
    total += string_bytes(metadata.display_name);
    total += string_bytes(metadata.notes);
    total += string_bytes(serial_identity.canonical_key());
    for (const auto& label : metadata.labels) {
        total += string_bytes(label.first) + string_bytes(label.second);
        total += 2 * sizeof(std::string);
    }
    total += vector_bytes(metadata.labels.size(), 0);
    for (const Reference& reference : references) {
        total += string_bytes(reference.canonical());
    }
    total += vector_bytes(references.size(), sizeof(Reference));
    for (const ProvenanceStep& step : provenance) {
        total += string_bytes(step.reason) + string_bytes(step.change);
        total += string_bytes(step.actor.writer_id().text());
    }
    total += vector_bytes(provenance.size(), sizeof(ProvenanceStep));
    for (const AssetIncarnation& incarnation : history) {
        total += string_bytes(incarnation.metadata.display_name);
        total += string_bytes(incarnation.metadata.notes);
        total += string_bytes(incarnation.serial_identity.canonical_key());
        for (const auto& label : incarnation.metadata.labels) {
            total += string_bytes(label.first) + string_bytes(label.second);
        }
        for (const Reference& reference : incarnation.references) {
            total += string_bytes(reference.canonical());
        }
    }
    total += vector_bytes(history.size(), sizeof(AssetIncarnation));
    return total;
}

std::vector<CapabilityReference> AssetView::capability_references() const {
    std::vector<CapabilityReference> result;
    for (const Reference& reference : references) {
        if (reference.kind() == Reference::Kind::Capability) {
            result.push_back(reference.capability());
        }
    }
    return result;
}

std::optional<LocationId> AssetView::location() const noexcept {
    for (const Reference& reference : references) {
        if (reference.kind() == Reference::Kind::Location) {
            return reference.location();
        }
    }
    return std::nullopt;
}

std::optional<RackId> AssetView::rack() const noexcept {
    for (const Reference& reference : references) {
        if (reference.kind() == Reference::Kind::Rack) {
            return reference.rack();
        }
    }
    return std::nullopt;
}

std::optional<ReferenceEvidence> AssetView::location_evidence() const noexcept {
    for (const Reference& reference : references) {
        if (reference.kind() == Reference::Kind::Location) {
            return reference.evidence();
        }
    }
    return std::nullopt;
}

std::optional<ReferenceEvidence> AssetView::rack_evidence() const noexcept {
    for (const Reference& reference : references) {
        if (reference.kind() == Reference::Kind::Rack) {
            return reference.evidence();
        }
    }
    return std::nullopt;
}

std::optional<std::string> validate_asset_record(const AssetRecord& record, const RegistryLimits& bounds) {
    return validate_asset_record(record, bounds, default_consistency_rules());
}

std::optional<std::string> validate_asset_record(const AssetRecord& record, const RegistryLimits& bounds,
                                                const StateConsistencyRules& consistency_rules) {
    if (record.id.is_nil()) {
        return std::string("asset identity is the nil identifier");
    }
    if (record.asset_class == AssetClass::Unknown) {
        return std::string("asset class is unknown; a stored record must carry a classified object");
    }
    if (record.serial_identity.empty()) {
        return std::string("asset carries no manufacturer/serial identity");
    }
    if (record.generation.is_zero()) {
        return std::string("asset generation is zero; generations start at one");
    }
    if (record.revision.is_zero()) {
        return std::string("asset revision is zero; revisions start at one");
    }
    if (record.metadata.display_name.empty() ||
        !is_display_text(record.metadata.display_name, bounds.max_display_name_bytes)) {
        return std::string("asset display name is missing or violates display-text rules");
    }
    if (!record.metadata.notes.empty() &&
        validate_utf8(record.metadata.notes) != Utf8Status::Valid) {
        return std::string("asset notes are not structurally valid UTF-8");
    }
    if (record.metadata.notes.size() > limits::kMaxNotesBytes) {
        return std::string("asset notes exceed the maximum length");
    }
    if (record.metadata.labels.size() > bounds.max_labels_per_asset) {
        return std::string("asset carries more labels than the configured bound");
    }
    for (std::size_t index = 1; index < record.metadata.labels.size(); ++index) {
        if (!(record.metadata.labels[index - 1].first < record.metadata.labels[index].first)) {
            return std::string("asset labels are not in strictly ascending key order") + suffix_index(index);
        }
    }    for (const auto& label : record.metadata.labels) {
        if (!is_label_key(label.first)) {
            return "asset label key violates label syntax: " + label.first;
        }
        if (label.second.size() > bounds.max_label_value_bytes ||
            validate_utf8(label.second, true) != Utf8Status::Valid) {
            return "asset label value violates text rules for key: " + label.first;
        }
    }
    if (record.references.size() > bounds.max_references_per_asset) {
        return std::string("asset carries more references than the configured bound");
    }
    // References are stored in strictly ascending canonical order, which makes
    // duplicate attachment impossible to represent and makes serialisation
    // order-stable without a sort at encode time.
    for (std::size_t index = 1; index < record.references.size(); ++index) {
        if (!(record.references[index - 1].canonical() < record.references[index].canonical())) {
            return std::string("asset references are not in strictly ascending canonical order") + suffix_index(index);
        }
    }
    for (const Reference& reference : record.references) {
        if (reference.canonical().empty()) {
            return std::string("asset carries a reference with an empty canonical key");
        }
        switch (reference.kind()) {
            case Reference::Kind::Capability:
                if (reference.capability().empty()) {
                    return std::string("asset carries an empty capability reference");
                }
                break;
            case Reference::Kind::Location:
                if (reference.location().empty()) {
                    return std::string("asset carries an empty location reference");
                }
                break;
            case Reference::Kind::Rack:
                if (reference.rack().empty()) {
                    return std::string("asset carries an empty rack reference");
                }
                break;
            case Reference::Kind::ExternalObject:
                if (reference.external_object().empty()) {
                    return std::string("asset carries an empty external object reference");
                }
                break;
        }
    }
    const std::optional<std::string> consistency =
        check_state_consistency(record.state.lifecycle, record.state.installation, consistency_rules);
    if (consistency.has_value()) {
        return "asset state violates consistency rules: " + *consistency;
    }
    if (record.provenance.size() > bounds.max_provenance_per_asset) {
        return std::string("asset provenance history exceeds the configured bound");
    }
    for (std::size_t index = 0; index < record.provenance.size(); ++index) {
        const ProvenanceStep& step = record.provenance[index];
        if (step.sequence.is_zero()) {
            return std::string("provenance step carries a zero transaction sequence") + suffix_index(index);
        }
        if (step.revision.is_zero()) {
            return std::string("provenance step carries a zero revision") + suffix_index(index);
        }
        if (step.reason.size() > limits::kMaxReasonBytes) {
            return std::string("provenance step reason exceeds the maximum length") + suffix_index(index);
        }
        if (validate_utf8(step.reason, true) != Utf8Status::Valid ||
            validate_utf8(step.change, true) != Utf8Status::Valid) {
            return std::string("provenance step carries text that is not structurally valid UTF-8") + suffix_index(index);
        }
        if (index > 0) {
            const ProvenanceStep& previous = record.provenance[index - 1];
            if (!(previous.sequence < step.sequence)) {
                return std::string("provenance steps are not in strictly ascending sequence order") + suffix_index(index);
            }
            if (!(previous.revision < step.revision)) {
                return std::string("provenance steps are not in strictly ascending revision order") + suffix_index(index);
            }
        }
    }
    if (!record.provenance.empty()) {
        const ProvenanceStep& newest = record.provenance.back();
        if (newest.revision != record.revision) {
            return std::string("newest provenance step revision does not match the record revision");
        }
        if (newest.sequence != record.last_sequence) {
            return std::string("newest provenance step sequence does not match the record sequence");
        }
    }
    if (record.history.size() > bounds.max_generations_per_asset) {
        return std::string("asset incarnation history exceeds the configured bound");
    }
    AssetGeneration previous_generation = record.generation;
    for (std::size_t index = 0; index < record.history.size(); ++index) {
        const AssetIncarnation& incarnation = record.history[index];
        if (incarnation.generation.is_zero()) {
            return std::string("incarnation carries a zero generation") + suffix_index(index);
        }
        if (!(incarnation.generation < previous_generation)) {
            return std::string("incarnation history is not in strictly ascending generation order") + suffix_index(index);
        }
        if (incarnation.final_revision.is_zero()) {
            return std::string("incarnation carries a zero final revision") + suffix_index(index);
        }
        if (incarnation.asset_class == AssetClass::Unknown) {
            return std::string("incarnation carries an unknown asset class") + suffix_index(index);
        }
        previous_generation = incarnation.generation;
    }
    if (record.supersedes.has_value()) {
        const ReplacementLink& link = *record.supersedes;
        if (link.predecessor.is_nil()) {
            return std::string("replacement link names the nil identifier as predecessor");
        }
        if (link.predecessor == record.id) {
            return std::string("replacement link names the record itself as predecessor");
        }
        if (link.predecessor_generation.is_zero()) {
            return std::string("replacement link carries a zero predecessor generation");
        }
        if (link.predecessor_final_revision.is_zero()) {
            return std::string("replacement link carries a zero predecessor revision");
        }
        if (link.linked_at.is_zero()) {
            return std::string("replacement link carries a zero transaction sequence");
        }
        if (link.note.size() > limits::kMaxReplacementReasonBytes) {
            return std::string("replacement link note exceeds the maximum length");
        }
        if (validate_utf8(link.note, true) != Utf8Status::Valid) {
            return std::string("replacement link note is not structurally valid UTF-8");
        }
    }
    return std::nullopt;
}

}  // namespace asset_registry
