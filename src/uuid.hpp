// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Internal: UUID derivation (RFC 9562 versions 4 and 5) and SHA-1.
//
// SHA-1 is used only as a name-based derivation and fingerprint primitive, never
// as a security control: an idempotency fingerprint detects accidental request
// divergence, and durable authority does not depend on it.

#ifndef ASSET_REGISTRY_INTERNAL_UUID_HPP
#define ASSET_REGISTRY_INTERNAL_UUID_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "asset_registry/error.hpp"
#include "asset_registry/strong_types.hpp"

namespace asset_registry::internal {

/// 160-bit SHA-1 digest.
using Sha1Digest = std::array<std::uint8_t, 20>;

[[nodiscard]] Sha1Digest sha1(std::string_view data) noexcept;
[[nodiscard]] Sha1Digest sha1(const std::uint8_t* data, std::size_t size) noexcept;

/// SHA-1 over a sequence of parts without concatenating them first.
[[nodiscard]] Sha1Digest sha1_parts(std::initializer_list<std::string_view> parts) noexcept;

/// Namespace UUID for canonical AssetId derivation. Fixed forever: it is part of
/// the identity contract, and changing it would silently re-derive every
/// deterministic asset identity.
[[nodiscard]] const AssetId::bytes_type& asset_namespace_uuid() noexcept;

/// RFC 9562 version 5 (name-based, SHA-1) identifier inside the supplied
/// namespace. Deterministic: the same name always yields the same identifier.
[[nodiscard]] AssetId uuid_v5(const AssetId::bytes_type& name_space, std::string_view name) noexcept;

/// RFC 9562 version 4 (random) identifier from the process CSPRNG.
/// Reports failure instead of silently returning a low-entropy identifier when
/// the operating system entropy source is unavailable.
[[nodiscard]] Outcome<AssetId> uuid_v4();

/// 128-bit deterministic fingerprint of a logical request. Used for idempotency
/// conflict detection: a repeated key with a different fingerprint is rejected
/// rather than applied.
[[nodiscard]] std::array<std::uint8_t, 16> request_fingerprint(std::string_view domain,
                                                               std::string_view payload) noexcept;

}  // namespace asset_registry::internal

#endif  // ASSET_REGISTRY_INTERNAL_UUID_HPP
