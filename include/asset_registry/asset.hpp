// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The asset record: immutable physical identity plus mutable metadata.
//
// The split is enforced by the type system and by the mutation API:
//
//   Physical identity (fixed for the lifetime of one identity generation)
//     canonical AssetId, AssetClass, AssetGeneration.
//
//   Mutable metadata (changed only through revision-checked mutations that
//   append provenance)
//     display name, owner, labels, serial identity spelling, capability/
//     location/rack references, lifecycle and installation state.
//
// Changing the class is a reclassification with its own provenance action, not
// a metadata update, because downstream consumers key physical behaviour off it.

#ifndef ASSET_REGISTRY_ASSET_HPP
#define ASSET_REGISTRY_ASSET_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_registry/asset_class.hpp"
#include "asset_registry/capability.hpp"
#include "asset_registry/error.hpp"
#include "asset_registry/lifecycle.hpp"
#include "asset_registry/limits.hpp"
#include "asset_registry/provenance.hpp"
#include "asset_registry/reference.hpp"
#include "asset_registry/replacement.hpp"
#include "asset_registry/serial_identity.hpp"
#include "asset_registry/strong_types.hpp"

namespace asset_registry {

namespace limits {
inline constexpr std::size_t kMaxDisplayNameCodePoints = 64;
inline constexpr std::size_t kMaxNotesBytes = 1024;
}  // namespace limits

/// Mutable descriptive metadata. Absent members are genuinely unknown: an
/// absent owner means "ownership not established", and such a record is never
/// treated as owned by anyone.
struct ASSET_REGISTRY_API AssetMetadata {
    /// Human-readable asset name. Required at registration.
    std::string display_name;

    /// Owning organisation reference. Absent when unknown.
    std::optional<OwnerId> owner;

    /// Site the asset belongs to, as an opaque reference into the facility
    /// topology repository. Absent when unknown.
    std::optional<LocationId> site;

    /// Free-text operator notes, bounded and UTF-8 validated.
    std::string notes;

    /// Sorted (key-ascending) label set. Ordering is part of the canonical
    /// serialisation contract.
    std::vector<std::pair<std::string, std::string>> labels;

    friend bool operator==(const AssetMetadata& lhs, const AssetMetadata& rhs) noexcept {
        return lhs.display_name == rhs.display_name && lhs.owner == rhs.owner && lhs.site == rhs.site &&
               lhs.notes == rhs.notes && lhs.labels == rhs.labels;
    }
    friend bool operator!=(const AssetMetadata& lhs, const AssetMetadata& rhs) noexcept { return !(lhs == rhs); }

    /// Looks up a label value; absent when the key is not present.
    [[nodiscard]] const std::string* find_label(std::string_view key) const noexcept;
};

/// A superseded incarnation of one canonical AssetId, retained when the
/// identity-reuse policy permitted reuse. The incarnation keeps the identity
/// evidence of the object that previously held the identity so that no audit
/// trail is lost.
struct ASSET_REGISTRY_API AssetIncarnation {
    AssetGeneration generation;
    AssetClass asset_class = AssetClass::Unknown;
    SerialIdentity serial_identity;
    AssetMetadata metadata;
    std::vector<Reference> references;
    AssetState state;
    /// Revision the incarnation reached before it was superseded.
    AssetRevision final_revision;
    /// Transaction sequence of the step that closed the incarnation.
    TransactionSequence closed_at;
    std::optional<Timestamp> closed_time;
};

/// The complete authoritative record for one live canonical AssetId.
struct ASSET_REGISTRY_API AssetRecord {
    // --- Physical identity -------------------------------------------------
    AssetId id;
    AssetClass asset_class = AssetClass::Unknown;
    AssetGeneration generation;

    // --- Versioning --------------------------------------------------------
    /// Monotonic revision. Advances by exactly one per published mutation of
    /// this record, never resets except when the identity itself is reused.
    AssetRevision revision;

    /// Transaction sequence of the commit that created the current revision.
    TransactionSequence last_sequence;

