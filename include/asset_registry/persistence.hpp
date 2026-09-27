// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Durable store.
//
// Layout inside a store directory (relative names are part of the format):
//
//   lock            exclusive writer lock, held for the lifetime of a writer
//   meta            small metadata record: format version, registry epoch,
//                   policy fingerprint, transaction sequence
//   CURRENT         pointer naming the authoritative generation file
//   generations/    immutable committed generation files
//   tmp/            staging area for generation files that are not yet published
//
// Commit protocol (plan -> validate -> reserve -> write -> verify -> publish):
//
//   1. The registry applies the mutation to a private working copy of the
//      published state and validates every invariant.
//   2. The next transaction sequence is reserved; it must be strictly greater
//      than the published one.
//   3. The complete new state is written to tmp/, flushed to the storage device,
//      and its integrity checksum verified by re-reading the staged bytes.
//   4. The staged file is renamed into generations/ (atomic within a directory
//      on every supported platform).
//   5. CURRENT is replaced atomically and flushed. Until this step completes, the
//      previous generation remains authoritative, so an interrupted commit
//      leaves a store that still reads as the last known-good state.
//
// A generation file that fails its header or payload integrity check, declares an
// unsupported format version, or decodes into an invalid record is never
// published. Recovery selects the highest-numbered fully valid generation and
// reports what it did; the store never presents damaged bytes as authoritative.

#ifndef ASSET_REGISTRY_PERSISTENCE_HPP
#define ASSET_REGISTRY_PERSISTENCE_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_registry/authority.hpp"
#include "asset_registry/error.hpp"
#include "asset_registry/limits.hpp"
#include "asset_registry/registry_policy.hpp"
#include "asset_registry/strong_types.hpp"

namespace asset_registry {

/// On-disk format version understood by this build.
inline constexpr std::uint32_t kStoreFormatVersion = 1;

/// Canonical payload schema version embedded in every generation file.
inline constexpr std::uint32_t kPayloadSchemaVersion = 1;

/// Instant of the registry clock, used for optional operator-visible metadata.
struct ASSET_REGISTRY_API StoreMetadata {
    StoreFormatVersion format_version;
    RegistryEpoch epoch;
    TransactionSequence sequence;
    /// Identity of the writer that most recently committed, empty after a fresh
    /// initialisation.
    std::string last_writer;
    /// Canonical policy fingerprint of the last commit. A mismatch between the
    /// stored fingerprint and the configured policy is reported by open().
    std::string policy_fingerprint;
    /// Number of committed generations retained at the time of the last commit.
    std::uint32_t retained_generations = 0;
};

/// How a store is opened.
enum class StoreOpenMode : std::uint8_t {
    /// Open an existing store for reading and writing. Fails when the directory
    /// holds no store.
    ReadWrite = 0,
    /// Open an existing store read-only: no lock is taken, no epoch is
    /// published, and mutation attempts are rejected. Several read-only openers
    /// may coexist with a writer, but they observe only committed generations.
    ReadOnly = 1,
    /// Create the store when absent, then open for reading and writing.
    CreateIfMissing = 2,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(StoreOpenMode mode) noexcept;

/// What recovery decided during open().
enum class RecoveryAction : std::uint8_t {
    /// CURRENT named a generation that passed every check and matched the stored
    /// metadata. Nothing was repaired.
    None = 0,
    /// CURRENT was missing, unreadable, or named a damaged generation. The store
    /// was republished at the highest-numbered fully valid generation.
    RolledBackToLastValid = 1,
    /// A damaged generation file was quarantined and left in place so the fault
    /// can be investigated; the store switched to a valid generation.
    DamagedGenerationQuarantined = 2,
    /// Staging files from an interrupted commit were removed.
    RemovedOrphanTemporaries = 3,
    /// The store directory did not exist or held no store, and one was created.
    Initialised = 4,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(RecoveryAction action) noexcept;

/// Structured description of what open() observed and did.
struct ASSET_REGISTRY_API RecoveryReport {
    RecoveryAction action = RecoveryAction::None;
    /// True when open() changed anything on disk.
    bool store_modified = false;
    /// True when the published state differs from what CURRENT named before the
    /// open, or from the state a previous incarnation left behind.
    bool content_rolled_back = false;
    /// Generation that was authoritative before the open; zero when there was
    /// none.
    TransactionSequence previous_sequence;
    /// Generation now authoritative; zero when the store is empty.
    TransactionSequence current_sequence;
    /// Epoch published by this open. Every successful open publishes a strictly
    /// greater epoch than any epoch previously stored.
    RegistryEpoch epoch;
    /// Names (relative to the store root) of generation files quarantined or
    /// removed. Sorted ascending for determinism.
    std::vector<std::string> affected_files;
    /// Operator-readable account of the decision. Never used for control flow.
    std::string detail;
};

/// Open-time options. Defaults are the conservative ones: verify integrity,
/// recover automatically, and require a format version this build understands.
struct ASSET_REGISTRY_API StoreOpenOptions {
    /// Verify the payload integrity of the authoritative generation even when
    /// CURRENT and meta agree. Off by default: the header checksum and payload
    /// checksum are always verified for the authoritative generation, and this
    /// switch additionally forces a full re-decode of every retained generation
    /// to detect bit rot in files that are not currently authoritative.
    bool deep_verify_all_generations = false;

