// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Replacement lineage.
//
// A physical replacement produces a new asset with a new canonical AssetId. The
// new record carries a ReplacementLink naming the asset it superseded. The
// predecessor is never deleted and its identity is never reused to represent the
// replacement, so historical references to the old AssetId keep resolving to the
// object they originally named.
//
// Every asset has at most one direct predecessor. An asset may have several
// direct successors (a failed unit replaced twice, or a unit split into parts).
// This makes the lineage graph a forest of in-trees and guarantees acyclicity by
// construction; the registry still verifies the property on insertion and on
// store load, because persisted state is untrusted.

#ifndef ASSET_REGISTRY_REPLACEMENT_HPP
#define ASSET_REGISTRY_REPLACEMENT_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_registry/error.hpp"
#include "asset_registry/provenance.hpp"
#include "asset_registry/strong_types.hpp"

namespace asset_registry {

namespace limits {
inline constexpr std::size_t kMaxReplacementReasonBytes = 256;
/// Maximum number of hops any lineage traversal will follow. Lineage chains in
/// a real facility are a handful of hops; the bound exists so that crafted
/// persisted state cannot drive unbounded traversal.
inline constexpr std::size_t kMaxLineageDepth = 64;
/// Maximum number of successors materialised by one lineage query.
inline constexpr std::size_t kMaxLineageFanout = 1024;
}  // namespace limits

/// Recorded reason for a replacement, from a closed set. Free-form text is
/// carried separately in the provenance step's reason field.
enum class ReplacementCause : std::uint8_t {
    Unknown = 0,
    Failure = 1,
    Upgrade = 2,
    Refresh = 3,
    Relocation = 4,
    Reconfiguration = 5,
    EndOfLife = 6,
    WarrantyReturn = 7,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(ReplacementCause value) noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::optional<ReplacementCause> replacement_cause_from_token(
    std::string_view token) noexcept;

/// The link installed on a successor record.
struct ASSET_REGISTRY_API ReplacementLink {
    /// Canonical identity of the asset this record superseded.
    AssetId predecessor;

    AssetGeneration predecessor_generation;

    /// Revision the predecessor had reached when it was superseded. Recorded so
    /// that auditors can tell which predecessor state the replacement decision
    /// was made against.
    AssetRevision predecessor_final_revision;

    ReplacementCause cause = ReplacementCause::Unknown;

    /// Transaction sequence of the step that created the link.
    TransactionSequence linked_at;

    /// Optional operator note; validated and bounded.
    std::string note;
};

/// Outcome of a lineage walk, reported so callers can distinguish "no
/// predecessor" from "traversal stopped at the safety bound".
enum class LineageTermination : std::uint8_t {
    /// The walk completed because it reached the start of the chain.
    ReachedOrigin = 0,
    /// The walk stopped because it hit the configured depth bound. The returned
    /// chain is a prefix, not the whole history.
    DepthLimitReached = 1,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(LineageTermination value) noexcept;

/// Result of resolving a replacement chain.
struct ASSET_REGISTRY_API LineageChain {
    /// Ancestors from the queried asset outward, nearest predecessor first. The
    /// queried asset itself is not included.
    std::vector<AssetId> predecessors;

    /// Direct successors of the queried asset, in ascending AssetId order,
    /// truncated to limits::kMaxLineageFanout.
    std::vector<AssetId> successors;

    /// True when successors() was truncated by the fan-out bound.
    bool successors_truncated = false;

    LineageTermination termination = LineageTermination::ReachedOrigin;

    /// The oldest ancestor reached, absent when the queried asset has no
    /// predecessor.
    [[nodiscard]] std::optional<AssetId> origin() const noexcept;
};

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_REPLACEMENT_HPP
