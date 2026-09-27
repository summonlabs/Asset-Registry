// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_registry/strong_types.hpp"

#include "uuid.hpp"

namespace asset_registry {

const AssetId& AssetId::nil() noexcept {
    static const AssetId value{};
    return value;
}

Outcome<AssetId> AssetId::generate() {
    return internal::uuid_v4();
}

AssetId AssetId::generate_unchecked() noexcept {
    const Outcome<AssetId> generated = internal::uuid_v4();
    return generated.has_value() ? generated.value() : AssetId{};
}

AssetId AssetId::from_bytes(const bytes_type& bytes) noexcept {
    AssetId identifier;
    identifier.bytes_ = bytes;
    return identifier;
}

std::optional<AssetId> AssetId::parse(std::string_view text) noexcept {
    // Canonical form only: 36 lowercase hex characters in 8-4-4-4-12 grouping.
    // Uppercase, braces, urn: prefixes, and unhyphenated forms are rejected so
    // that one identity has exactly one textual spelling.
    if (text.size() != 36) {
        return std::nullopt;
    }
    static constexpr std::size_t kHyphenPositions[] = {8, 13, 18, 23};
    for (const std::size_t position : kHyphenPositions) {
        if (text[position] != '-') {
            return std::nullopt;
        }
    }
    bytes_type bytes{};
    std::size_t nibble_index = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '-') {
            continue;
        }
        std::uint8_t value = 0;
        if (character >= '0' && character <= '9') {
            value = static_cast<std::uint8_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            value = static_cast<std::uint8_t>(character - 'a' + 10);
        } else {
            return std::nullopt;
        }
        const std::size_t byte_index = nibble_index / 2;
        if (byte_index >= bytes.size()) {
            return std::nullopt;
        }
        if (nibble_index % 2 == 0) {
            bytes[byte_index] = static_cast<std::uint8_t>(value << 4U);
        } else {
            bytes[byte_index] = static_cast<std::uint8_t>(bytes[byte_index] | value);
        }
        ++nibble_index;
    }
    if (nibble_index != 32) {
        return std::nullopt;
    }
    return from_bytes(bytes);
}

bool AssetId::is_nil() const noexcept {
    for (const std::uint8_t byte : bytes_) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::string AssetId::to_string() const {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < bytes_.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result.push_back('-');
        }
        result.push_back(kHexDigits[(bytes_[index] >> 4U) & 0x0FU]);
        result.push_back(kHexDigits[bytes_[index] & 0x0FU]);
    }
    return result;
}

std::string AssetId::to_compact_string() const {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (const std::uint8_t byte : bytes_) {
        result.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
        result.push_back(kHexDigits[byte & 0x0FU]);
    }
    return result;
}

}  // namespace asset_registry
