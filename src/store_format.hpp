// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Internal: durable payload schema.
//
// A generation file is:
//
//   [ fixed 48-byte header ]
//   [ payload bytes        ]
//   [ CRC-32 of payload    ]   (4 bytes, big-endian)
//
// The header carries a magic number, the format version, the schema version, the
// generation sequence, the registry epoch, a CRC-32 over the first 40 header
// bytes, and the declared payload length. Nothing is trusted before it is
// checked: the header CRC is verified first, then the declared payload length is
// compared against the actual file size, and only then is the payload parsed.
//
// A payload records the complete authoritative state: policy, bounds, registry
// epoch, every live asset record (including its retained provenance and
// incarnation history), and the per-writer mutation and idempotency history that
// makes retries safe across a restart.

#ifndef ASSET_REGISTRY_INTERNAL_STORE_FORMAT_HPP
#define ASSET_REGISTRY_INTERNAL_STORE_FORMAT_HPP

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "asset_registry/asset.hpp"
#include "asset_registry/error.hpp"
#include "asset_registry/limits.hpp"
#include "asset_registry/persistence.hpp"
#include "asset_registry/registry_policy.hpp"
#include "asset_registry/strong_types.hpp"

namespace asset_registry::internal {

/// Fixed header size in bytes.
inline constexpr std::size_t kHeaderBytes = 48;
/// Trailing integrity field size in bytes.
inline constexpr std::size_t kTrailerBytes = 4;
/// Smallest possible valid generation file.
inline constexpr std::size_t kMinimumGenerationBytes = kHeaderBytes + kTrailerBytes;

struct GenerationHeader {
    std::uint32_t format_version = kStoreFormatVersion;
    std::uint32_t schema_version = kPayloadSchemaVersion;
    std::uint64_t sequence = 0;
    std::uint64_t epoch = 0;
    std::uint64_t payload_bytes = 0;
};

/// Encodes a header into exactly kHeaderBytes bytes.
[[nodiscard]] std::string encode_generation_header(const GenerationHeader& header);

/// Decodes and verifies a header from a complete generation file image.
[[nodiscard]] Outcome<GenerationHeader> decode_generation_header(std::string_view file_bytes);

/// Discriminator for a retained idempotency outcome.
enum class StoredOutcomeKind : std::uint8_t {
    Revision = 0,
    RegisteredAsset = 1,
    Error = 2,
};

/// One retained idempotency outcome.
struct StoredIdempotency {
    MutationSequence sequence;
    std::array<std::uint8_t, 16> fingerprint{};
    std::string key;
    StoredOutcomeKind outcome_kind = StoredOutcomeKind::Revision;
    /// Revision produced by the original mutation, for the Revision and
    /// RegisteredAsset outcomes.
    std::uint64_t revision = 0;
    /// Identity the original mutation produced, for the RegisteredAsset outcome.
    AssetId asset;
    /// Rejection the original mutation produced, for the Error outcome. Retained
    /// verbatim so a replayed rejection explains itself exactly as the original
    /// did.
    ErrorCode error_code = ErrorCode::None;
    std::string error_message;
};

struct StoredWriter {
    WriterId writer;
    MutationSequence high_water;
    /// Lowest sequence still retained for this writer. A repeated sequence below
    /// low_water was applied but its outcome has been pruned, so the registry
    /// rejects the retry instead of guessing what it returned.
    MutationSequence low_water;
    std::vector<StoredIdempotency> records;
};

/// The complete decoded payload.
struct StorePayload {
    TransactionSequence sequence;
    RegistryEpoch epoch;
    RegistryPolicy policy;
    RegistryLimits limits;
    std::string last_writer;
    std::vector<std::shared_ptr<const AssetRecord>> records;
    std::vector<StoredWriter> writers;
    /// Chain continuity: the sequence and payload CRC of the generation this one
    /// superseded. Zero for the first generation of a store.
    TransactionSequence predecessor_sequence;
    std::uint32_t predecessor_crc = 0;
};

/// Encodes a payload. Deterministic for a given payload value.
[[nodiscard]] Outcome<std::string> encode_store_payload(const StorePayload& payload, std::uint64_t max_bytes);

struct DecodeLimits {
    RegistryLimits limits;
    std::uint64_t max_bytes = 0;
    std::uint32_t max_depth = 64;
};

/// Decodes and fully validates a payload, including every asset record and the
/// every-record-agrees invariants that make the payload a coherent inventory.
[[nodiscard]] Outcome<StorePayload> decode_store_payload(std::string_view payload, const DecodeLimits& decode_limits);

/// Tag numbers. Part of the durable format; never reused or renumbered.
namespace tag {

inline constexpr std::uint64_t kPayload = 0x0101;
inline constexpr std::uint64_t kSequence = 0x0102;
inline constexpr std::uint64_t kEpoch = 0x0103;
inline constexpr std::uint64_t kLastWriter = 0x0104;
inline constexpr std::uint64_t kPredecessorSequence = 0x0105;
inline constexpr std::uint64_t kPredecessorCrc = 0x0106;

inline constexpr std::uint64_t kPolicy = 0x0110;
inline constexpr std::uint64_t kPolicySerialCollision = 0x0111;
inline constexpr std::uint64_t kPolicyIdentityReuse = 0x0112;
inline constexpr std::uint64_t kPolicyModelConflict = 0x0113;
inline constexpr std::uint64_t kPolicyRevisionCheck = 0x0114;
inline constexpr std::uint64_t kPolicyRequireTerminalSupersede = 0x0115;
inline constexpr std::uint64_t kPolicyConsistency = 0x0116;
inline constexpr std::uint64_t kConsistencyInstalledForActive = 0x0117;
inline constexpr std::uint64_t kConsistencyInstalledForMaintenance = 0x0118;
inline constexpr std::uint64_t kConsistencyStagedForProvisioned = 0x0119;
inline constexpr std::uint64_t kConsistencyTerminalForSuperseded = 0x011A;

inline constexpr std::uint64_t kLimits = 0x0120;
inline constexpr std::uint64_t kLimitsMaxAssets = 0x0121;
inline constexpr std::uint64_t kLimitsMaxReferences = 0x0122;
inline constexpr std::uint64_t kLimitsMaxLabels = 0x0123;
inline constexpr std::uint64_t kLimitsMaxLabelBytes = 0x0124;
inline constexpr std::uint64_t kLimitsMaxProvenance = 0x0125;
inline constexpr std::uint64_t kLimitsMaxGenerations = 0x0126;
inline constexpr std::uint64_t kLimitsMaxDisplayName = 0x0127;
inline constexpr std::uint64_t kLimitsMaxDocumentBytes = 0x0128;
inline constexpr std::uint64_t kLimitsMaxImportRecords = 0x0129;
inline constexpr std::uint64_t kLimitsMaxExportRecords = 0x012A;
inline constexpr std::uint64_t kLimitsMaxRegistryBytes = 0x012B;
inline constexpr std::uint64_t kLimitsMaxIdempotency = 0x012C;
inline constexpr std::uint64_t kLimitsMaxWriters = 0x012D;
inline constexpr std::uint64_t kLimitsMaxOrphanTemporaries = 0x012E;
inline constexpr std::uint64_t kLimitsMaxRetainedGenerations = 0x012F;
inline constexpr std::uint64_t kLimitsMaxTraversalDepth = 0x0130;

inline constexpr std::uint64_t kRecords = 0x0200;
inline constexpr std::uint64_t kRecord = 0x0201;
inline constexpr std::uint64_t kRecordId = 0x0202;
inline constexpr std::uint64_t kRecordClass = 0x0203;
inline constexpr std::uint64_t kRecordGeneration = 0x0204;
inline constexpr std::uint64_t kRecordRevision = 0x0205;
inline constexpr std::uint64_t kRecordLastSequence = 0x0206;
inline constexpr std::uint64_t kRecordManufacturer = 0x0207;
inline constexpr std::uint64_t kRecordSerial = 0x0208;
inline constexpr std::uint64_t kRecordModelPresent = 0x0209;
inline constexpr std::uint64_t kRecordModel = 0x020A;
inline constexpr std::uint64_t kRecordDisplayName = 0x020B;
inline constexpr std::uint64_t kRecordOwnerPresent = 0x020C;
inline constexpr std::uint64_t kRecordOwner = 0x020D;
inline constexpr std::uint64_t kRecordSitePresent = 0x020E;
inline constexpr std::uint64_t kRecordSite = 0x020F;
inline constexpr std::uint64_t kRecordNotes = 0x0210;
inline constexpr std::uint64_t kRecordLabels = 0x0211;
inline constexpr std::uint64_t kLabel = 0x0212;
inline constexpr std::uint64_t kLabelKey = 0x0213;
inline constexpr std::uint64_t kLabelValue = 0x0214;
inline constexpr std::uint64_t kRecordReferences = 0x0215;
inline constexpr std::uint64_t kReference = 0x0216;
inline constexpr std::uint64_t kReferenceKind = 0x0217;
inline constexpr std::uint64_t kReferenceEvidence = 0x0218;
inline constexpr std::uint64_t kReferenceTarget = 0x0219;
inline constexpr std::uint64_t kReferenceNamespace = 0x021A;
inline constexpr std::uint64_t kReferenceExternalKind = 0x021B;
inline constexpr std::uint64_t kRecordLifecycle = 0x021C;
inline constexpr std::uint64_t kRecordInstallation = 0x021D;
inline constexpr std::uint64_t kRecordSupersedesPresent = 0x021E;
inline constexpr std::uint64_t kSupersedes = 0x021F;
inline constexpr std::uint64_t kSupersedePredecessor = 0x0220;
inline constexpr std::uint64_t kSupersedePredecessorGeneration = 0x0221;
inline constexpr std::uint64_t kSupersedePredecessorRevision = 0x0222;
inline constexpr std::uint64_t kSupersedeCause = 0x0223;
inline constexpr std::uint64_t kSupersedeLinkedAt = 0x0224;
inline constexpr std::uint64_t kSupersedeNote = 0x0225;
inline constexpr std::uint64_t kRecordProvenance = 0x0226;
inline constexpr std::uint64_t kProvenanceStep = 0x0227;
inline constexpr std::uint64_t kProvenanceSequence = 0x0228;
inline constexpr std::uint64_t kProvenanceEpoch = 0x0229;
inline constexpr std::uint64_t kProvenanceRevision = 0x022A;
inline constexpr std::uint64_t kProvenanceAction = 0x022B;
inline constexpr std::uint64_t kProvenanceOrigin = 0x022C;
inline constexpr std::uint64_t kProvenanceActor = 0x022D;
inline constexpr std::uint64_t kProvenanceTimestampPresent = 0x022E;
inline constexpr std::uint64_t kProvenanceTimestamp = 0x022F;
inline constexpr std::uint64_t kProvenanceReason = 0x0230;
inline constexpr std::uint64_t kProvenanceChange = 0x0231;
inline constexpr std::uint64_t kRecordHistory = 0x0232;
inline constexpr std::uint64_t kIncarnation = 0x0233;
inline constexpr std::uint64_t kIncarnationGeneration = 0x0234;
inline constexpr std::uint64_t kIncarnationClass = 0x0235;
inline constexpr std::uint64_t kIncarnationManufacturer = 0x0236;
inline constexpr std::uint64_t kIncarnationSerial = 0x0237;
inline constexpr std::uint64_t kIncarnationModelPresent = 0x0238;
inline constexpr std::uint64_t kIncarnationModel = 0x0239;
inline constexpr std::uint64_t kIncarnationDisplayName = 0x023A;
inline constexpr std::uint64_t kIncarnationOwnerPresent = 0x023B;
inline constexpr std::uint64_t kIncarnationOwner = 0x023C;
inline constexpr std::uint64_t kIncarnationSitePresent = 0x023D;
inline constexpr std::uint64_t kIncarnationSite = 0x023E;
inline constexpr std::uint64_t kIncarnationNotes = 0x023F;
inline constexpr std::uint64_t kIncarnationLabels = 0x0240;
inline constexpr std::uint64_t kIncarnationReferences = 0x0241;
inline constexpr std::uint64_t kIncarnationLifecycle = 0x0242;
inline constexpr std::uint64_t kIncarnationInstallation = 0x0243;
inline constexpr std::uint64_t kIncarnationRevision = 0x0244;
inline constexpr std::uint64_t kIncarnationClosedAt = 0x0245;
inline constexpr std::uint64_t kIncarnationClosedTimePresent = 0x0246;
inline constexpr std::uint64_t kIncarnationClosedTime = 0x0247;

inline constexpr std::uint64_t kWriters = 0x0300;
inline constexpr std::uint64_t kWriter = 0x0301;
inline constexpr std::uint64_t kWriterId = 0x0302;
inline constexpr std::uint64_t kWriterHighWater = 0x0303;
inline constexpr std::uint64_t kWriterLowWater = 0x0304;
inline constexpr std::uint64_t kWriterRecords = 0x0305;
inline constexpr std::uint64_t kIdempotency = 0x0306;
inline constexpr std::uint64_t kIdempotencySequence = 0x0307;
inline constexpr std::uint64_t kIdempotencyFingerprint = 0x0308;
inline constexpr std::uint64_t kIdempotencyKey = 0x0309;
inline constexpr std::uint64_t kIdempotencyOutcomeKind = 0x030A;
inline constexpr std::uint64_t kIdempotencyRevision = 0x030B;
inline constexpr std::uint64_t kIdempotencyAsset = 0x030C;
inline constexpr std::uint64_t kIdempotencyErrorName = 0x030D;
inline constexpr std::uint64_t kIdempotencyErrorMessage = 0x030E;

}  // namespace tag

}  // namespace asset_registry::internal

#endif  // ASSET_REGISTRY_INTERNAL_STORE_FORMAT_HPP
