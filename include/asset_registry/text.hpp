// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Text validation. Everything crossing the API boundary is untrusted, so
// strings are validated and bounded before they are stored. Validation never
// rewrites the value: a rejected string is rejected, not normalised into
// something surprising. Normalisation exists only for derived comparison keys.

#ifndef ASSET_REGISTRY_TEXT_HPP
#define ASSET_REGISTRY_TEXT_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "asset_registry/error.hpp"

namespace asset_registry {

/// Result of a UTF-8 structural validation pass.
enum class Utf8Status {
    Valid,
    Empty,             ///< zero bytes; the caller decides whether that matters
    InvalidSequence,   ///< malformed lead/continuation byte structure
    OverlongEncoding,  ///< a code point encoded in more bytes than required
    SurrogateCodePoint,///< U+D800..U+DFFF, forbidden by the UTF-8 definition
    OutOfRange,        ///< above U+10FFFF
    DisallowedCodePoint///< decorative/invisible formatting or control character
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(Utf8Status status) noexcept;

/// Structural UTF-8 validation. Rejects invalid sequences, overlong encodings,
/// UTF-16 surrogates, code points above U+10FFFF, C0/C1 control characters, and
/// bidirectional-override/zero-width formatting characters that would allow an
/// identifier to render identically to a different identifier.
[[nodiscard]] ASSET_REGISTRY_API Utf8Status validate_utf8(std::string_view text) noexcept;
[[nodiscard]] ASSET_REGISTRY_API Utf8Status validate_utf8(std::string_view text, bool allow_empty) noexcept;

/// Byte length of a UTF-8 string, counted as code points.
[[nodiscard]] ASSET_REGISTRY_API std::size_t utf8_code_point_count(std::string_view text) noexcept;

/// Derived comparison key: NFKC-style compatibility folding for the narrow set
/// of forms that can make two visually distinct vendor identifiers collide
/// (full-width ASCII, ideographic space, non-breaking space), whitespace run
/// collapsing, and locale-independent ASCII case folding. The fold is
/// deterministic and pure ASCII-in/ASCII-out for ASCII input.
[[nodiscard]] ASSET_REGISTRY_API std::string make_comparison_key(std::string_view text);

/// True when the folded key contains only ASCII letters, digits, and separators
/// that survive folding. Used to decide whether a collision can be resolved by
/// exact spelling comparison.
[[nodiscard]] ASSET_REGISTRY_API bool is_ascii_foldable(std::string_view text) noexcept;

/// Lowercase-hyphen namespace syntax: [a-z][a-z0-9-]{0,30}[a-z0-9].
[[nodiscard]] ASSET_REGISTRY_API bool is_identity_namespace(std::string_view text) noexcept;

/// Dotted identity path syntax: one or more dot-separated segments, each
/// matching [a-z0-9][a-z0-9_-]{0,62}[a-z0-9] (single character allowed).
[[nodiscard]] ASSET_REGISTRY_API bool is_dotted_identity_path(std::string_view text) noexcept;

/// Serial-number syntax: 1..128 bytes of unrestricted (but structurally valid)
/// UTF-8 excluding control characters and separators that would break framed
/// encodings. Vendor serials legitimately contain '/', '#', '.', ' ', '-'.
[[nodiscard]] ASSET_REGISTRY_API bool is_serial_number_text(std::string_view text) noexcept;

/// Bounded, validated display text: non-empty, structurally valid UTF-8, no
/// control characters, at most `max_bytes` bytes.
[[nodiscard]] ASSET_REGISTRY_API bool is_display_text(std::string_view text, std::size_t max_bytes) noexcept;

/// Label key syntax: [a-z][a-z0-9_.-]{0,62}, lowercase only, so that label
/// lookup is case-deterministic.
[[nodiscard]] ASSET_REGISTRY_API bool is_label_key(std::string_view text) noexcept;

/// Parses an unsigned decimal value with no sign, no whitespace, no leading
/// zero (except "0" itself), and no overflow. The single canonical integer
/// parser used by every external text input path.
[[nodiscard]] ASSET_REGISTRY_API std::optional<std::uint64_t> parse_unsigned_decimal(std::string_view text) noexcept;

/// Escapes text as a JSON string body (without surrounding quotes) using
/// deterministic escape selection. Assumes structurally valid UTF-8.
[[nodiscard]] ASSET_REGISTRY_API std::string json_escape(std::string_view text);

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_TEXT_HPP
