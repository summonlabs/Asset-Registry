// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Internal: construction of published snapshots.
//
// AssetRegistry and the Snapshot implementation are split across translation
// units, so the small bridge between them lives here rather than in the public
// header: consumers never name RegistryBody or build one.

#ifndef ASSET_REGISTRY_INTERNAL_SNAPSHOT_INTERNAL_HPP
#define ASSET_REGISTRY_INTERNAL_SNAPSHOT_INTERNAL_HPP

#include <memory>

#include "asset_registry/limits.hpp"
#include "asset_registry/registry_policy.hpp"
#include "asset_registry/snapshot.hpp"
#include "asset_registry/strong_types.hpp"
#include "registry_state.hpp"

namespace asset_registry::detail {

/// Builds a published body from a completed state, deriving the serial-identity
/// index and successor counts in the same pass.
[[nodiscard]] std::shared_ptr<const RegistryBody> make_body(const RegistryPolicy& policy, const RegistryLimits& bounds,
                                                            RegistryEpoch epoch, TransactionSequence sequence,
                                                            IndexedState state);

/// Wraps a body in a Snapshot value.
[[nodiscard]] Snapshot snapshot_from_body(std::shared_ptr<const RegistryBody> body);

}  // namespace asset_registry::detail

#endif  // ASSET_REGISTRY_INTERNAL_SNAPSHOT_INTERNAL_HPP
