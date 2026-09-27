// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Internal: the interface between AssetRegistry and Store.
//
// AssetRegistry owns the in-memory inventory; Store owns durability, the
// registry epoch, and writer authority. The seam is deliberately narrow: the
// registry hands the store a complete payload to publish and receives back the
// transaction sequence it was published at, or a structured failure. The store
// never inspects registry semantics beyond the codec it owns, and the registry
// never touches the store layout.

#ifndef ASSET_REGISTRY_INTERNAL_STORE_INTERNAL_HPP
#define ASSET_REGISTRY_INTERNAL_STORE_INTERNAL_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "asset_registry/error.hpp"
#include "asset_registry/persistence.hpp"
#include "asset_registry/strong_types.hpp"
#include "store_format.hpp"

namespace asset_registry {

/// Payload of one asset record as the registry supplies it.
using StoreRecord = std::shared_ptr<const AssetRecord>;

/// A decoded snapshot of the authoritative state, as read at open time.
struct StoreLoadedState {
    TransactionSequence sequence;
    RegistryEpoch epoch;
    std::string last_writer;
    std::vector<StoreRecord> records;
    std::vector<internal::StoredWriter> writers;
};

/// Implementation detail shared between Store and AssetRegistry. Declared here
/// rather than in the public header so consumers cannot depend on it.
class StoreAccess {
public:
    /// Publishes a complete payload. On success the store has atomically made
    /// the new generation authoritative and `published_sequence` reports the
    /// sequence it was published at. On failure the previous generation remains
    /// authoritative and nothing visible to readers has changed.
    [[nodiscard]] static Outcome<TransactionSequence> commit(Store& store, internal::StorePayload& payload,
                                                             const WriterId& writer);

    /// The state the store loaded at open time.
    [[nodiscard]] static const StoreLoadedState& loaded_state(const Store& store);

    /// Current published transaction sequence.
    [[nodiscard]] static TransactionSequence current_sequence(const Store& store);

    /// Current epoch.
    [[nodiscard]] static RegistryEpoch current_epoch(const Store& store);

    /// Serialises access to the grant table. Used by AssetRegistry so that token
    /// validation and commit are decided against one consistent view.
    [[nodiscard]] static bool token_is_live(const Store& store, const AuthorityToken& token);

    /// Bounds in force for this store.
    [[nodiscard]] static const RegistryLimits& stored_limits(const Store& store);
};

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_INTERNAL_STORE_INTERNAL_HPP
