// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_registry/replacement.hpp"

#include <array>

namespace asset_registry {
namespace {

struct CauseEntry {
    ReplacementCause value;
    std::string_view token;
};

constexpr std::array<CauseEntry, 8> kCauses = {{
    {ReplacementCause::Unknown, "unknown"},
    {ReplacementCause::Failure, "failure"},
    {ReplacementCause::Upgrade, "upgrade"},
    {ReplacementCause::Refresh, "refresh"},
    {ReplacementCause::Relocation, "relocation"},
    {ReplacementCause::Reconfiguration, "reconfiguration"},
    {ReplacementCause::EndOfLife, "end_of_life"},
    {ReplacementCause::WarrantyReturn, "warranty_return"},
}};

}  // namespace

std::string_view to_string(ReplacementCause value) noexcept {
    for (const CauseEntry& entry : kCauses) {
        if (entry.value == value) {
            return entry.token;
        }
    }
    return "unknown";
}

std::optional<ReplacementCause> replacement_cause_from_token(std::string_view token) noexcept {
    for (const CauseEntry& entry : kCauses) {
        if (entry.token == token) {
            return entry.value;
        }
    }
    return std::nullopt;
}

std::string_view to_string(LineageTermination value) noexcept {
    switch (value) {
        case LineageTermination::ReachedOrigin:
            return "reached_origin";
        case LineageTermination::DepthLimitReached:
            return "depth_limit_reached";
    }
    return "reached_origin";
}

std::optional<AssetId> LineageChain::origin() const noexcept {
    if (predecessors.empty()) {
        return std::nullopt;
    }
    return predecessors.back();
}

}  // namespace asset_registry
