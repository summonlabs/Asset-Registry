// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Deterministic query surface.
//
// Every public enumeration is ordered by canonical AssetId ascending, which is
// the total order over the raw 128-bit identity. Index-backed queries return
// results in the same order as a full scan, so no consumer can observe an
// ordering that depends on which index answered the query.

#ifndef ASSET_REGISTRY_QUERY_HPP
#define ASSET_REGISTRY_QUERY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "asset_registry/asset.hpp"
#include "asset_registry/asset_class.hpp"
#include "asset_registry/capability.hpp"
#include "asset_registry/lifecycle.hpp"
#include "asset_registry/reference.hpp"

namespace asset_registry {

/// Predicate applied to the asset set. Every field is an independent
/// conjunction; a default-constructed filter matches every asset.
struct ASSET_REGISTRY_API QueryFilter {
    std::optional<AssetClass> asset_class;
    std::optional<LifecycleState> lifecycle;
    std::optional<InstallationState> installation;
    std::optional<OwnerId> owner;
    std::optional<LocationId> site;
    std::optional<LocationId> location;
    std::optional<RackId> rack;
    std::optional<CapabilityReference> capability;

    /// True when the record is in a state that permits workload.
    bool workload_capable_only = false;

    /// True when the record is in a terminal lifecycle state.
    bool terminal_only = false;

    /// True when the record carries at least one reference whose evidence is
    /// Unverified.
    bool has_unverified_references_only = false;

    /// True when the record has no owner recorded.
    bool unowned_only = false;

    friend bool operator==(const QueryFilter& lhs, const QueryFilter& rhs) noexcept {
        return lhs.asset_class == rhs.asset_class && lhs.lifecycle == rhs.lifecycle &&
               lhs.installation == rhs.installation && lhs.owner == rhs.owner && lhs.site == rhs.site &&
               lhs.location == rhs.location && lhs.rack == rhs.rack && lhs.capability == rhs.capability &&
               lhs.workload_capable_only == rhs.workload_capable_only &&
               lhs.terminal_only == rhs.terminal_only &&
               lhs.has_unverified_references_only == rhs.has_unverified_references_only &&
               lhs.unowned_only == rhs.unowned_only;
    }
};

/// Bounded page of asset identities.
struct ASSET_REGISTRY_API QueryPage {
    std::vector<AssetId> ids;
    /// Total number of records matching the filter across the whole snapshot.
    std::uint64_t total_matches = 0;
    /// Offset this page started at.
    std::uint64_t offset = 0;
    /// True when further matches exist beyond this page.
    bool truncated = false;
};

/// Aggregate counts over a snapshot. Every field is exact for the snapshot it
/// was computed from; counts are never sampled or estimated.
struct ASSET_REGISTRY_API InventorySummary {
    std::uint64_t total_assets = 0;
    std::uint64_t terminal_assets = 0;
    std::uint64_t workload_capable_assets = 0;
    std::uint64_t unowned_assets = 0;
    std::uint64_t assets_with_unverified_references = 0;
    std::uint64_t assets_with_predecessor = 0;
    std::uint64_t assets_with_successor = 0;
    std::uint64_t total_references = 0;
    std::uint64_t total_provenance_steps = 0;
    std::uint64_t estimated_bytes = 0;

    /// Per-class counts in ascending AssetClass order, omitting classes with no
    /// members. Deterministic ordering.
    std::vector<std::pair<AssetClass, std::uint64_t>> by_class;
    /// Per-lifecycle counts in ascending LifecycleState order.
    std::vector<std::pair<LifecycleState, std::uint64_t>> by_lifecycle;
    /// Per-installation-state counts in ascending InstallationState order.
    std::vector<std::pair<InstallationState, std::uint64_t>> by_installation;
    /// Per-owner counts, ordered by owner identifier ascending. Records with no
    /// owner are counted in unowned_assets and omitted here.
    std::vector<std::pair<OwnerId, std::uint64_t>> by_owner;

    /// Highest revision observed across the snapshot.
    AssetRevision max_revision;
    /// Transaction sequence the snapshot was published at.
    TransactionSequence published_sequence;
    /// Registry epoch the snapshot was produced under.
    RegistryEpoch epoch;
};

/// Outcome of a duplicate/alias audit.
struct ASSET_REGISTRY_API ConflictReport {
    /// Canonical serial identity keys claimed by more than one live record.
    /// Every key here indicates a store that should not have been writable,
    /// so a non-empty result is a defect report, not a routine finding.
    std::vector<std::string> duplicate_serial_keys;
    /// Pairs of live records whose serial identities carry conflicting model
    /// strings. Reported because a disagreement about the model of one physical
    /// object is a data-quality defect.
    std::vector<std::pair<AssetId, AssetId>> model_conflicts;
    /// Assets whose replacement link names a predecessor that is not present in
    /// this registry. Legitimate when a predecessor was never imported; reported
    /// so operators can tell "not imported" from "lost".
    std::vector<AssetId> dangling_predecessors;
    /// Assets whose state pair violates the configured consistency rules.
    std::vector<AssetId> state_inconsistencies;
    /// Assets that are superseded in lineage but not in a terminal lifecycle
    /// state, or vice versa.
    std::vector<AssetId> supersession_state_mismatches;

    [[nodiscard]] bool clean() const noexcept {
        return duplicate_serial_keys.empty() && model_conflicts.empty() && dangling_predecessors.empty() &&
               state_inconsistencies.empty() && supersession_state_mismatches.empty();
    }
};

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_QUERY_HPP
