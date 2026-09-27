// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// SerialIdentity: the manufacturer-supplied physical identity of an asset.
// This is the strongest evidence the registry has that a physical object is the
// same object across imports, and it is therefore uniqueness-constrained.
//
// The stored spelling is preserved exactly as supplied. A separate canonical
// key is derived for collision detection so that "SN 1234" and "sn  1234" are
// recognised as the same physical claim regardless of formatting noise.

#ifndef ASSET_REGISTRY_SERIAL_IDENTITY_HPP
#define ASSET_REGISTRY_SERIAL_IDENTITY_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "asset_registry/error.hpp"

namespace asset_registry {

namespace limits {

inline constexpr std::size_t kMaxManufacturerBytes = 128;
inline constexpr std::size_t kMaxSerialNumberBytes = 128;
inline constexpr std::size_t kMaxModelBytes = 128;
inline constexpr std::size_t kManufacturerCodePoints = 64;
inline constexpr std::size_t kSerialNumberCodePoints = 64;
inline constexpr std::size_t kModelCodePoints = 64;

}  // namespace limits

/// Vendor or standards-body manufacturer name, e.g. "Acme Compute".
class ASSET_REGISTRY_API ManufacturerIdentity {
public:
    ManufacturerIdentity() = default;

    [[nodiscard]] static std::optional<ManufacturerIdentity> create(std::string_view name);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    /// Folded comparison form derived at construction; never re-derived later.
    [[nodiscard]] const std::string& comparison_key() const noexcept { return comparison_key_; }
    [[nodiscard]] bool empty() const noexcept { return name_.empty(); }

    friend bool operator==(const ManufacturerIdentity& lhs, const ManufacturerIdentity& rhs) noexcept {
        return lhs.comparison_key_ == rhs.comparison_key_;
    }
    friend bool operator!=(const ManufacturerIdentity& lhs, const ManufacturerIdentity& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend bool operator<(const ManufacturerIdentity& lhs, const ManufacturerIdentity& rhs) noexcept {
        return lhs.comparison_key_ < rhs.comparison_key_;
    }

private:
    std::string name_;
    std::string comparison_key_;
};

/// Manufacturer serial number. Case is significant in the stored spelling
/// because some vendors use it, but the comparison key is case-folded.
class ASSET_REGISTRY_API SerialNumber {
public:
    SerialNumber() = default;

    [[nodiscard]] static std::optional<SerialNumber> create(std::string_view text);

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] const std::string& comparison_key() const noexcept { return comparison_key_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

    friend bool operator==(const SerialNumber& lhs, const SerialNumber& rhs) noexcept {
        return lhs.comparison_key_ == rhs.comparison_key_;
    }
    friend bool operator!=(const SerialNumber& lhs, const SerialNumber& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend bool operator<(const SerialNumber& lhs, const SerialNumber& rhs) noexcept {
        return lhs.comparison_key_ < rhs.comparison_key_;
    }

private:
    std::string text_;
    std::string comparison_key_;
};

/// Vendor model or part designation. Optional evidence: it raises the
/// specificity of a collision report without being required for uniqueness.
class ASSET_REGISTRY_API ModelIdentity {
public:
    ModelIdentity() = default;

    [[nodiscard]] static std::optional<ModelIdentity> create(std::string_view text);

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] const std::string& comparison_key() const noexcept { return comparison_key_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

    friend bool operator==(const ModelIdentity& lhs, const ModelIdentity& rhs) noexcept {
        return lhs.comparison_key_ == rhs.comparison_key_;
    }
    friend bool operator!=(const ModelIdentity& lhs, const ModelIdentity& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend bool operator<(const ModelIdentity& lhs, const ModelIdentity& rhs) noexcept {
        return lhs.comparison_key_ < rhs.comparison_key_;
    }

private:
    std::string text_;
    std::string comparison_key_;
};

/// Canonical collision key: manufacturer key + unit separator + serial key.
///
/// The model is deliberately excluded. Two records that claim the same
/// manufacturer and serial describe the same physical object even if the model
/// strings disagree, and that disagreement is a data-quality conflict the
/// registry must surface rather than use to authorise a duplicate record.
class ASSET_REGISTRY_API SerialIdentity {
public:
    SerialIdentity() = default;

    /// Builds an identity from validated parts. `manufacturer` and `serial` are
    /// both required; `model` is optional.
    [[nodiscard]] static std::optional<SerialIdentity> create(const ManufacturerIdentity& manufacturer,
                                                             const SerialNumber& serial,
                                                             const std::optional<ModelIdentity>& model);

    /// Parses "manufacturer//serial" or "manufacturer//serial//model". The
    /// separator is chosen because it cannot appear in a valid serial: control
    /// characters and '/' runs are rejected by the component validators.
    [[nodiscard]] static std::optional<SerialIdentity> parse(std::string_view text);

    [[nodiscard]] const ManufacturerIdentity& manufacturer() const noexcept { return manufacturer_; }
    [[nodiscard]] const SerialNumber& serial() const noexcept { return serial_; }
    /// Absent when the vendor metadata did not supply a model. An absent model
    /// is unknown data, not matching data.
    [[nodiscard]] const std::optional<ModelIdentity>& model() const noexcept { return model_; }

    [[nodiscard]] const std::string& canonical_key() const noexcept { return canonical_key_; }
    [[nodiscard]] bool empty() const noexcept { return canonical_key_.empty(); }

    /// True when both identities carry a model and the models disagree.
    /// The registry reports this as a data-quality observation; it does not
    /// authorise a second record.
    [[nodiscard]] bool model_conflicts_with(const SerialIdentity& other) const noexcept;

    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const SerialIdentity& lhs, const SerialIdentity& rhs) noexcept {
        return lhs.canonical_key_ == rhs.canonical_key_;
    }
    friend bool operator!=(const SerialIdentity& lhs, const SerialIdentity& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend bool operator<(const SerialIdentity& lhs, const SerialIdentity& rhs) noexcept {
        return lhs.canonical_key_ < rhs.canonical_key_;
    }

private:
    ManufacturerIdentity manufacturer_;
    SerialNumber serial_;
    std::optional<ModelIdentity> model_;
    std::string canonical_key_;
};

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_SERIAL_IDENTITY_HPP
