// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_registry/serial_identity.hpp"

#include "asset_registry/text.hpp"

namespace asset_registry {
namespace {

constexpr char kComponentSeparator[] = "//";

[[nodiscard]] std::optional<std::string> validate_component(std::string_view text, std::size_t max_bytes,
                                                            std::size_t max_code_points) {
    if (text.empty()) {
        return std::nullopt;
    }
    if (text.size() > max_bytes || utf8_code_point_count(text) > max_code_points) {
        return std::nullopt;
    }
    if (validate_utf8(text, false) != Utf8Status::Valid) {
        return std::nullopt;
    }
    if (text.find(kComponentSeparator) != std::string_view::npos) {
        return std::nullopt;
    }
    return std::string(text);
}

}  // namespace

std::optional<ManufacturerIdentity> ManufacturerIdentity::create(std::string_view name) {
    const auto validated = validate_component(name, limits::kMaxManufacturerBytes, limits::kManufacturerCodePoints);
    if (!validated.has_value()) {
        return std::nullopt;
    }
    ManufacturerIdentity identity;
    identity.name_ = *validated;
    identity.comparison_key_ = make_comparison_key(identity.name_);
    if (identity.comparison_key_.empty()) {
        // A manufacturer name that folds to nothing (all whitespace or all
        // invisible characters) carries no identity information.
        return std::nullopt;
    }
    return identity;
}

std::optional<SerialNumber> SerialNumber::create(std::string_view text) {
    if (!is_serial_number_text(text)) {
        return std::nullopt;
    }
    if (utf8_code_point_count(text) > limits::kSerialNumberCodePoints) {
        return std::nullopt;
    }
    SerialNumber serial;
    serial.text_ = std::string(text);
    serial.comparison_key_ = make_comparison_key(serial.text_);
    if (serial.comparison_key_.empty()) {
        return std::nullopt;
    }
    return serial;
}

std::optional<ModelIdentity> ModelIdentity::create(std::string_view text) {
    const auto validated = validate_component(text, limits::kMaxModelBytes, limits::kModelCodePoints);
    if (!validated.has_value()) {
        return std::nullopt;
    }
    ModelIdentity identity;
    identity.text_ = *validated;
    identity.comparison_key_ = make_comparison_key(identity.text_);
    if (identity.comparison_key_.empty()) {
        return std::nullopt;
    }
    return identity;
}

std::optional<SerialIdentity> SerialIdentity::create(const ManufacturerIdentity& manufacturer,
                                                     const SerialNumber& serial,
                                                     const std::optional<ModelIdentity>& model) {
    if (manufacturer.empty() || serial.empty()) {
        return std::nullopt;
    }
    SerialIdentity identity;
    identity.manufacturer_ = manufacturer;
    identity.serial_ = serial;
    identity.model_ = model;
    identity.canonical_key_.reserve(manufacturer.comparison_key().size() + 1 + serial.comparison_key().size());
    identity.canonical_key_ = manufacturer.comparison_key();
    identity.canonical_key_ += '/';
    identity.canonical_key_ += serial.comparison_key();
    return identity;
}

std::optional<SerialIdentity> SerialIdentity::parse(std::string_view text) {
    if (text.empty() || text.size() > limits::kMaxManufacturerBytes + limits::kMaxSerialNumberBytes + 8) {
        return std::nullopt;
    }
    const std::size_t first = text.find(kComponentSeparator);
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view manufacturer_text = text.substr(0, first);
    const std::string_view remainder = text.substr(first + 2);

    std::string_view serial_text = remainder;
    std::optional<std::string_view> model_text;
    const std::size_t second = remainder.find(kComponentSeparator);
    if (second != std::string_view::npos) {
        serial_text = remainder.substr(0, second);
        model_text = remainder.substr(second + 2);
        if (model_text->empty()) {
            return std::nullopt;
        }
    }
    if (serial_text.empty()) {
        return std::nullopt;
    }

    const auto manufacturer = ManufacturerIdentity::create(manufacturer_text);
    if (!manufacturer.has_value()) {
        return std::nullopt;
    }
    const auto serial = SerialNumber::create(serial_text);
    if (!serial.has_value()) {
        return std::nullopt;
    }
    std::optional<ModelIdentity> model;
    if (model_text.has_value()) {
        model = ModelIdentity::create(*model_text);
        if (!model.has_value()) {
            return std::nullopt;
        }
    }
    return create(*manufacturer, *serial, model);
}

bool SerialIdentity::model_conflicts_with(const SerialIdentity& other) const noexcept {
    if (!model_.has_value() || !other.model_.has_value()) {
        return false;
    }
    return model_->comparison_key() != other.model_->comparison_key();
}

std::string SerialIdentity::to_string() const {
    if (canonical_key_.empty()) {
        return {};
    }
    std::string result = manufacturer_.name();
    result += kComponentSeparator;
    result += serial_.text();
    if (model_.has_value()) {
        result += kComponentSeparator;
        result += model_->text();
    }
    return result;
}

}  // namespace asset_registry
