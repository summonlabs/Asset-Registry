// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// AssetClass: the typed classification of a physical infrastructure object.
// The class is physical-identity data: it is fixed at registration and can only
// change through an explicit class reclassification, never through a metadata
// update, because downstream consumers key physical behaviour off it.

#ifndef ASSET_REGISTRY_ASSET_CLASS_HPP
#define ASSET_REGISTRY_ASSET_CLASS_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "asset_registry/error.hpp"

namespace asset_registry {

enum class AssetClass : std::uint16_t {
    Unknown = 0,
    Server = 1,
    AcceleratorEnclosure = 2,
    AcceleratorModule = 3,
    Switch = 4,
    NetworkInterface = 5,
    PatchPanel = 6,
    PowerDistributionUnit = 7,
    Busway = 8,
    UninterruptiblePowerSupply = 9,
    PowerSupply = 10,
    Generator = 11,
    AutomaticTransferSwitch = 12,
    CoolingUnit = 13,
    CoolingDistributionUnit = 14,
    AirHandler = 15,
    Rack = 16,
    StorageAppliance = 17,
    Sensor = 18,
    EnvironmentalMonitor = 19,
    KvmConsole = 20,
    StructuredCablingRun = 21,
    FireSuppressionUnit = 22,
    External = 23,
};

/// Stable wire token, e.g. "power_distribution_unit". Tokens are part of the
/// durable and export schema and must never be renamed.
[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(AssetClass value) noexcept;

/// Canonical description used by CLI listings and diagnostics.
[[nodiscard]] ASSET_REGISTRY_API std::string_view describe(AssetClass value) noexcept;

/// Strict parse. Returns std::nullopt for an unrecognised token; unrecognised
/// vendor class tokens are never silently coerced to Unknown.
[[nodiscard]] ASSET_REGISTRY_API std::optional<AssetClass> asset_class_from_token(std::string_view token) noexcept;

/// Every known (non-Unknown) class in ascending numeric order. Iteration order
/// of this sequence is part of the public contract.
[[nodiscard]] ASSET_REGISTRY_API const std::optional<AssetClass>* known_asset_classes() noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::size_t known_asset_class_count() noexcept;

/// True when the class is a known physical class rather than Unknown.
[[nodiscard]] ASSET_REGISTRY_API bool is_known(AssetClass value) noexcept;

/// True for classes whose primary purpose is electrical power delivery.
[[nodiscard]] ASSET_REGISTRY_API bool is_power_class(AssetClass value) noexcept;

/// True for classes whose primary purpose is heat removal.
[[nodiscard]] ASSET_REGISTRY_API bool is_cooling_class(AssetClass value) noexcept;

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_ASSET_CLASS_HPP
