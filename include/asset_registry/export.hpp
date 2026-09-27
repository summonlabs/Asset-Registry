// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Canonical export and import.
//
// The export document is the interchange contract for later DCCP consumers. It
// is deterministic: two exports of the same snapshot at the same schema version
// are byte-identical, records appear in canonical AssetId order, reference and
// label sequences are sorted, and every numeric field is rendered in its
// canonical decimal form. No map iteration order, locale setting, clock reading,
// or pointer value participates in the output.

#ifndef ASSET_REGISTRY_EXPORT_HPP
#define ASSET_REGISTRY_EXPORT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_registry/asset.hpp"
#include "asset_registry/error.hpp"
#include "asset_registry/snapshot.hpp"
#include "asset_registry/strong_types.hpp"

namespace asset_registry {

/// Export schema versions understood by this build.
inline constexpr std::uint32_t kExportSchemaVersion = 1;

enum class ExportFormat : std::uint8_t {
    /// Full canonical JSON: identity, metadata, references, state, lineage, and
    /// retained provenance. Intended for archival and for consumers that need
    /// provenance.
    CanonicalJson = 0,
    /// Compact JSON: identity, metadata, references, state, and lineage, with
    /// provenance omitted. Intended for consumers that only need current state.
    CompactJson = 1,
    /// Newline-delimited JSON: one canonical object per asset, one per line, in
    /// canonical AssetId order. Intended for streaming consumers.
    NewlineDelimitedJson = 2,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(ExportFormat format) noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::optional<ExportFormat> export_format_from_token(std::string_view token) noexcept;

struct ASSET_REGISTRY_API ExportOptions {
    ExportFormat format = ExportFormat::CanonicalJson;
    /// Schema version to emit. Only versions this build implements are accepted;
    /// requesting another version fails with ExportFormatUnsupported rather than
    /// emitting something the consumer cannot rely on.
    std::uint32_t schema_version = kExportSchemaVersion;
    /// Upper bound on records emitted. Exceeding it fails with CapacityExceeded
    /// rather than silently truncating an export.
    std::uint64_t max_records = 0;  // 0 means "use the snapshot's configured bound"
    /// Extra newline at the end of the document.
    bool trailing_newline = true;
    /// Deterministic indentation of the canonical JSON document. Zero emits a
    /// single-line document.
    std::uint32_t indent = 2;
};

/// Result of an export call.
struct ASSET_REGISTRY_API ExportReport {
    ExportFormat format = ExportFormat::CanonicalJson;
    std::uint32_t schema_version = kExportSchemaVersion;
    std::uint64_t records_exported = 0;
    std::uint64_t bytes_written = 0;
    /// Transaction sequence the exported snapshot was published at.
    TransactionSequence published_sequence;
    /// Registry epoch the exported snapshot belongs to.
    RegistryEpoch epoch;
};

/// Renders a snapshot to the canonical export document. Returns the document on
/// success and a machine-readable error otherwise; the caller decides where the
/// bytes go, so export never writes to a path implicitly.
[[nodiscard]] ASSET_REGISTRY_API Outcome<std::string> export_snapshot(const Snapshot& snapshot,
                                                                     const ExportOptions& options,
                                                                     ExportReport& report);

/// How an import batch treats a record that matches existing inventory.
enum class ImportConflictPolicy : std::uint8_t {
    /// A record whose identity already exists is rejected and reported.
    Reject = 0,
    /// A record whose identity already exists is skipped and reported as
    /// already present. Nothing about the existing record changes.
    SkipExisting = 1,
    /// A record whose identity already exists and whose canonical rendering
    /// differs from the stored record is applied as a metadata update through the
    /// ordinary revision-checked path. Divergence is reported per record.
    UpdateExisting = 2,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(ImportConflictPolicy policy) noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::optional<ImportConflictPolicy> import_conflict_policy_from_token(
    std::string_view token) noexcept;

/// What to do when an imported record violates a registry invariant.
enum class ImportErrorPolicy : std::uint8_t {
    /// Reject the offending record, keep the rest of the batch, report the
    /// rejection. The batch is still committed atomically.
    RejectRecord = 0,
    /// Reject the whole batch. Nothing is applied.
    RejectBatch = 1,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(ImportErrorPolicy policy) noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::optional<ImportErrorPolicy> import_error_policy_from_token(
    std::string_view token) noexcept;

struct ASSET_REGISTRY_API ImportOptions {
    ImportConflictPolicy conflict = ImportConflictPolicy::Reject;
    ImportErrorPolicy on_error = ImportErrorPolicy::RejectRecord;
    /// When false, a record carrying an unverified reference is imported with the
    /// reference retained at Unverified evidence. When true, unverified
    /// references are dropped from the record and the drop is reported.
    bool drop_unverified_references = false;
    /// Optional source attribution recorded in provenance for imported records.
    std::string source;
};

/// One rejected record from an import batch.
struct ASSET_REGISTRY_API ImportRejection {
    /// Zero-based position of the record inside the batch.
    std::uint64_t record_index = 0;
    /// Identity the record claimed, canonicalised when parseable and left empty
    /// when the record was too malformed to yield one.
    std::string claimed_id;
    ErrorCode code = ErrorCode::None;
    std::string message;
};

/// Outcome of an import batch.
struct ASSET_REGISTRY_API ImportReport {
    std::uint64_t records_seen = 0;
    std::uint64_t records_registered = 0;
    std::uint64_t records_updated = 0;
    std::uint64_t records_skipped = 0;
    std::uint64_t references_dropped = 0;
    std::vector<ImportRejection> rejections;
    /// True when the batch was committed. False when it was rejected wholesale
    /// or when every record failed under ImportErrorPolicy::RejectRecord and
    /// nothing changed.
    bool committed = false;
    TransactionSequence committed_sequence;

    [[nodiscard]] std::uint64_t records_rejected() const noexcept {
        return static_cast<std::uint64_t>(rejections.size());
    }
};

/// A parsed import record: a complete asset description with no revision
/// expectation, because the document describes desired state rather than a
/// mutation against known state.
struct ASSET_REGISTRY_API ImportedAsset {
    AssetId id;
    AssetClass asset_class = AssetClass::Unknown;
    SerialIdentity serial_identity;
    AssetMetadata metadata;
    std::vector<Reference> references;
    AssetState state;
    /// Optional predecessor identity carried by the document. When present the
    /// registry installs the same replacement link a live link_replacement call
    /// would, subject to the same acyclicity and terminal-state rules.
    std::optional<AssetId> supersedes;
    ReplacementCause supersession_cause = ReplacementCause::Unknown;
    /// Optional event time from the source document. Retained as provenance
    /// evidence and never used for ordering.
    std::optional<Timestamp> observed_at;
};

/// Parses a canonical export document into records, without touching a registry.
///
/// Every structural property of the document is validated: the schema version
/// must be one this build implements, object members must be unique and known,
/// required members must be present with the right type, enumerations must be
/// inside their domain, integers must be canonical non-negative decimals, and
/// strings must be structurally valid UTF-8. Nesting depth, total value count,
/// record count, and byte length are bounded before the corresponding allocation.
[[nodiscard]] ASSET_REGISTRY_API Outcome<std::vector<ImportedAsset>> parse_import_json(
    std::string_view document, const RegistryLimits& bounds);

/// Parses one asset object using the same rules the export document uses. The
/// object carries the members of one element of the document's `assets` array.
[[nodiscard]] ASSET_REGISTRY_API Outcome<ImportedAsset> parse_import_asset_json(std::string_view object,
                                                                               const RegistryLimits& bounds);

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_EXPORT_HPP
