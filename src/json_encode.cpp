// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "json_encode.hpp"

#include "asset_registry/text.hpp"

namespace asset_registry::internal {
namespace {

constexpr char kContextObject = 'O';
constexpr char kContextArray = 'A';

}  // namespace

void JsonWriter::push(Context context) {
    stack_.push_back(context == Context::Array ? kContextArray : kContextObject);
    ++depth_;
}

void JsonWriter::newline_indent() {
    if (indent_ == 0) {
        return;
    }
    buffer_.push_back('\n');
    buffer_.append(static_cast<std::size_t>(depth_) * indent_, ' ');
}

void JsonWriter::before_value() {
    if (stack_.empty()) {
        started_ = true;
        return;
    }
    if (stack_.back() == kContextObject) {
        // A value outside a member is a programming error. Emitting nothing keeps
        // the document well-formed rather than silently corrupting its shape; the
        // encoder tests assert that every value in this repository follows a key.
        pending_key_ = false;
        return;
    }
    if (!started_) {
        started_ = true;
        newline_indent();
        return;
    }
    buffer_.push_back(',');
    newline_indent();
}

void JsonWriter::begin_object() {
    before_value();
    buffer_.push_back('{');
    push(Context::Object);
    pending_key_ = false;
    started_ = false;
}

void JsonWriter::end_object() {
    const bool was_empty = !started_;
    if (!stack_.empty()) {
        stack_.pop_back();
        if (depth_ > 0) {
            --depth_;
        }
    }
    if (!was_empty) {
        newline_indent();
    }
    buffer_.push_back('}');
    pending_key_ = false;
    started_ = true;
}

void JsonWriter::begin_array() {
    before_value();
    buffer_.push_back('[');
    push(Context::Array);
    pending_key_ = false;
    started_ = false;
}

void JsonWriter::end_array() {
    const bool was_empty = !started_;
    if (!stack_.empty()) {
        stack_.pop_back();
        if (depth_ > 0) {
            --depth_;
        }
    }
    if (!was_empty) {
        newline_indent();
    }
    buffer_.push_back(']');
    pending_key_ = false;
    started_ = true;
}

void JsonWriter::key(std::string_view name) {
    if (!started_) {
        started_ = true;
    } else {
        buffer_.push_back(',');
    }
    newline_indent();
    buffer_.push_back('"');
    buffer_ += json_escape(name);
    buffer_.push_back('"');
    buffer_.push_back(':');
    if (indent_ != 0) {
        buffer_.push_back(' ');
    }
    pending_key_ = true;
}

void JsonWriter::value_string(std::string_view value) {
    before_value();
    buffer_.push_back('"');
    buffer_ += json_escape(value);
    buffer_.push_back('"');
    started_ = true;
}

// The null case exists so that an absent optional field is emitted explicitly as
// null rather than being omitted, which keeps the export schema fixed.
void JsonWriter::value_null() {
    before_value();
    buffer_ += "null";
    started_ = true;
}

void JsonWriter::value_unsigned(std::uint64_t value) {
    before_value();
    buffer_ += std::to_string(value);
    started_ = true;
}

void JsonWriter::value_bool(bool value) {
    before_value();
    buffer_ += value ? "true" : "false";
    started_ = true;
}

}  // namespace asset_registry::internal
