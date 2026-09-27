// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "json_parse.hpp"

#include <limits>

#include "asset_registry/text.hpp"

namespace asset_registry::internal {
namespace {

class Parser {
public:
    Parser(std::string_view document, std::uint32_t max_depth, std::uint64_t max_values)
        : document_(document), max_depth_(max_depth), max_values_(max_values) {}

    [[nodiscard]] Outcome<JsonValuePtr> parse() {
        skip_whitespace();
        auto root = parse_value(0);
        if (!root) {
            return root.error();
        }
        skip_whitespace();
        if (offset_ != document_.size()) {
            return fail("the document carries trailing content after the root value");
        }
        return root.value();
    }

private:
    /// Builds the rejection for the current offset. Returned as an Error so the
    /// caller's Outcome type converts implicitly.
    [[nodiscard]] Error fail(std::string message) const {
        std::string full = "document is not valid JSON at byte ";
        full += std::to_string(offset_);
        full += ": ";
        full += message;
        return Error(ErrorCode::ImportRecordInvalid, std::move(full));
    }

    void skip_whitespace() noexcept {
        while (offset_ < document_.size()) {
            const char character = document_[offset_];
            if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
                ++offset_;
                continue;
            }
            break;
        }
    }

    [[nodiscard]] bool consume(char expected) noexcept {
        if (offset_ < document_.size() && document_[offset_] == expected) {
            ++offset_;
            return true;
        }
        return false;
    }

    [[nodiscard]] Outcome<JsonValuePtr> parse_value(std::uint32_t depth) {
        if (depth > max_depth_) {
            return fail("nesting depth exceeds the permitted bound");
        }
        if (++values_ > max_values_) {
            return fail("the document carries more values than the permitted bound");
        }
        if (offset_ >= document_.size()) {
            return fail("unexpected end of input where a value was expected");
        }
        switch (document_[offset_]) {
            case '{':
                return parse_object(depth);
            case '[':
                return parse_array(depth);
            case '"': {
                auto value = std::make_shared<JsonValue>();
                value->kind = JsonValue::Kind::String;
                auto text = parse_string();
                if (!text) {
                    return text.error();
                }
                value->text = std::move(text).value();
                return JsonValuePtr(std::move(value));
            }
            case 't':
                if (document_.substr(offset_, 4) == "true") {
                    offset_ += 4;
                    auto value = std::make_shared<JsonValue>();
                    value->kind = JsonValue::Kind::Bool;
                    value->boolean = true;
                    return JsonValuePtr(std::move(value));
                }
                return fail("unexpected token");
            case 'f':
                if (document_.substr(offset_, 5) == "false") {
                    offset_ += 5;
                    auto value = std::make_shared<JsonValue>();
                    value->kind = JsonValue::Kind::Bool;
                    value->boolean = false;
                    return JsonValuePtr(std::move(value));
                }
                return fail("unexpected token");
            case 'n':
                if (document_.substr(offset_, 4) == "null") {
                    offset_ += 4;
                    auto value = std::make_shared<JsonValue>();
                    value->kind = JsonValue::Kind::Null;
                    return JsonValuePtr(std::move(value));
                }
                return fail("unexpected token");
            default:
                return parse_number();
        }
    }

    [[nodiscard]] Outcome<JsonValuePtr> parse_object(std::uint32_t depth) {
        auto value = std::make_shared<JsonValue>();
        value->kind = JsonValue::Kind::Object;
        ++offset_;  // consume '{'
        skip_whitespace();
        if (consume('}')) {
            return JsonValuePtr(std::move(value));
        }
        while (true) {
            skip_whitespace();
            if (offset_ >= document_.size() || document_[offset_] != '"') {
                return fail("an object member name must be a string");
            }
            auto name = parse_string();
            if (!name) {
                return name.error();
            }
            for (const JsonMember& existing : value->members) {
                if (existing.name == name.value()) {
                    return fail("the object repeats the member name: " + name.value());
                }
            }
            skip_whitespace();
            if (!consume(':')) {
                return fail("an object member name must be followed by a colon");
            }
            skip_whitespace();
            auto member = parse_value(depth + 1);
            if (!member) {
                return member.error();
            }
            value->members.push_back(JsonMember{std::move(name).value(), std::move(member).value()});
            skip_whitespace();
            if (consume(',')) {
                continue;
            }
            if (consume('}')) {
                return JsonValuePtr(std::move(value));
            }
            return fail("an object member must be followed by a comma or a closing brace");
        }
    }

    [[nodiscard]] Outcome<JsonValuePtr> parse_array(std::uint32_t depth) {
        auto value = std::make_shared<JsonValue>();
        value->kind = JsonValue::Kind::Array;
        ++offset_;  // consume '['
        skip_whitespace();
        if (consume(']')) {
            return JsonValuePtr(std::move(value));
        }
        while (true) {
            skip_whitespace();
            auto element = parse_value(depth + 1);
            if (!element) {
                return element.error();
            }
            value->elements.push_back(std::move(element).value());
            skip_whitespace();
            if (consume(',')) {
                continue;
            }
            if (consume(']')) {
                return JsonValuePtr(std::move(value));
            }
            return fail("an array element must be followed by a comma or a closing bracket");
        }
    }

