// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_registry/registry_policy.hpp"

#include <array>

namespace asset_registry {
namespace {

struct TokenEntry {
    std::string_view token;
};

template <typename Enum>
[[nodiscard]] std::optional<Enum> find_token(const std::pair<Enum, std::string_view>* entries, std::size_t count,
                                             std::string_view token) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (entries[index].second == token) {
            return entries[index].first;
        }
    }
    return std::nullopt;
}

constexpr std::array<std::pair<AssetIdMode, std::string_view>, 3> kAssetIdModes = {{
    {AssetIdMode::CallerSupplied, "caller_supplied"},
    {AssetIdMode::DerivedFromSerialIdentity, "derived_from_serial_identity"},
    {AssetIdMode::GeneratedRandom, "generated_random"},
}};

constexpr std::array<std::pair<SerialCollisionPolicy, std::string_view>, 2> kSerialCollisionPolicies = {{
    {SerialCollisionPolicy::Reject, "reject"},
    {SerialCollisionPolicy::AllowReplacementOfTerminal, "allow_replacement_of_terminal"},
}};

constexpr std::array<std::pair<IdentityReusePolicy, std::string_view>, 2> kIdentityReusePolicies = {{
    {IdentityReusePolicy::Forbid, "forbid"},
    {IdentityReusePolicy::AllowAfterTerminal, "allow_after_terminal"},
}};

constexpr std::array<std::pair<ModelConflictPolicy, std::string_view>, 2> kModelConflictPolicies = {{
    {ModelConflictPolicy::Report, "report"},
    {ModelConflictPolicy::Reject, "reject"},
}};

constexpr std::array<std::pair<RevisionCheckPolicy, std::string_view>, 2> kRevisionCheckPolicies = {{
    {RevisionCheckPolicy::Enforce, "enforce"},
    {RevisionCheckPolicy::RecordOnly, "record_only"},
}};

[[nodiscard]] std::string_view bool_token(bool value) noexcept {
    return value ? "true" : "false";
}

}  // namespace

std::string_view to_string(AssetIdMode value) noexcept {
    for (const auto& entry : kAssetIdModes) {
        if (entry.first == value) {
            return entry.second;
        }
    }
    return "caller_supplied";
}

std::optional<AssetIdMode> asset_id_mode_from_token(std::string_view token) noexcept {
    return find_token(kAssetIdModes.data(), kAssetIdModes.size(), token);
}

std::string_view to_string(SerialCollisionPolicy value) noexcept {
    for (const auto& entry : kSerialCollisionPolicies) {
        if (entry.first == value) {
            return entry.second;
        }
    }
    return "reject";
}

std::optional<SerialCollisionPolicy> serial_collision_policy_from_token(std::string_view token) noexcept {
    return find_token(kSerialCollisionPolicies.data(), kSerialCollisionPolicies.size(), token);
}

std::string_view to_string(IdentityReusePolicy value) noexcept {
    for (const auto& entry : kIdentityReusePolicies) {
        if (entry.first == value) {
            return entry.second;
        }
    }
    return "forbid";
}

std::optional<IdentityReusePolicy> identity_reuse_policy_from_token(std::string_view token) noexcept {
    return find_token(kIdentityReusePolicies.data(), kIdentityReusePolicies.size(), token);
}

std::string_view to_string(ModelConflictPolicy value) noexcept {
    for (const auto& entry : kModelConflictPolicies) {
        if (entry.first == value) {
            return entry.second;
        }
    }
    return "report";
}

std::optional<ModelConflictPolicy> model_conflict_policy_from_token(std::string_view token) noexcept {
    return find_token(kModelConflictPolicies.data(), kModelConflictPolicies.size(), token);
}

std::string_view to_string(RevisionCheckPolicy value) noexcept {
    for (const auto& entry : kRevisionCheckPolicies) {
        if (entry.first == value) {
            return entry.second;
        }
    }
    return "enforce";
}

std::optional<RevisionCheckPolicy> revision_check_policy_from_token(std::string_view token) noexcept {
    return find_token(kRevisionCheckPolicies.data(), kRevisionCheckPolicies.size(), token);
}

std::string RegistryPolicy::to_canonical_string() const {
    std::string result;
    result += "policy/1;serial_collision=";
    result += to_string(serial_collision);
    result += ";identity_reuse=";
    result += to_string(identity_reuse);
    result += ";model_conflict=";
    result += to_string(model_conflict);
    result += ";revision_check=";
    result += to_string(revision_check);
    result += ";require_installed_for_active=";
    result += bool_token(consistency.require_installed_for_active);
    result += ";require_installed_for_maintenance=";
    result += bool_token(consistency.require_installed_for_maintenance);
    result += ";require_staged_for_provisioned=";
    result += bool_token(consistency.require_staged_for_provisioned);
    result += ";require_terminal_for_superseded=";
    result += bool_token(consistency.require_terminal_for_superseded);
    result += ";require_terminal_before_supersede=";
    result += bool_token(require_terminal_before_supersede);
    return result;
}

const RegistryPolicy& default_policy() noexcept {
    static const RegistryPolicy kPolicy{};
    return kPolicy;
}

}  // namespace asset_registry
