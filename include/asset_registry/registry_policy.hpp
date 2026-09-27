// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Registry mutation policy. These switches are stored with the durable state,
// because a store written under one policy must not be silently reinterpreted
// under another after a restart.

#ifndef ASSET_REGISTRY_REGISTRY_POLICY_HPP
#define ASSET_REGISTRY_REGISTRY_POLICY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "asset_registry/error.hpp"
#include "asset_registry/lifecycle.hpp"

namespace asset_registry {

/// How the canonical AssetId of a newly registered asset is chosen.
enum class AssetIdMode : std::uint8_t {
    /// The caller supplies the identity; the registry validates and uses it.
    CallerSupplied = 0,
    /// The registry derives a version-5 (name-based) identifier from the asset
    /// class and the canonical manufacturer/serial key. Re-importing the same
    /// physical object reproduces the same canonical identity, which makes
    /// idempotent re-import possible without a side table.
    DerivedFromSerialIdentity = 1,
    /// The registry generates a version-4 (random) identifier.
    GeneratedRandom = 2,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(AssetIdMode value) noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::optional<AssetIdMode> asset_id_mode_from_token(std::string_view token) noexcept;

/// What happens when a registration presents a serial identity that is already
/// claimed by a different canonical AssetId.
enum class SerialCollisionPolicy : std::uint8_t {
    /// Reject with DuplicateSerialIdentity. The default: two records claiming the
    /// same physical object is the situation this registry exists to prevent.
    Reject = 0,
    /// Reject unless the existing record is in a terminal lifecycle state and the
    /// new registration declares a replacement link naming it, in which case the
    /// registration is accepted as a replacement.
    AllowReplacementOfTerminal = 1,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(SerialCollisionPolicy value) noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::optional<SerialCollisionPolicy> serial_collision_policy_from_token(
    std::string_view token) noexcept;

/// Whether a canonical AssetId may be reused after its record is fully retired.
enum class IdentityReusePolicy : std::uint8_t {
    /// A canonical AssetId names one physical object forever. The default.
    Forbid = 0,
    /// After an asset reaches a terminal lifecycle state, the same AssetId may be
    /// reused for a new physical object. The previous incarnation is preserved in
    /// history, the generation increments, and the revision restarts at 1 only
    /// after the reuse is durably recorded as its own step.
    AllowAfterTerminal = 1,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(IdentityReusePolicy value) noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::optional<IdentityReusePolicy> identity_reuse_policy_from_token(
    std::string_view token) noexcept;

/// Whether conflicting model strings on an otherwise identical serial identity
/// are fatal.
enum class ModelConflictPolicy : std::uint8_t {
    /// Record the conflict as a data-quality observation on the collision report
    /// and continue to treat the two claims as the same physical object.
    Report = 0,
    /// Reject the registration outright.
    Reject = 1,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(ModelConflictPolicy value) noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::optional<ModelConflictPolicy> model_conflict_policy_from_token(
    std::string_view token) noexcept;

/// Behaviour when a mutation names a revision that is not the current one.
enum class RevisionCheckPolicy : std::uint8_t {
    /// Enforce the comparison. The default.
    Enforce = 0,
    /// Accept the mutation and record both the expected and observed revision in
    /// provenance. Intended for administrative repair paths; never silently
    /// widens authority because the mutation still requires a valid epoch.
    RecordOnly = 1,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(RevisionCheckPolicy value) noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::optional<RevisionCheckPolicy> revision_check_policy_from_token(
    std::string_view token) noexcept;

/// The mutable policy set of a store.
struct ASSET_REGISTRY_API RegistryPolicy {
    SerialCollisionPolicy serial_collision = SerialCollisionPolicy::Reject;
    IdentityReusePolicy identity_reuse = IdentityReusePolicy::Forbid;
    ModelConflictPolicy model_conflict = ModelConflictPolicy::Report;
    RevisionCheckPolicy revision_check = RevisionCheckPolicy::Enforce;

    /// Cross-dimension lifecycle/installation consistency rules.
    StateConsistencyRules consistency{};

    /// When true, an asset must be in a terminal lifecycle state before an
    /// installation transition to Superseded is accepted. Redundant with the
    /// consistency rule but kept explicit because it is a policy decision.
    bool require_terminal_before_supersede = true;

    friend bool operator==(const RegistryPolicy& lhs, const RegistryPolicy& rhs) noexcept {
        return lhs.serial_collision == rhs.serial_collision && lhs.identity_reuse == rhs.identity_reuse &&
               lhs.model_conflict == rhs.model_conflict && lhs.revision_check == rhs.revision_check &&
               lhs.consistency == rhs.consistency &&
               lhs.require_terminal_before_supersede == rhs.require_terminal_before_supersede;
    }
    friend bool operator!=(const RegistryPolicy& lhs, const RegistryPolicy& rhs) noexcept { return !(lhs == rhs); }

    /// Canonical, deterministic token rendering used by export and by the store
    /// metadata record. Field order is fixed.
    [[nodiscard]] std::string to_canonical_string() const;
};

[[nodiscard]] ASSET_REGISTRY_API const RegistryPolicy& default_policy() noexcept;

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_REGISTRY_POLICY_HPP
