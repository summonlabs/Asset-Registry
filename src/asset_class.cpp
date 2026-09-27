// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_registry/asset_class.hpp"

#include <array>

namespace asset_registry {
namespace {

struct ClassEntry {
    AssetClass value;
    std::string_view token;
    std::string_view description;
};

// Token spellings are part of the durable and export schema. They are additive
// only: a token is never renamed or reused.
constexpr std::array<ClassEntry, 24> kClasses = {{
    {AssetClass::Unknown, "unknown", "unclassified physical object"},
    {AssetClass::Server, "server", "compute server or node chassis"},
    {AssetClass::AcceleratorEnclosure, "accelerator_enclosure", "accelerator enclosure or expansion chassis"},
    {AssetClass::AcceleratorModule, "accelerator_module", "individually installed accelerator module"},
    {AssetClass::Switch, "switch", "network switch"},
    {AssetClass::NetworkInterface, "network_interface", "network interface card or transceiver"},
    {AssetClass::PatchPanel, "patch_panel", "structured cabling patch panel"},
    {AssetClass::PowerDistributionUnit, "power_distribution_unit", "rack or row power distribution unit"},
    {AssetClass::Busway, "busway", "overhead or underfloor power busway"},
    {AssetClass::UninterruptiblePowerSupply, "uninterruptible_power_supply", "uninterruptible power supply"},
    {AssetClass::PowerSupply, "power_supply", "rectifier or power shelf"},
    {AssetClass::Generator, "generator", "standby generator"},
    {AssetClass::AutomaticTransferSwitch, "automatic_transfer_switch", "automatic transfer switch"},
    {AssetClass::CoolingUnit, "cooling_unit", "computer room air conditioner or chiller"},
    {AssetClass::CoolingDistributionUnit, "cooling_distribution_unit", "coolant distribution unit"},
    {AssetClass::AirHandler, "air_handler", "air handling unit"},
    {AssetClass::Rack, "rack", "equipment rack or cabinet"},
    {AssetClass::StorageAppliance, "storage_appliance", "storage array or appliance"},
    {AssetClass::Sensor, "sensor", "point sensor"},
    {AssetClass::EnvironmentalMonitor, "environmental_monitor", "environmental monitoring unit"},
    {AssetClass::KvmConsole, "kvm_console", "keyboard/video/mouse console or serial aggregator"},
    {AssetClass::StructuredCablingRun, "structured_cabling_run", "structured cabling run"},
    {AssetClass::FireSuppressionUnit, "fire_suppression_unit", "fire detection or suppression unit"},
    {AssetClass::External, "external", "object owned by an external system, recorded for reference only"},
}};

}  // namespace

std::string_view to_string(AssetClass value) noexcept {
    for (const ClassEntry& entry : kClasses) {
        if (entry.value == value) {
            return entry.token;
        }
    }
    return "unknown";
}

std::string_view describe(AssetClass value) noexcept {
    for (const ClassEntry& entry : kClasses) {
        if (entry.value == value) {
            return entry.description;
        }
    }
    return "unclassified physical object";
}

std::optional<AssetClass> asset_class_from_token(std::string_view token) noexcept {
    for (const ClassEntry& entry : kClasses) {
        if (entry.token == token) {
            return entry.value;
        }
    }
    return std::nullopt;
}

const std::optional<AssetClass>* known_asset_classes() noexcept {
    static const std::array<std::optional<AssetClass>, 23> kKnown = {{
        AssetClass::Server,
        AssetClass::AcceleratorEnclosure,
        AssetClass::AcceleratorModule,
        AssetClass::Switch,
        AssetClass::NetworkInterface,
        AssetClass::PatchPanel,
        AssetClass::PowerDistributionUnit,
        AssetClass::Busway,
        AssetClass::UninterruptiblePowerSupply,
        AssetClass::PowerSupply,
        AssetClass::Generator,
        AssetClass::AutomaticTransferSwitch,
        AssetClass::CoolingUnit,
        AssetClass::CoolingDistributionUnit,
        AssetClass::AirHandler,
        AssetClass::Rack,
        AssetClass::StorageAppliance,
        AssetClass::Sensor,
        AssetClass::EnvironmentalMonitor,
        AssetClass::KvmConsole,
        AssetClass::StructuredCablingRun,
        AssetClass::FireSuppressionUnit,
        AssetClass::External,
    }};
    return kKnown.data();
}

std::size_t known_asset_class_count() noexcept {
    return 23;
}

bool is_known(AssetClass value) noexcept {
    return value != AssetClass::Unknown;
}

bool is_power_class(AssetClass value) noexcept {
    switch (value) {
        case AssetClass::PowerDistributionUnit:
        case AssetClass::Busway:
        case AssetClass::UninterruptiblePowerSupply:
        case AssetClass::PowerSupply:
        case AssetClass::Generator:
        case AssetClass::AutomaticTransferSwitch:
            return true;
        default:
            return false;
    }
}

bool is_cooling_class(AssetClass value) noexcept {
    switch (value) {
        case AssetClass::CoolingUnit:
        case AssetClass::CoolingDistributionUnit:
        case AssetClass::AirHandler:
            return true;
        default:
            return false;
    }
}

}  // namespace asset_registry
