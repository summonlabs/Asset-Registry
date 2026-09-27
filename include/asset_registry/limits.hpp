// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Bounds. Every externally influenced size, count, length, and depth in this
// library is bounded by a value in RegistryLimits. Bounds are checked before
// allocation, and a rejected bound produces a machine-readable error rather
// than a truncated or partially applied change.

#ifndef ASSET_REGISTRY_LIMITS_HPP
#define ASSET_REGISTRY_LIMITS_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "asset_registry/error.hpp"

namespace asset_registry {

struct RegistryLimits {
    /// Maximum number of live asset records. Registrations beyond this are
    /// rejected with CapacityExceeded; existing records remain fully readable.
    std::uint64_t max_assets = 1'000'000;

    /// Maximum number of references (capability/location/rack/external) on one
    /// asset record.
    std::uint32_t max_references_per_asset = 256;

    /// Maximum number of label entries on one asset record.
    std::uint32_t max_labels_per_asset = 64;

    /// Maximum byte length of one label value.
    std::size_t max_label_value_bytes = 256;

    /// Maximum number of provenance steps retained in memory per asset. The
    /// durable store retains the same number; exceeding it is rejected with
    /// ProvenanceLimitExceeded rather than silently discarding authority
    /// evidence.
    std::uint32_t max_provenance_per_asset = 4096;

    /// Maximum number of identity generations retained in history for one
    /// AssetId.
    std::uint32_t max_generations_per_asset = 16;

    /// Maximum byte length of an asset display name.
    std::size_t max_display_name_bytes = 192;

    /// Maximum bytes accepted for one imported document or persisted payload.
    /// Checked against the declared length before any allocation.
    std::uint64_t max_document_bytes = 512ULL * 1024ULL * 1024ULL;

    /// Maximum number of records accepted in one import batch.
    std::uint32_t max_import_records = 1'000'000;

    /// Maximum number of records serialised by one export.
    std::uint32_t max_export_records = 5'000'000;

    /// Soft ceiling on estimated in-memory registry footprint in bytes. A
    /// mutation that would exceed it is rejected with CapacityExceeded before
    /// the commit is attempted.
    std::uint64_t max_registry_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;

    /// Maximum number of idempotency records retained per writer.
    std::uint32_t max_idempotency_records_per_writer = 4096;

    /// Maximum number of distinct writers with retained idempotency records.
    std::uint32_t max_tracked_writers = 4096;

    /// Maximum number of temporary generation files left behind by interrupted
    /// commits before a new commit is refused. Recovery removes them, so a
    /// persistent backlog indicates a real fault rather than normal operation.
    std::uint32_t max_orphan_temporaries = 64;

    /// Maximum number of committed generation files retained before compaction
    /// is required.
    std::uint32_t max_retained_generations = 64;

    /// Maximum depth of a stack-allocated bounded traversal (hierarchy walks).
    std::uint32_t max_traversal_depth = 64;
};

/// Validates a limit set. Rejects internally inconsistent bounds, such as a zero
/// asset bound, a zero provenance bound, or an export bound below the asset bound
/// (which would make a full export impossible). Returns an empty optional when the
/// set is usable.
[[nodiscard]] ASSET_REGISTRY_API std::optional<std::string> validate_limits(const RegistryLimits& limits);

/// Documented defaults.
[[nodiscard]] ASSET_REGISTRY_API const RegistryLimits& default_limits() noexcept;

/// Absolute hard ceiling on max_document_bytes, independent of configuration.
/// A caller cannot raise the import/restore bound past this value.
inline constexpr std::uint64_t kAbsoluteMaxDocumentBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_LIMITS_HPP
