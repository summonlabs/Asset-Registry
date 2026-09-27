// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Internal: JSON document model and parser.
//
// The parser is the trust boundary for every imported document. It is strict by
// design:
//
//   * Object member names must be unique; a repeated name is rejected rather than
//     silently letting the last one win.
//   * Numbers must be canonical non-negative integers. Fractions, exponents,
//     signs, and leading zeros are rejected, so an integer field has exactly one
//     accepted spelling and no float rounding enters an identity or a counter.
//   * Strings must be structurally valid UTF-8 after escape processing, and
//     \u escapes must be well formed, including surrogate pairs.
//   * Nesting depth and total value count are bounded, and the bounds are checked
//     before the corresponding allocation, so a crafted document cannot exhaust
//     the stack or the heap.
//
// Object members are preserved in document order so that validation can report
// the position of a fault and so that "the same document" has one meaning.

#ifndef ASSET_REGISTRY_INTERNAL_JSON_PARSE_HPP
#define ASSET_REGISTRY_INTERNAL_JSON_PARSE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_registry/error.hpp"

namespace asset_registry::internal {

struct JsonValue;
using JsonValuePtr = std::shared_ptr<const JsonValue>;

struct JsonMember {
    std::string name;
    JsonValuePtr value;
};

struct JsonValue {
    enum class Kind : std::uint8_t {
        Null = 0,
        Bool = 1,
        UInt = 2,
        Int = 3,
        String = 4,
        Array = 5,
        Object = 6,
    };

    Kind kind = Kind::Null;
    bool boolean = false;
    std::uint64_t unsigned_value = 0;
    std::int64_t signed_value = 0;
    std::string text;
    std::vector<JsonValuePtr> elements;
    std::vector<JsonMember> members;

    [[nodiscard]] const JsonValue* find(std::string_view name) const noexcept {
        for (const JsonMember& member : members) {
            if (member.name == name) {
                return member.value.get();
            }
        }
        return nullptr;
    }
};

/// Parses one JSON document. The whole input must be consumed: trailing
/// non-whitespace is rejected.
[[nodiscard]] Outcome<JsonValuePtr> parse_json(std::string_view document, std::uint32_t max_depth,
                                               std::uint64_t max_values);

}  // namespace asset_registry::internal

#endif  // ASSET_REGISTRY_INTERNAL_JSON_PARSE_HPP
