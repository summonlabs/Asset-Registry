// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Internal: the published read model and the secondary index cache.
//
// Publication model
// -----------------
// Every mutation builds a new immutable body from the current one, so a snapshot
// handed to a consumer is never modified afterwards. Records are shared between
// bodies through shared pointers and are themselves immutable, which makes
// "copy the body" a pointer copy rather than a deep copy of the inventory; only
// the records a mutation actually changes are duplicated.
//
// The slot array is the single public ordering: strictly ascending canonical
// AssetId. Lookups binary-search it, so lookup cost is logarithmic and every
// enumeration is already in canonical order without a sort.
//
// Secondary indices (per class, per state, per reference target) are derived data
// and are built lazily on first use, cached inside the body that produced them.
// Building them never mutates the body, so a partially built index cannot be
// observed by another reader.

#ifndef ASSET_REGISTRY_INTERNAL_REGISTRY_STATE_HPP
#define ASSET_REGISTRY_INTERNAL_REGISTRY_STATE_HPP

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "asset_registry/asset.hpp"
#include "asset_registry/limits.hpp"
#include "asset_registry/query.hpp"
#include "asset_registry/registry_policy.hpp"
#include "asset_registry/strong_types.hpp"
#include "store_format.hpp"

namespace asset_registry {

/// Records are immutable once published, so bodies share them.
using RecordPtr = std::shared_ptr<const AssetRecord>;

/// Where one record sits inside the published arrays.
struct RecordPlacement {
    std::size_t index = 0;
    bool found = false;
};

/// Immutable published inventory, in canonical AssetId order.
struct IndexedState {
    std::vector<RecordPtr> records;
    /// Parallel identifier array: ids[index] identifies records[index].
    std::vector<AssetId> ids;
    /// Per-writer mutation and idempotency history, ordered by writer identity.
    std::vector<internal::StoredWriter> writers;

    [[nodiscard]] std::size_t size() const noexcept { return records.size(); }
    [[nodiscard]] bool empty() const noexcept { return records.empty(); }

    [[nodiscard]] RecordPlacement find(const AssetId& id) const noexcept {
        const auto iterator = std::lower_bound(ids.begin(), ids.end(), id);
        if (iterator == ids.end() || !(*iterator == id)) {
            return RecordPlacement{0, false};
        }
        return RecordPlacement{static_cast<std::size_t>(iterator - ids.begin()), true};
    }

    /// Inserts a record in canonical order and returns the slot it occupies. A
    /// record whose identity is already present must be replaced through
    /// replace_at, because duplicate canonical identities are rejected rather than
    /// tolerated.
    [[nodiscard]] std::size_t insert(RecordPtr record);
    void replace_at(std::size_t index, RecordPtr record);
    void erase_at(std::size_t index);

    [[nodiscard]] const internal::StoredWriter* find_writer(const WriterId& writer) const noexcept;
    [[nodiscard]] internal::StoredWriter* find_writer(const WriterId& writer) noexcept;
    /// Slot at which a writer's history belongs, keeping the array ordered by
    /// writer identity.
    [[nodiscard]] std::size_t writer_slot_for_insert(const WriterId& writer) const noexcept;
};

/// Derived index set over an IndexedState.
struct SecondaryIndices {
    TransactionSequence sequence;
    std::map<AssetClass, std::vector<AssetId>> by_class;
    std::map<LifecycleState, std::vector<AssetId>> by_lifecycle;
    std::map<InstallationState, std::vector<AssetId>> by_installation;
    std::map<OwnerId, std::vector<AssetId>> by_owner;
    std::vector<AssetId> unowned;
    std::vector<AssetId> workload_capable;
    std::vector<AssetId> terminal;
    std::vector<AssetId> with_unverified_references;
    std::vector<AssetId> with_predecessor;
    std::map<std::string, std::vector<AssetId>> by_capability;
    std::map<std::string, std::vector<AssetId>> by_location;
    std::map<std::string, std::vector<AssetId>> by_rack;
};

/// Builds the derived index set for a state. Deterministic: every list is in
/// ascending canonical AssetId order because the source is iterated in that order.
[[nodiscard]] std::shared_ptr<const SecondaryIndices> build_secondary_indices(const IndexedState& state,
                                                                             TransactionSequence sequence);

/// Cache holding one derived index set for the state it was built from.
class IndexCache {
public:
    [[nodiscard]] const SecondaryIndices& get(const IndexedState& state, TransactionSequence sequence) const {
        {
            const std::lock_guard<std::mutex> guard(mutex_);
            if (cache_ != nullptr && cache_->sequence == sequence) {
                return *cache_;
            }
        }
        auto built = build_secondary_indices(state, sequence);
        const std::lock_guard<std::mutex> guard(mutex_);
        if (cache_ == nullptr || cache_->sequence != sequence) {
            cache_ = std::move(built);
        }
        return *cache_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::shared_ptr<const SecondaryIndices> cache_;
};

/// The full published body a Snapshot refers to.
struct RegistryBody {
    RegistryPolicy policy = default_policy();
    RegistryLimits bounds = default_limits();
    RegistryEpoch epoch;
    TransactionSequence sequence;
    IndexedState state;
    /// Serial identity key -> record slot. Maintained alongside the slot array so
    /// collision detection is a hash lookup rather than a scan.
    std::unordered_map<std::string, std::size_t> serial_index;
    /// Successor counts keyed by compact predecessor identity.
    std::unordered_map<std::string, std::uint32_t> successor_count;
    /// Derived index set, built on first use.
    IndexCache secondary;

    [[nodiscard]] const SecondaryIndices& indices() const { return secondary.get(state, sequence); }
};

/// True when the record satisfies the filter. Shared by the indexed and scanned
/// paths so the two cannot disagree about what a filter means.
[[nodiscard]] bool record_matches(const AssetRecord& record, const QueryFilter& filter);

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_INTERNAL_REGISTRY_STATE_HPP
