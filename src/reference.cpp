// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_registry/reference.hpp"

#include <limits>

#include "asset_registry/strong_types.hpp"
#include "asset_registry/text.hpp"

namespace asset_registry {
namespace {

constexpr char kUnitSeparator[] = "-";

}  // namespace

std::optional<OwnerId> OwnerId::create(std::string_view text) {
    if (text.empty() || text.size() > limits::kMaxOwnerIdBytes) {
        return std::nullopt;
    }
    if (!is_dotted_identity_path(text)) {
        return std::nullopt;
    }
    OwnerId identifier;
    identifier.text_ = std::string(text);
    return identifier;
}

std::optional<WriterId> WriterId::create(std::string_view text) {
    if (text.empty() || text.size() > limits::kMaxWriterIdBytes) {
        return std::nullopt;
    }
    // Writer identifiers are operator-chosen labels, so they allow the same
    // conservative syntax as other identity paths plus a trailing context tag.
    for (const char character : text) {
        const bool allowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') || character == '.' || character == '-' ||
                             character == '_' || character == ':';
        if (!allowed) {
            return std::nullopt;
        }
    }
    if (text.front() == '.' || text.front() == '-' || text.front() == '_' || text.front() == ':') {
        return std::nullopt;
    }
    WriterId identifier;
    identifier.text_ = std::string(text);
    return identifier;
}

WriterId WriterId::generate(std::string_view prefix) {
    const Outcome<AssetId> generated = AssetId::generate();
    WriterId identifier;
    std::string text(prefix);
    if (text.empty()) {
        text = "writer";
    }
    text += '-';
    text += generated.has_value() ? generated.value().to_compact_string() : std::string("unavailable");
    if (text.size() > limits::kMaxWriterIdBytes) {
        text.resize(limits::kMaxWriterIdBytes);
    }
    identifier.text_ = std::move(text);
    return identifier;
}

std::optional<LocationId> LocationId::create(std::string_view text) {
    if (text.empty() || text.size() > limits::kMaxLocationIdBytes) {
        return std::nullopt;
    }
    if (!is_dotted_identity_path(text)) {
        return std::nullopt;
    }
    LocationId identifier;
    identifier.text_ = std::string(text);
    return identifier;
}

std::optional<RackId> RackId::create(std::string_view text) {
    if (text.empty() || text.size() > limits::kMaxRackIdBytes) {
        return std::nullopt;
    }
    if (!is_dotted_identity_path(text)) {
        return std::nullopt;
    }
    RackId identifier;
    identifier.text_ = std::string(text);
    return identifier;
}

std::optional<UnitSpan> UnitSpan::create(std::uint32_t low, std::uint32_t high) noexcept {
    if (low < 1 || high <= low) {
        return std::nullopt;
    }
    // The highest representable unit is kMaxRackUnit; a span whose exclusive
    // upper bound is kMaxRackUnit + 1 therefore covers the last unit exactly.
    if (high > static_cast<std::uint32_t>(kMaxRackUnit) + 1U) {
        return std::nullopt;
    }
    UnitSpan span;
    span.low_ = low;
    span.high_ = high;
    return span;
}

std::optional<UnitSpan> UnitSpan::parse(std::string_view text) noexcept {
    const std::size_t separator = text.find(kUnitSeparator);
    if (separator == std::string_view::npos) {
        const auto single = parse_unsigned_decimal(text);
        if (!single.has_value() || *single == 0 || *single > kMaxRackUnit) {
            return std::nullopt;
        }
        const auto low = static_cast<std::uint32_t>(*single);
        return create(low, low + 1U);
    }
    const auto low_value = parse_unsigned_decimal(text.substr(0, separator));
    const auto high_value = parse_unsigned_decimal(text.substr(separator + 1));
    if (!low_value.has_value() || !high_value.has_value()) {
        return std::nullopt;
    }
    if (*low_value == 0 || *high_value == 0 || *low_value > std::numeric_limits<std::uint32_t>::max() ||
        *high_value > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return create(static_cast<std::uint32_t>(*low_value), static_cast<std::uint32_t>(*high_value));
}

std::string UnitSpan::to_string() const {
    if (empty()) {
        return {};
    }
    std::string result = std::to_string(low_);
    result += kUnitSeparator;
    result += std::to_string(high_);
    return result;
}

std::optional<ExternalObjectReference> ExternalObjectReference::create(std::string_view kind, std::string_view id) {
    if (!is_identity_namespace(kind)) {
        return std::nullopt;
    }
    if (id.empty() || id.size() > limits::kMaxExternalObjectIdBytes) {
        return std::nullopt;
    }
    if (validate_utf8(id, false) != Utf8Status::Valid) {
        return std::nullopt;
    }
    ExternalObjectReference reference;
    reference.kind_ = std::string(kind);
    reference.id_ = std::string(id);
    return reference;
}

std::string_view to_string(ReferenceEvidence value) noexcept {
    switch (value) {
        case ReferenceEvidence::Unverified:
            return "unverified";
        case ReferenceEvidence::Verified:
            return "verified";
        case ReferenceEvidence::Stale:
            return "stale";
    }
    return "unverified";
}

std::optional<ReferenceEvidence> reference_evidence_from_token(std::string_view token) noexcept {
    if (token == "unverified") {
        return ReferenceEvidence::Unverified;
    }
    if (token == "verified") {
        return ReferenceEvidence::Verified;
    }
    if (token == "stale") {
        return ReferenceEvidence::Stale;
    }
    return std::nullopt;
}

bool is_actionable(ReferenceEvidence value) noexcept {
    return value == ReferenceEvidence::Verified;
}

std::string_view to_string(Reference::Kind value) noexcept {
    switch (value) {
        case Reference::Kind::Capability:
            return "capability";
        case Reference::Kind::Location:
            return "location";
        case Reference::Kind::Rack:
            return "rack";
        case Reference::Kind::ExternalObject:
            return "external_object";
    }
    return "capability";
}

Reference Reference::capability(class CapabilityReference value, ReferenceEvidence evidence) {
    Reference reference;
    reference.kind_ = Kind::Capability;
    reference.evidence_ = evidence;
    reference.capability_ = std::move(value);
    reference.canonical_ = std::string(to_string(Kind::Capability));
    reference.canonical_ += ':';
    reference.canonical_ += reference.capability_.canonical();
    return reference;
}

Reference Reference::location(class LocationId value, ReferenceEvidence evidence) {
    Reference reference;
    reference.kind_ = Kind::Location;
    reference.evidence_ = evidence;
    reference.location_ = std::move(value);
    reference.canonical_ = std::string(to_string(Kind::Location));
    reference.canonical_ += ':';
    reference.canonical_ += reference.location_.text();
    return reference;
}

Reference Reference::rack(class RackId value, ReferenceEvidence evidence) {
    Reference reference;
    reference.kind_ = Kind::Rack;
    reference.evidence_ = evidence;
    reference.rack_ = std::move(value);
    reference.canonical_ = std::string(to_string(Kind::Rack));
    reference.canonical_ += ':';
    reference.canonical_ += reference.rack_.text();
    return reference;
}

Reference Reference::external_object(class ExternalObjectReference value, ReferenceEvidence evidence) {
    Reference reference;
    reference.kind_ = Kind::ExternalObject;
    reference.evidence_ = evidence;
    reference.external_ = std::move(value);
    reference.canonical_ = std::string(to_string(Kind::ExternalObject));
    reference.canonical_ += ':';
    reference.canonical_ += reference.external_.kind();
    reference.canonical_ += '/';
    reference.canonical_ += reference.external_.id();
    return reference;
}

std::string_view Reference::kind_token() const noexcept {
    return to_string(kind_);
}

}  // namespace asset_registry
