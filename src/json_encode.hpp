// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Internal: deterministic JSON emission.
//
// The writer owns indentation and comma placement so that callers cannot produce
// a document whose shape depends on call order. Only the object member order the
// caller chooses is under caller control, and every caller in this repository
// emits members in a documented fixed order.

#ifndef ASSET_REGISTRY_INTERNAL_JSON_ENCODE_HPP
#define ASSET_REGISTRY_INTERNAL_JSON_ENCODE_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace asset_registry::internal {

class JsonWriter {
public:
    /// `indent` of zero emits a single-line document.
    explicit JsonWriter(std::uint32_t indent = 2) : indent_(indent) {}

    void begin_object();
    void end_object();
    void begin_array();
    void end_array();

    /// Emits a member key. Must be called immediately before a value inside an
    /// object.
    void key(std::string_view name);
    void value_string(std::string_view value);
    void value_unsigned(std::uint64_t value);
    void value_bool(bool value);
    void value_null();

    [[nodiscard]] const std::string& str() const noexcept { return buffer_; }
    [[nodiscard]] std::string take() { return std::move(buffer_); }
    void reserve(std::size_t bytes) { buffer_.reserve(bytes); }
    /// Discards the accumulated document so the writer can be reused.
    void reset() {
        buffer_.clear();
        depth_ = 0;
        stack_.clear();
        pending_key_ = false;
        started_ = false;
    }

private:
    enum class Context : std::uint8_t {
        Root = 0,
        Object = 1,
        Array = 2,
    };

    void before_value();
    void push(Context context);
    void newline_indent();

    std::string buffer_;
    std::uint32_t indent_ = 2;
    std::uint32_t depth_ = 0;
    std::string stack_;
    bool pending_key_ = false;
    bool started_ = false;
};

}  // namespace asset_registry::internal

#endif  // ASSET_REGISTRY_INTERNAL_JSON_ENCODE_HPP