    // --- Evidence ----------------------------------------------------------
    SerialIdentity serial_identity;
    AssetMetadata metadata;
    std::vector<Reference> references;
    AssetState state;

    // --- Lineage -----------------------------------------------------------
    std::optional<ReplacementLink> supersedes;

    // --- Provenance --------------------------------------------------------
    /// Append-only history for the current revision series, oldest first.
    std::vector<ProvenanceStep> provenance;
    /// Superseded incarnations, oldest first, bounded by
    /// RegistryLimits::max_generations_per_asset.
    std::vector<AssetIncarnation> history;

    [[nodiscard]] std::optional<Reference> find_reference(const Reference& like) const noexcept;
    [[nodiscard]] bool has_reference(const Reference& reference) const noexcept;

    /// Estimated heap footprint in bytes. Deterministic and reproducible; used
    /// for the memory bound and reported by the CLI. It is an estimate of
    /// allocated storage, never a claim about allocator behaviour.
    [[nodiscard]] std::uint64_t estimated_bytes() const noexcept;
};

/// Immutable read model handed to consumers. Views are self-contained values, so
/// a consumer can hold one across registry mutations without observing torn
/// state; the snapshot it came from stays alive through a shared owner.
struct ASSET_REGISTRY_API AssetView {
    AssetId id;
    AssetClass asset_class = AssetClass::Unknown;
    AssetGeneration generation;
    AssetRevision revision;
    TransactionSequence last_sequence;
    SerialIdentity serial_identity;
    AssetMetadata metadata;
    std::vector<Reference> references;
    AssetState state;
    std::optional<ReplacementLink> supersedes;
    /// Provenance steps retained in memory for this record, oldest first.
    const std::vector<ProvenanceStep>* provenance = nullptr;
    /// Superseded incarnations, oldest first.
    const std::vector<AssetIncarnation>* history = nullptr;

    /// True when the record is in a lifecycle state that permits workload.
    [[nodiscard]] bool permits_workload() const noexcept { return asset_registry::permits_workload(state.lifecycle); }

    /// True when the record is in a terminal lifecycle state.
    [[nodiscard]] bool is_terminal() const noexcept { return is_terminal_lifecycle(state.lifecycle); }

    /// Capability references attached to this asset, in canonical order.
    [[nodiscard]] std::vector<CapabilityReference> capability_references() const;

    /// The location reference, absent when none is attached.
    [[nodiscard]] std::optional<LocationId> location() const noexcept;
    /// The rack reference, absent when none is attached.
    [[nodiscard]] std::optional<RackId> rack() const noexcept;
    /// Evidence level of the location reference, absent when none is attached.
    [[nodiscard]] std::optional<ReferenceEvidence> location_evidence() const noexcept;
    /// Evidence level of the rack reference, absent when none is attached.
    [[nodiscard]] std::optional<ReferenceEvidence> rack_evidence() const noexcept;
};

/// Checks that a record satisfies every invariant the registry asserts about a
/// live record. Returns an empty optional when the record is sound, otherwise a
/// machine-readable description of the first violation found.
///
/// This is the same predicate the registry applies to freshly built records and
/// to records decoded from durable state, which is why a corrupt or hand-edited
/// store cannot introduce a record the API could not have produced.
///
/// The cross-dimension state rules are a matter of policy, so this form applies the
/// documented defaults. A caller that holds its own rules — a registry validating a
/// mutation, or a decoder validating a payload that records the rules it was written
/// under — uses the form below. That is what makes "a registry configured with a
/// relaxed rule can hold a record a stricter reader would refuse" true rather than a
/// claim the validator quietly contradicts.
[[nodiscard]] ASSET_REGISTRY_API std::optional<std::string> validate_asset_record(const AssetRecord& record,
                                                                                 const RegistryLimits& bounds);

/// Checks a record against the given cross-dimension consistency rules.
[[nodiscard]] ASSET_REGISTRY_API std::optional<std::string> validate_asset_record(
    const AssetRecord& record, const RegistryLimits& bounds, const StateConsistencyRules& consistency_rules);

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_ASSET_HPP
