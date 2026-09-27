// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_registry/text.hpp"

#include <array>
#include <cstdint>
#include <limits>

#include "asset_registry/capability.hpp"
#include "asset_registry/reference.hpp"
#include "asset_registry/serial_identity.hpp"

namespace asset_registry {
namespace {

struct Decoded {
    std::uint32_t code_point = 0;
    std::size_t length = 0;
    Utf8Status status = Utf8Status::Valid;
};

struct Sequence {
    /// Bits of the leading byte that identify the sequence length. The test is
    /// (byte & mask) == lead, so the mask selects the identifying bits and the
    /// lead gives their required value.
    std::uint8_t mask;
    std::uint8_t lead;
    std::size_t length;    ///< total bytes in the sequence
    std::uint32_t minimum; ///< smallest code point this length may encode
};

constexpr std::array<Sequence, 4> kSequences = {{
    {0x80U, 0x00U, 1, 0x0000U},  // 0xxxxxxx
    {0xE0U, 0xC0U, 2, 0x0080U},  // 110xxxxx
    {0xF0U, 0xE0U, 3, 0x0800U},  // 1110xxxx
    {0xF8U, 0xF0U, 4, 0x10000U}, // 11110xxx
}};

/// Code point bits carried by the leading byte of a sequence of this length.
[[nodiscard]] constexpr std::uint32_t leading_value_mask(std::size_t length) noexcept {
    switch (length) {
        case 1:
            return 0x7FU;
        case 2:
            return 0x1FU;
        case 3:
            return 0x0FU;
        default:
            return 0x07U;
    }
}

[[nodiscard]] Decoded decode_one(std::string_view text, std::size_t offset) noexcept {
    Decoded result;
    const auto byte = [&text](std::size_t index) -> std::uint8_t {
        return static_cast<std::uint8_t>(text[index]);
    };

    const std::uint8_t first = byte(offset);
    const std::size_t remaining = text.size() - offset;

    const Sequence* chosen = nullptr;
    for (const Sequence& candidate : kSequences) {
        if ((first & candidate.mask) == candidate.lead) {
            chosen = &candidate;
            break;
        }
    }
    if (chosen == nullptr) {
        result.status = Utf8Status::InvalidSequence;
        result.length = 1;
        return result;
    }
    if (remaining < chosen->length) {
        result.status = Utf8Status::InvalidSequence;
        result.length = remaining == 0 ? 1 : remaining;
        return result;
    }

    std::uint32_t code_point = static_cast<std::uint32_t>(first & leading_value_mask(chosen->length));
    for (std::size_t index = 1; index < chosen->length; ++index) {
        const std::uint8_t continuation = byte(offset + index);
        if ((continuation & 0xC0U) != 0x80U) {
            result.status = Utf8Status::InvalidSequence;
            result.length = index;
            return result;
        }
        code_point = (code_point << 6U) | static_cast<std::uint32_t>(continuation & 0x3FU);
    }

    if (code_point < chosen->minimum) {
        result.status = Utf8Status::OverlongEncoding;
        result.length = chosen->length;
        return result;
    }
    if (code_point >= 0xD800U && code_point <= 0xDFFFU) {
        result.status = Utf8Status::SurrogateCodePoint;
        result.length = chosen->length;
        return result;
    }
    if (code_point > 0x10FFFFU) {
        result.status = Utf8Status::OutOfRange;
        result.length = chosen->length;
        return result;
    }

    result.code_point = code_point;
    result.length = chosen->length;
    result.status = Utf8Status::Valid;
    return result;
}

/// Code points the registry refuses to store. Control and formatting characters
/// are excluded because they let two different identifiers render identically (a
/// trojan-source style ambiguity) or corrupt terminal and log output.
[[nodiscard]] bool is_disallowed_code_point(std::uint32_t code_point) noexcept {
    if (code_point < 0x20U) {
        return code_point != 0x09U && code_point != 0x0AU && code_point != 0x0DU;
    }
    if (code_point == 0x7FU) {
        return true;  // DEL
    }
    if (code_point >= 0x80U && code_point <= 0x9FU) {
        return true;  // C1 controls
    }
    if (code_point == 0x00A0U || code_point == 0x3000U) {
        // Non-breaking space and ideographic space fold to a plain space in
        // comparison keys, so storing them verbatim would create two spellings of
        // one identifier. They are rejected instead of silently normalised.
        return true;
    }
    if (code_point == 0x00ADU || code_point == 0x200BU) {
        return true;  // soft hyphen, zero width space
    }
    if (code_point == 0x200EU || code_point == 0x200FU) {
        return true;  // left/right-to-left mark
    }
    if (code_point >= 0x202AU && code_point <= 0x202EU) {
        return true;  // bidi embedding and override controls
    }
    if (code_point == 0x2060U || (code_point >= 0x2066U && code_point <= 0x2069U)) {
        return true;  // word joiner, bidi isolates
    }
    return code_point == 0xFEFFU;  // byte order mark
}

[[nodiscard]] Utf8Status validate(std::string_view text, bool allow_empty, bool allow_newlines) noexcept {
    if (text.empty()) {
        return allow_empty ? Utf8Status::Valid : Utf8Status::Empty;
    }
    std::size_t offset = 0;
    while (offset < text.size()) {
        const Decoded decoded = decode_one(text, offset);
        if (decoded.status != Utf8Status::Valid) {
            return decoded.status;
        }
        if (is_disallowed_code_point(decoded.code_point)) {
            const bool newline = decoded.code_point == 0x0AU || decoded.code_point == 0x0DU;
            if (!(newline && allow_newlines)) {
                return Utf8Status::DisallowedCodePoint;
            }
        }
        offset += decoded.length;
    }
    return Utf8Status::Valid;
}

}  // namespace

std::string_view to_string(Utf8Status status) noexcept {
    switch (status) {
        case Utf8Status::Valid:
            return "valid";
        case Utf8Status::Empty:
            return "empty";
        case Utf8Status::InvalidSequence:
            return "invalid_utf8_sequence";
        case Utf8Status::OverlongEncoding:
            return "overlong_utf8_encoding";
        case Utf8Status::SurrogateCodePoint:
            return "utf16_surrogate_code_point";
        case Utf8Status::OutOfRange:
            return "code_point_above_u10ffff";
        case Utf8Status::DisallowedCodePoint:
            return "disallowed_control_or_formatting_code_point";
    }
    return "unknown";
}

Utf8Status validate_utf8(std::string_view text) noexcept {
    return validate(text, true, true);
}

Utf8Status validate_utf8(std::string_view text, bool allow_empty) noexcept {
    return validate(text, allow_empty, true);
}

std::size_t utf8_code_point_count(std::string_view text) noexcept {
    std::size_t count = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const Decoded decoded = decode_one(text, offset);
        offset += decoded.length == 0 ? 1 : decoded.length;
        ++count;
    }
    return count;
}