    [[nodiscard]] Outcome<JsonValuePtr> parse_number() {
        bool negative = false;
        if (offset_ < document_.size() && document_[offset_] == '-') {
            negative = true;
            ++offset_;
        }
        const std::size_t digits_start = offset_;
        while (offset_ < document_.size() && document_[offset_] >= '0' && document_[offset_] <= '9') {
            ++offset_;
        }
        const std::string_view digits = document_.substr(digits_start, offset_ - digits_start);
        if (digits.empty()) {
            return fail("a number must carry at least one digit");
        }
        if (offset_ < document_.size()) {
            const char next = document_[offset_];
            if (next == '.' || next == 'e' || next == 'E') {
                return fail("only integers are accepted; fractions and exponents are rejected");
            }
        }
        if (negative) {
            // Signed values are only meaningful for timestamps, which are validated
            // against the representable range after parsing.
            std::int64_t magnitude = 0;
            for (const char digit : digits) {
                const auto value = static_cast<std::int64_t>(digit - '0');
                if (magnitude > (std::numeric_limits<std::int64_t>::max() - value) / 10) {
                    return fail("integer magnitude exceeds the representable range");
                }
                magnitude = magnitude * 10 + value;
            }
            if (magnitude == 0 && digits == "0") {
                auto value = std::make_shared<JsonValue>();
                value->kind = JsonValue::Kind::Int;
                value->signed_value = 0;
                return JsonValuePtr(std::move(value));
            }
            if (magnitude - 1 > std::numeric_limits<std::int64_t>::max()) {
                return fail("integer magnitude exceeds the representable range");
            }
            auto value = std::make_shared<JsonValue>();
            value->kind = JsonValue::Kind::Int;
            value->signed_value = -magnitude;
            return JsonValuePtr(std::move(value));
        }
        const auto parsed = parse_unsigned_decimal(digits);
        if (!parsed.has_value()) {
            return fail("integer is not in canonical decimal form or exceeds the representable range");
        }
        auto value = std::make_shared<JsonValue>();
        value->kind = JsonValue::Kind::UInt;
        value->unsigned_value = *parsed;
        return JsonValuePtr(std::move(value));
    }

    [[nodiscard]] Outcome<std::string> parse_string() {
        ++offset_;  // consume opening quote
        std::string result;
        while (true) {
            if (offset_ >= document_.size()) {
                return fail("unterminated string");
            }
            const char character = document_[offset_];
            if (character == '"') {
                ++offset_;
                break;
            }
            if (character == '\\') {
                ++offset_;
                if (offset_ >= document_.size()) {
                    return fail("unterminated escape sequence");
                }
                const char escape = document_[offset_++];
                switch (escape) {
                    case '"':
                        result.push_back('"');
                        break;
                    case '\\':
                        result.push_back('\\');
                        break;
                    case '/':
                        result.push_back('/');
                        break;
                    case 'b':
                        result.push_back('\b');
                        break;
                    case 'f':
                        result.push_back('\f');
                        break;
                    case 'n':
                        result.push_back('\n');
                        break;
                    case 'r':
                        result.push_back('\r');
                        break;
                    case 't':
                        result.push_back('\t');
                        break;
                    case 'u': {
                        auto code_point = parse_hex4();
                        if (!code_point) {
                            return code_point.error();
                        }
                        std::uint32_t value = code_point.value();
                        if (value >= 0xD800U && value <= 0xDBFFU) {
                            if (offset_ + 1 >= document_.size() || document_[offset_] != '\\' ||
                                document_[offset_ + 1] != 'u') {
                                return fail("a high surrogate must be followed by a low surrogate escape");
                            }
                            offset_ += 2;
                            auto low = parse_hex4();
                            if (!low) {
                                return low.error();
                            }
                            if (low.value() < 0xDC00U || low.value() > 0xDFFFU) {
                                return fail("a high surrogate must be followed by a low surrogate escape");
                            }
                            value = 0x10000U + ((value - 0xD800U) << 10U) + (low.value() - 0xDC00U);
                        } else if (value >= 0xDC00U && value <= 0xDFFFU) {
                            return fail("a low surrogate appeared without a preceding high surrogate");
                        }
                        append_utf8(result, value);
                        break;
                    }
                    default:
                        return fail("unrecognised escape sequence");
                }
                continue;
            }
            if (static_cast<unsigned char>(character) < 0x20U) {
                return fail("a control character must be escaped inside a string");
            }
            result.push_back(character);
            ++offset_;
        }
        if (validate_utf8(result, true) != Utf8Status::Valid) {
            return fail("string content is not structurally valid UTF-8");
        }
        return result;
    }

    [[nodiscard]] Outcome<std::uint32_t> parse_hex4() {
        if (offset_ + 4 > document_.size()) {
            return fail("truncated \\u escape");
        }
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char character = document_[offset_++];
            value <<= 4U;
            if (character >= '0' && character <= '9') {
                value |= static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                value |= static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                return fail("\\u escape must carry four hexadecimal digits");
            }
        }
        return value;
    }

    static void append_utf8(std::string& target, std::uint32_t code_point) {
        if (code_point < 0x80U) {
            target.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800U) {
            target.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
            target.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else if (code_point < 0x10000U) {
            target.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
            target.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            target.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else {
            target.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
            target.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
            target.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            target.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        }
    }

    std::string_view document_;
    std::size_t offset_ = 0;
    std::uint32_t max_depth_ = 32;
    std::uint64_t max_values_ = 1'000'000;
    std::uint64_t values_ = 0;
};

}  // namespace

Outcome<JsonValuePtr> parse_json(std::string_view document, std::uint32_t max_depth, std::uint64_t max_values) {
    Parser parser(document, max_depth, max_values);
    return parser.parse();
}

}  // namespace asset_registry::internal
