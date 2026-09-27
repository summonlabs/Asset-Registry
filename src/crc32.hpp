// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Internal: CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
//
// Used for structural integrity of durable artefacts: every generation file and
// every metadata record carries a CRC-32 over its declared byte payload. This is
// a corruption detector, not an authenticity mechanism; durability does not rely
// on it for adversarial resistance.

#ifndef ASSET_REGISTRY_INTERNAL_CRC32_HPP
#define ASSET_REGISTRY_INTERNAL_CRC32_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "asset_registry/strong_types.hpp"

namespace asset_registry::internal {

[[nodiscard]] std::uint32_t crc32(std::string_view data) noexcept;
[[nodiscard]] std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept;

/// CRC-32 of the concatenation of two buffers without materialising the
/// concatenation.
[[nodiscard]] std::uint32_t crc32_pair(std::string_view first, std::string_view second) noexcept;

/// CRC-32 of "A followed by B" given crc32(A), crc32(B), and the length of B.
/// Standard GF(2) matrix combination; used to verify hash chains across
/// generation files without holding both payloads in memory at once.
[[nodiscard]] std::uint32_t crc32_combine(std::uint32_t first, std::uint32_t second,
                                          std::uint64_t second_length) noexcept;

/// CRC of the zero byte sequence of the given length, i.e. the effect of
/// appending `length` zero bytes to the CRC value `seed`.
[[nodiscard]] std::uint32_t crc32_shift_zeros(std::uint32_t seed, std::uint64_t length) noexcept;

/// CRC-32 of a big-endian unsigned integer encoding.
[[nodiscard]] std::uint32_t crc32_u64_be(std::uint32_t seed, std::uint64_t value) noexcept;

}  // namespace asset_registry::internal

#endif  // ASSET_REGISTRY_INTERNAL_CRC32_HPP