std::string make_comparison_key(std::string_view text) {
    std::string result;
    result.reserve(text.size());

    bool pending_space = false;
    bool produced_any = false;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const Decoded decoded = decode_one(text, offset);
        if (decoded.status != Utf8Status::Valid) {
            // Structurally invalid input never reaches this function through a
            // validated constructor; if it does, the bytes are copied verbatim so
            // the key stays deterministic and still distinguishes inputs.
            result.push_back(text[offset]);
            offset += decoded.length == 0 ? 1 : decoded.length;
            continue;
        }
        offset += decoded.length;

        std::uint32_t code_point = decoded.code_point;
        if (code_point == 0x3000U || code_point == 0x00A0U || code_point == 0x2007U || code_point == 0x202FU) {
            code_point = 0x20U;
        } else if (code_point >= 0xFF01U && code_point <= 0xFF5EU) {
            // Full-width ASCII compatibility fold: the substitution class that can
            // turn one vendor identifier into another's visual twin.
            code_point = code_point - 0xFF01U + 0x21U;
        } else if (code_point == 0xFEFFU || code_point == 0x200BU || code_point == 0x00ADU) {
            continue;  // invisible: contributes nothing to the key
        }

        const bool is_space =
            code_point == 0x20U || code_point == 0x09U || code_point == 0x0AU || code_point == 0x0DU;
        if (is_space) {
            if (produced_any) {
                pending_space = true;
            }
            continue;
        }
        if (pending_space) {
            result.push_back(' ');
            pending_space = false;
        }
        if (code_point < 0x80U) {
            char character = static_cast<char>(code_point);
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
            result.push_back(character);
        } else if (code_point <= 0x7FFU) {
            result.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else if (code_point <= 0xFFFFU) {
            result.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
            result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else {
            result.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
            result.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        }
        produced_any = true;
    }
    return result;
}

bool is_ascii_foldable(std::string_view text) noexcept {
    for (const char character : text) {
        if (static_cast<unsigned char>(character) >= 0x80U) {
            return false;
        }
    }
    return true;
}

bool is_identity_namespace(std::string_view text) noexcept {
    if (text.empty() || text.size() > limits::kMaxCapabilityNamespaceBytes) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        const bool alpha_numeric = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        if (index == 0) {
            if (!(character >= 'a' && character <= 'z')) {
                return false;
            }
            continue;
        }
        if (index + 1 == text.size()) {
            if (!alpha_numeric) {
                return false;
            }
            continue;
        }
        if (!alpha_numeric && character != '-') {
            return false;
        }
    }
    return true;
}

