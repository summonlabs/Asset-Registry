// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Immutable snapshot.
//
// A Snapshot is a complete, self-consistent, immutable read model of the
// inventory at one published transaction sequence. Consumers hold snapshots
// instead of reaching into the registry, which means:
//
//   * a consumer can iterate for as long as it likes without blocking writers
//     and without observing a partially applied mutation;
//   * every query answer, index, and aggregate within one snapshot agrees,
//     because they all come from the same published state;
//   * a snapshot remains valid after the registry is closed, so canonical
//     exports and long-running analyses do not need to hold the store open.
//
// Snapshots are values with a shared immutable body, so copying one is cheap and
// never duplicates the inventory.

#ifndef ASSET_REGISTRY_SNAPSHOT_HPP
#define ASSET_REGISTRY_SNAPSHOT_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <vector>

#include "asset_registry/asset.hpp"
#include "asset_registry/asset_class.hpp"
#include "asset_registry/limits.hpp"
#include "asset_registry/query.hpp"
#include "asset_registry/registry_policy.hpp"
#include "asset_registry/replacement.hpp"
#include "asset_registry/strong_types.hpp"

namespace asset_registry {

/// The immutable published body a snapshot refers to. Defined by the
/// implementation; consumers never name it and never construct one.
struct RegistryBody;

namespace detail {

/// Implements Snapshot::for_each over the published body. Declared here so the
/// template below can be defined in the header while the body stays opaque.
/// Returns false from `visitor` to stop the traversal.
ASSET_REGISTRY_API void visit_snapshot_body(const RegistryBody& body,
                                            const std::function<bool(const AssetRecord&)>& visitor);

}  // namespace detail

class ASSET_REGISTRY_API Snapshot {
public:
    /// An empty snapshot with the supplied policy and bounds. Used for
    /// validation and for building detached registries.
    [[nodiscard]] static Snapshot empty(const RegistryPolicy& policy, const RegistryLimits& bounds);

    Snapshot() = default;

    [[nodiscard]] bool valid() const noexcept { return body_ != nullptr; }
    [[nodiscard]] std::uint64_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// Registry epoch the snapshot was published under. Every snapshot reports
    /// the epoch that produced it, so a consumer can tell whether the evidence
    /// predates the current store incarnation.
    [[nodiscard]] RegistryEpoch epoch() const noexcept;
    [[nodiscard]] TransactionSequence published_sequence() const noexcept;
    [[nodiscard]] const RegistryPolicy& policy() const noexcept;
    [[nodiscard]] const RegistryLimits& bounds() const noexcept;

    /// Record lookup. Absent when the identity is not present in the snapshot.
    [[nodiscard]] std::optional<AssetView> find(const AssetId& id) const;
    [[nodiscard]] bool contains(const AssetId& id) const noexcept;

    /// The canonical identity of the asset that currently claims a serial
    /// identity, if any.
    [[nodiscard]] std::optional<AssetId> find_by_serial(const SerialIdentity& identity) const;

    /// All asset identities in canonical ascending order.
    [[nodiscard]] std::vector<AssetId> ids() const;

    /// All live records in canonical ascending order. Copied by value; use
    /// for_each to avoid materialising a large vector.
    [[nodiscard]] std::vector<AssetView> records() const;

    /// Visits every live record in canonical ascending order. `visitor` must be
    /// callable as bool(const AssetRecord&) or void(const AssetRecord&);
    /// returning false stops the traversal early. No registry lock is held and no
    /// per-record copy is made, so a visitor may call back into the registry
    /// without deadlocking and a full scan does not allocate.
    template <typename Visitor>
    void for_each(Visitor&& visitor) const {
        if (body_ == nullptr) {
            return;
        }
        detail::visit_snapshot_body(*body_, [&visitor](const AssetRecord& record) -> bool {
            if constexpr (std::is_void_v<std::invoke_result_t<Visitor&, const AssetRecord&>>) {
                visitor(record);
                return true;
            } else {
                return static_cast<bool>(visitor(record));
            }
        });
    }

    /// Bounded, deterministic page of matching identities.
    [[nodiscard]] Outcome<QueryPage> query(const QueryFilter& filter, std::uint64_t offset = 0,
                                           std::uint64_t limit = 256) const;

    /// Every identity matching the filter, in canonical ascending order.
    [[nodiscard]] std::vector<AssetId> matching(const QueryFilter& filter) const;

    /// Exact aggregate counts over the whole snapshot.
    [[nodiscard]] InventorySummary summary() const;

    /// Duplicate, conflict, and dangling-reference audit.
    [[nodiscard]] ConflictReport audit_conflicts() const;

    /// Resolves replacement lineage for one asset. The asset is reported as
    /// present through the returned outcome even when it has no lineage; a
    /// missing asset produces AssetNotFound.
    [[nodiscard]] Outcome<LineageChain> lineage(const AssetId& id,
                                                std::size_t max_depth = limits::kMaxLineageDepth) const;

    /// The nearest predecessor, absent when the asset has none.
    [[nodiscard]] std::optional<AssetId> predecessor_of(const AssetId& id) const;
    /// Direct successors in canonical ascending order.
    [[nodiscard]] std::vector<AssetId> successors_of(const AssetId& id) const;

    /// Provenance steps for one asset, oldest first, bounded to `limit` entries
    /// counted from the newest step backwards. Returns AssetNotFound when the
    /// asset is absent.
    [[nodiscard]] Outcome<std::vector<ProvenanceStep>> provenance(const AssetId& id, std::size_t limit = 64) const;

    /// Full retained provenance history for one asset, oldest first.
    [[nodiscard]] Outcome<std::vector<ProvenanceStep>> full_provenance(const AssetId& id) const;

    /// Estimated heap footprint of the snapshot body in bytes.
    [[nodiscard]] std::uint64_t estimated_bytes() const noexcept;

    /// Wraps an already built published body. Only the implementation produces
    /// bodies; the parameter type is opaque to consumers, so this cannot be used
    /// to fabricate a snapshot from arbitrary state.
    [[nodiscard]] static Snapshot from_body(std::shared_ptr<const RegistryBody> body);

private:
    friend class AssetRegistry;
    friend struct RegistryStateAccess;

    std::shared_ptr<const RegistryBody> body_;
};

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_SNAPSHOT_HPP
