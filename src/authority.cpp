// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Writer sessions and the request fingerprinting that makes retries safe.

#include "asset_registry/authority.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "asset_registry/asset.hpp"
#include "asset_registry/text.hpp"
#include "fingerprint.hpp"
#include "uuid.hpp"

namespace asset_registry {

namespace {

/// Appends a length-prefixed field so that concatenated fields cannot be
/// confused with a single field that happens to contain the same bytes.
void append_field(std::string& target, std::string_view value) {
    target += std::to_string(value.size());
    target += ':';
    target.append(value.data(), value.size());
    target += ';';
}

void append_optional_field(std::string& target, const std::optional<std::string>& value) {
    if (!value.has_value()) {
        target += "-;";
        return;
    }
    target += '+';
    append_field(target, *value);
}

}  // namespace

WriterSession WriterSession::create(WriterId writer) {
    WriterSession session;
    session.writer_ = std::move(writer);
    session.next_sequence_ = MutationSequence(1);
    return session;
}

WriterSession WriterSession::resume(WriterId writer, MutationSequence next_sequence) {
    WriterSession session;
    session.writer_ = std::move(writer);
    session.next_sequence_ = next_sequence.is_zero() ? MutationSequence(1) : next_sequence;
    return session;
}

MutationEnvelope WriterSession::advance(std::optional<std::string> idempotency_key, std::string reason) {
    MutationEnvelope envelope;
    envelope.sequence = next_sequence_;
    envelope.idempotency_key = std::move(idempotency_key);
    envelope.reason = std::move(reason);
    const auto next = next_sequence_.next();
    if (next.has_value()) {
        next_sequence_ = *next;
    }
    return envelope;
}

std::string AuthorityToken::to_string() const {
    std::string result = "authority/";
    result += writer_.text();
    result += ";generation=";
    result += generation_.to_string();
    result += ";epoch=";
    result += epoch_.to_string();
    if (expires_at_.has_value()) {
        result += ";expires_at=";
        result += expires_at_->to_rfc3339();
    }
    return result;
}

std::string_view to_string(AuthorityMode value) noexcept {
    switch (value) {
        case AuthorityMode::RegistryAuthority:
            return "registry_authority";
        case AuthorityMode::LocalAuthority:
            return "local_authority";
    }
    return "registry_authority";
}

namespace detail {

std::string fingerprint_material_metadata(const AssetMetadata& metadata) {
    std::string material;
    append_field(material, metadata.display_name);
    append_field(material, metadata.owner.has_value() ? metadata.owner->text() : std::string());
    append_field(material, metadata.site.has_value() ? metadata.site->text() : std::string());
    append_field(material, metadata.notes);
    for (const auto& label : metadata.labels) {
        append_field(material, label.first);
        append_field(material, label.second);
    }
    return material;
}

std::string fingerprint_material_references(const std::vector<Reference>& references) {
    std::string material;
    for (const Reference& reference : references) {
        append_field(material, reference.canonical());
        append_field(material, to_string(reference.evidence()));
    }
    return material;
}

std::string fingerprint_material_serial(const SerialIdentity& identity) {
    std::string material;
    append_field(material, identity.manufacturer().name());
    append_field(material, identity.serial().text());
    append_field(material, identity.model().has_value() ? identity.model()->text() : std::string());
    return material;
}

std::string fingerprint_field(std::string_view value) {
    std::string material;
    append_field(material, value);
    return material;
}

std::string fingerprint_optional(std::string_view presence, const std::optional<std::string>& value) {
    std::string material;
    append_field(material, presence);
    append_optional_field(material, value);
    return material;
}

std::string fingerprint_join(std::initializer_list<std::string_view> parts) {
    std::string material;
    for (const std::string_view part : parts) {
        append_field(material, part);
    }
    return material;
}

std::array<std::uint8_t, 16> fingerprint_of(std::string_view domain, std::string_view payload) {
    return internal::request_fingerprint(domain, payload);
}

}  // namespace detail

}  // namespace asset_registry
