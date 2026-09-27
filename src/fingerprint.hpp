// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Internal: canonical request material for idempotency fingerprints.
//
// A fingerprint answers exactly one question: "is this request byte-for-byte the
// request that produced the recorded outcome?". It therefore needs to be
// unambiguous (two different requests never produce the same material) and cheap,
// but it is not a wire format and is never parsed back.

#ifndef ASSET_REGISTRY_INTERNAL_FINGERPRINT_HPP
#define ASSET_REGISTRY_INTERNAL_FINGERPRINT_HPP

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_registry/asset.hpp"
#include "asset_registry/reference.hpp"
#include "asset_registry/serial_identity.hpp"

namespace asset_registry::detail {

/// One length-prefixed, self-delimiting field.
[[nodiscard]] std::string fingerprint_field(std::string_view value);
/// Presence marker plus the value when present, so "absent" and "empty" differ.
[[nodiscard]] std::string fingerprint_optional(std::string_view presence, const std::optional<std::string>& value);
/// Concatenation of length-prefixed fields.
[[nodiscard]] std::string fingerprint_join(std::initializer_list<std::string_view> parts);
[[nodiscard]] std::string fingerprint_material_metadata(const AssetMetadata& metadata);
[[nodiscard]] std::string fingerprint_material_references(const std::vector<Reference>& references);
[[nodiscard]] std::string fingerprint_material_serial(const SerialIdentity& identity);

/// Builds the fingerprint input for a command from its domain name and payload.
[[nodiscard]] std::array<std::uint8_t, 16> fingerprint_of(std::string_view domain, std::string_view payload);

}  // namespace asset_registry::detail

#endif  // ASSET_REGISTRY_INTERNAL_FINGERPRINT_HPP