    /// Repair instead of failing when the authoritative generation is damaged.
    /// When false, a damaged authoritative generation produces a
    /// StoreIntegrityFailed error and the store is left untouched. This is the
    /// only switch that decides whether an open may rewrite durable state.
    bool allow_recovery = true;

    /// Walk the recorded ancestry of the authoritative generation and verify that
    /// every generation it descends from is present and matches the checksum its
    /// successor recorded. Detects a hand-edited CURRENT pointer, which no check
    /// inside a single generation file can detect. Off by default because it
    /// re-reads the retained history; the pointer, header, declared length, and
    /// payload checksums are always verified regardless of this switch.
    bool deep_verify_chain = false;

    /// Remove staging files left by interrupted commits.
    bool clean_orphan_temporaries = true;

    /// Policy to enforce when the store records none yet (fresh store) or when
    /// the stored fingerprint differs from the supplied policy.
    RegistryPolicy policy = default_policy();

    /// When true, a stored policy fingerprint that disagrees with the supplied
    /// policy makes open() fail with InvalidConfiguration instead of accepting
    /// the stored policy.
    bool require_policy_match = false;

    /// Bounds applied to decoded state. Strictly enforced: a stored payload that
    /// exceeds a configured bound fails to open rather than opening partially.
    RegistryLimits limits = default_limits();

    /// Optional operator-visible writer identity recorded for a freshly
    /// initialised store.
    std::string initial_writer;
};

/// Handle on a store directory. A Store owns the exclusive lock, the current
/// epoch, and the mapped metadata; it does not own the registry state.
class ASSET_REGISTRY_API Store {
public:
    /// Opens a store directory, performing recovery and publishing a new epoch.
    ///
    /// On success the returned Store holds the exclusive lock (unless read-only)
    /// and `report` describes exactly what recovery did and what the store looked
    /// like before and after.
    [[nodiscard]] static Outcome<std::shared_ptr<Store>> open(const std::filesystem::path& directory,
                                                             StoreOpenMode mode,
                                                             const StoreOpenOptions& options,
                                                             RecoveryReport& report);

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    /// Releases the exclusive lock and clears grant state. Closing twice is
    /// harmless. Grants outstanding at close are revoked, so a token held across
    /// a close can never publish. Destruction performs the same release, so a
    /// process that exits normally cannot leave a store locked.
    ~Store();

    /// Releases the exclusive lock and clears grant state. Closing twice is
    /// harmless. Grants outstanding at close are revoked, so a token held across
    /// a close can never publish. Returns StoreIoError when the final metadata
    /// publication fails; the lock is still released in that case.
    Outcome<void> close();

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool is_read_only() const noexcept;
    [[nodiscard]] const std::filesystem::path& directory() const noexcept;
    [[nodiscard]] StoreMetadata metadata() const;
    [[nodiscard]] const RecoveryReport& recovery() const noexcept;
    [[nodiscard]] const RegistryPolicy& policy() const noexcept;
    [[nodiscard]] const RegistryLimits& limits() const noexcept;
    [[nodiscard]] RegistryEpoch epoch() const noexcept;

    /// Number of committed generation files currently retained.
    [[nodiscard]] std::uint32_t retained_generation_count() const;

    /// Lists retained generation files, oldest first. Read-only; used by
    /// diagnostics and by tests that assert compaction behaviour.
    [[nodiscard]] std::vector<std::string> retained_generations() const;

    /// Mints mutation authority for `writer`. The grant generation advances
    /// monotonically within an epoch, and the returned token carries the current
    /// epoch. Returns StaleAuthorityEpoch when the store has since been reopened
    /// by another incarnation, and RegistryClosed when the store is closed.
    [[nodiscard]] Outcome<AuthorityToken> grant(const WriterId& writer, const AuthorityOptions& options = {});

    /// Revokes a grant. Revocation is immediate for every copy of the token and
    /// is idempotent.
    Outcome<void> revoke(const AuthorityToken& token);

    /// True when the token is live: minted by this store, in the current epoch,
    /// not revoked, and not expired. This is the exact predicate every mutation
    /// enforces.
    [[nodiscard]] bool is_token_live(const AuthorityToken& token) const;

    /// Highest mutation sequence this store has recorded for a writer in the
    /// current epoch, absent when the writer has no recorded history.
    [[nodiscard]] std::optional<MutationSequence> writer_high_water(const WriterId& writer) const;

private:
    friend class AssetRegistry;
    friend class StoreAccess;
    friend struct RegistryStateAccess;

    Store();
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_PERSISTENCE_HPP