bool is_dotted_identity_path(std::string_view text) noexcept {
    if (text.empty() || text.size() > limits::kMaxExternalObjectIdBytes) {
        return false;
    }
    std::size_t segment_start = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index != text.size() && text[index] != '.') {
            continue;
        }
        const std::string_view segment = text.substr(segment_start, index - segment_start);
        if (segment.empty() || segment.size() > 64) {
            return false;
        }
        for (std::size_t position = 0; position < segment.size(); ++position) {
            const char character = segment[position];
            const bool alpha_numeric =
                (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
            const bool at_edge = position == 0 || position + 1 == segment.size();
            if (at_edge) {
                if (!alpha_numeric) {
                    return false;
                }
                continue;
            }
            if (!alpha_numeric && character != '_' && character != '-') {
                return false;
            }
        }
        segment_start = index + 1;
    }
    return true;
}

bool is_serial_number_text(std::string_view text) noexcept {
    if (text.empty() || text.size() > limits::kMaxSerialNumberBytes) {
        return false;
    }
    // Newlines are rejected as well as other controls: a serial appears in
    // line-oriented output and in framed records, and a newline inside one would
    // make the field boundary ambiguous.
    if (text.find('\n') != std::string_view::npos || text.find('\r') != std::string_view::npos) {
        return false;
    }
    if (validate(text, false, false) != Utf8Status::Valid) {
        return false;
    }
    // Framing separators are excluded so a serial cannot forge the component
    // separator used by SerialIdentity::parse.
    return text.find("//") == std::string_view::npos;
}

bool is_display_text(std::string_view text, std::size_t max_bytes) noexcept {
    if (text.empty() || text.size() > max_bytes) {
        return false;
    }
    return validate(text, false, false) == Utf8Status::Valid;
}

bool is_label_key(std::string_view text) noexcept {
    if (text.empty() || text.size() > 63) {
        return false;
    }
    if (!(text.front() >= 'a' && text.front() <= 'z')) {
        return false;
    }
    for (const char character : text) {
        const bool allowed = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                             character == '_' || character == '.' || character == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

std::optional<std::uint64_t> parse_unsigned_decimal(std::string_view text) noexcept {
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    if (text.size() > 1 && text.front() == '0') {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return std::nullopt;
        }
        value = value * 10U + digit;
    }
    return value;
}

std::string json_escape(std::string_view text) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(text.size() + 8);
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (raw) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (byte < 0x20U) {
                    result += "\\u00";
                    result.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
                    result.push_back(kHexDigits[byte & 0x0FU]);
                } else {
                    result.push_back(raw);
                }
                break;
        }
    }
    return result;
}

}  // namespace asset_registry
