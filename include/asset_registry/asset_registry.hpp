// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// AssetRegistry: the mutation surface.
//
// Shape of the API:
//
//   * Mutation commands are free functions on the registry taking a
//     WriterSession and a MutationEnvelope. Authority is checked before any
//     state is touched, the mutation is applied to a private working copy,
//     invariants are validated, and only then is the working copy committed and
//     published. A rejected mutation leaves the registry byte-identical to
//     before the call.
//   * Reads go through Snapshot. A snapshot is immutable, self-consistent, and
//     stays valid after the registry closes.
//   * No consumer touches persistence structures. The store layout, generation
//     files, and metadata records are private to the implementation.
//
// Threading model: all mutation commands are serialised through one registry
// mutex; reads of a snapshot are lock-free and may run concurrently with
// mutations. No callback is ever invoked while an internal lock is held. A
// WriterSession is not thread safe; one session belongs to one logical writer.

#ifndef ASSET_REGISTRY_ASSET_REGISTRY_HPP
#define ASSET_REGISTRY_ASSET_REGISTRY_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_registry/asset.hpp"
#include "asset_registry/asset_class.hpp"
#include "asset_registry/authority.hpp"
#include "asset_registry/error.hpp"
#include "asset_registry/export.hpp"
#include "asset_registry/limits.hpp"
#include "asset_registry/persistence.hpp"
#include "asset_registry/provenance.hpp"
#include "asset_registry/query.hpp"
#include "asset_registry/registry_policy.hpp"
#include "asset_registry/replacement.hpp"
#include "asset_registry/serial_identity.hpp"
#include "asset_registry/snapshot.hpp"

namespace asset_registry {

namespace internal {
struct RegistryImpl;
}  // namespace internal

/// Expected revision supplied with a mutation. An absent expectation means
/// "apply unconditionally"; when supplied, the mutation is rejected with
/// StaleRevision unless the record is exactly at that revision.
using RevisionExpectation = std::optional<AssetRevision>;

/// Expected generation supplied with a mutation. An absent expectation means
/// "any generation"; when supplied, the mutation is rejected with
/// StaleGeneration unless the record is at that generation.
using GenerationExpectation = std::optional<AssetGeneration>;

/// Result of a successful mutation.
struct ASSET_REGISTRY_API MutationResult {
    /// True when the outcome was replayed from an idempotency record and no
    /// change was applied by this call.
    bool idempotent_replay = false;
    /// Revision the record holds after the call. For a replay this is the
    /// revision the original call produced.
    AssetRevision revision;
    /// Canonical identity the mutation targeted, empty for mutations that
    /// operate on the store rather than one record.
    AssetId subject;
    /// Transaction sequence the change was published at. Absent for a replay,
    /// because a replay publishes nothing.
    std::optional<TransactionSequence> published_sequence;
};

/// Description of an asset being registered.
struct ASSET_REGISTRY_API RegisterAssetRequest {
    // --- Canonical identity -------------------------------------------------
    /// How the canonical AssetId is chosen. CallerSupplied uses `id`; the other
    /// modes derive or generate one and reject a request that also supplies an
    /// id, so there is never ambiguity about which identity was used.
    AssetIdMode id_mode = AssetIdMode::CallerSupplied;
    /// Required when id_mode is CallerSupplied; the nil identifier is rejected.
    AssetId id;

    // --- Physical identity --------------------------------------------------
    AssetClass asset_class = AssetClass::Unknown;
    SerialIdentity serial_identity;

    // --- Metadata -----------------------------------------------------------
    AssetMetadata metadata;

    // --- Initial state ------------------------------------------------------
    /// Initial lifecycle state. Defaults to Planned: a newly registered asset is
    /// not automatically considered deployable.
    LifecycleState lifecycle = LifecycleState::Planned;
    /// Initial installation state. Defaults to Unknown, because a registration
    /// is not evidence that anyone installed anything.
    InstallationState installation = InstallationState::Unknown;

    // --- References ---------------------------------------------------------
    std::vector<Reference> references;

    // --- Lineage ------------------------------------------------------------
    /// Optional predecessor. When present, the same checks as
    /// link_replacement apply: the predecessor must exist unless the request
    /// explicitly allows a dangling link, the link must not create a cycle, and
    /// the serial-collision policy decides whether a predecessor is required.
    std::optional<AssetId> supersedes;
    ReplacementCause supersession_cause = ReplacementCause::Unknown;
    std::string supersession_note;

    /// When true, an absent predecessor is accepted and recorded as a dangling
    /// link. Default false: a replacement that names an asset this registry has
    /// never seen is a data-quality problem worth rejecting during normal
    /// registration.
    bool allow_dangling_predecessor = false;
};

/// Result of a successful registration.
struct ASSET_REGISTRY_API RegisterResult {
    AssetId id;
    AssetRevision revision;
    /// True when the registry recognised that the same physical object was
    /// already present and returned the existing record instead of creating a
    /// second one. This happens only under the derived-identity mode, where the
    /// canonical identity is a pure function of physical identity.
    bool already_present = false;
    /// True when the canonical identity was derived rather than taken from the
    /// request. Reported so operators can see which identity mode was applied.
    bool derived_identity = false;
    bool idempotent_replay = false;
    std::optional<TransactionSequence> published_sequence;
};

/// Metadata patch. Absent members are left unchanged; a member that is present
/// replaces the current value, and empty_option members clear an optional field.
struct ASSET_REGISTRY_API MetadataPatch {
    std::optional<std::string> display_name;
    std::optional<OwnerId> owner;
    /// When true, the owner field is cleared. Distinct from an absent owner,
    /// which means "leave unchanged".
    bool clear_owner = false;
    std::optional<LocationId> site;
    bool clear_site = false;
    std::optional<std::string> notes;
    bool clear_notes = false;
    /// Labels to set. An empty string value removes the label.
    std::vector<std::pair<std::string, std::string>> set_labels;
    /// Label keys to remove.
    std::vector<std::string> remove_labels;
};

/// Placement change. Rack and location are mutually exclusive per rack: an asset
/// occupies at most one rack, and a rack reference without a location reference
/// is legitimate (the rack identifier may be globally unique).
struct ASSET_REGISTRY_API PlacementChange {
    std::optional<LocationId> location;
    bool clear_location = false;
    std::optional<RackId> rack;
    bool clear_rack = false;
    std::optional<UnitSpan> units;
    bool clear_units = false;
    /// Evidence level recorded for the placement references.
    ReferenceEvidence evidence = ReferenceEvidence::Unverified;
};

class ASSET_REGISTRY_API AssetRegistry {
public:
    /// Creates a detached, in-memory registry with no durable store. Mutations
    /// require no token because there is no durable authority to fence; the
    /// registry reports AuthorityMode::LocalAuthority. Intended for building an
    /// inventory programmatically and for tests.
    [[nodiscard]] static Outcome<std::shared_ptr<AssetRegistry>> create_detached(
        const RegistryPolicy& policy = default_policy(), const RegistryLimits& bounds = default_limits());

    /// Opens or creates a store and returns a registry over it. The registry
    /// shares ownership of the store; the store stays open until every registry
    /// and snapshot derived from it is gone, or until close() is called.
    [[nodiscard]] static Outcome<std::shared_ptr<AssetRegistry>> open(const std::filesystem::path& directory,
                                                                     StoreOpenMode mode,
                                                                     const StoreOpenOptions& options,
                                                                     RecoveryReport& report);

    AssetRegistry(const AssetRegistry&) = delete;
    ~AssetRegistry();

    // --- Identity and status ------------------------------------------------

    [[nodiscard]] AuthorityMode authority_mode() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] RegistryEpoch epoch() const noexcept;
    [[nodiscard]] TransactionSequence published_sequence() const noexcept;
    [[nodiscard]] const RegistryPolicy& policy() const noexcept;
    [[nodiscard]] const RegistryLimits& limits() const noexcept;

    /// The store backing this registry. Null for a detached registry.
    [[nodiscard]] std::shared_ptr<Store> store() const noexcept;

    /// The current immutable read model. Cheap: copies a shared pointer.
    [[nodiscard]] Snapshot snapshot() const;

    /// Commits any pending durable publication. Every successful mutation is
    /// already committed before it returns, so this exists for callers that want
    /// to assert durability at a specific point (for example before reporting
    /// readiness). Idempotent and safe to call repeatedly.
    Outcome<void> flush();

    /// Stops accepting new mutations, revokes every outstanding grant, and
    /// releases the store lock. Mutations attempted afterwards fail with
    /// RegistryClosed. Reads of existing snapshots keep working. Idempotent.
    Outcome<void> close();

    // --- Sessions -----------------------------------------------------------

    /// Mints authority for a writer against this registry's store. Fails with
    /// InvalidConfiguration on a detached registry, where no token is needed.
    [[nodiscard]] Outcome<WriterSession> open_writer(const WriterId& writer,
                                                     const AuthorityOptions& options = {});

    /// Revokes a session's authority. A revoked session can never publish.
    Outcome<void> close_writer(WriterSession& session);

    // --- Mutation commands --------------------------------------------------

    [[nodiscard]] Outcome<RegisterResult> register_asset(WriterSession& session, const MutationEnvelope& envelope,
                                                         const RegisterAssetRequest& request);

    [[nodiscard]] Outcome<MutationResult> update_metadata(WriterSession& session, const MutationEnvelope& envelope,
                                                          const AssetId& id, RevisionExpectation expected,
                                                          const MetadataPatch& patch);

    [[nodiscard]] Outcome<MutationResult> reclassify_asset(WriterSession& session, const MutationEnvelope& envelope,
                                                           const AssetId& id, RevisionExpectation expected,
                                                           AssetClass new_class);

    [[nodiscard]] Outcome<MutationResult> update_serial_identity(WriterSession& session,
                                                                 const MutationEnvelope& envelope, const AssetId& id,
                                                                 RevisionExpectation expected,
                                                                 const SerialIdentity& identity);

    [[nodiscard]] Outcome<MutationResult> set_placement(WriterSession& session, const MutationEnvelope& envelope,
                                                        const AssetId& id, RevisionExpectation expected,
                                                        const PlacementChange& change);

    [[nodiscard]] Outcome<MutationResult> transition_lifecycle(WriterSession& session,
                                                               const MutationEnvelope& envelope, const AssetId& id,
                                                               RevisionExpectation expected,
                                                               LifecycleState target);

    [[nodiscard]] Outcome<MutationResult> transition_installation(WriterSession& session,
                                                                  const MutationEnvelope& envelope, const AssetId& id,
                                                                  RevisionExpectation expected,
                                                                  InstallationState target);

    [[nodiscard]] Outcome<MutationResult> attach_reference(WriterSession& session, const MutationEnvelope& envelope,
                                                           const AssetId& id, RevisionExpectation expected,
                                                           const Reference& reference);

    [[nodiscard]] Outcome<MutationResult> detach_reference(WriterSession& session, const MutationEnvelope& envelope,
                                                           const AssetId& id, RevisionExpectation expected,
                                                           const Reference& reference);

    /// Changes the evidence level of an already attached reference. Re-verifying
    /// an unverified reference requires no new information; marking a verified
    /// reference stale does not require the owning repository to answer.
    [[nodiscard]] Outcome<MutationResult> set_reference_evidence(WriterSession& session,
                                                                 const MutationEnvelope& envelope, const AssetId& id,
                                                                 RevisionExpectation expected,
                                                                 const Reference& reference,
                                                                 ReferenceEvidence evidence);

    /// Installs a replacement link on `successor` naming `predecessor`. Requires
    /// the predecessor to be in a terminal lifecycle state, requires it not to
    /// have a successor already when the policy forbids it, and rejects links
    /// that would create a cycle or a self-reference.
    [[nodiscard]] Outcome<MutationResult> link_replacement(WriterSession& session, const MutationEnvelope& envelope,
                                                           const AssetId& successor, RevisionExpectation expected,
                                                           const AssetId& predecessor, ReplacementCause cause,
                                                           std::string_view note = {});

    /// Reuses a canonical AssetId for a new physical object. Legal only when the
    /// identity-reuse policy permits it and the current record is in a terminal
    /// lifecycle state. The prior incarnation is preserved in history, the
    /// generation advances, and provenance records the decision.
    [[nodiscard]] Outcome<MutationResult> reuse_identity(WriterSession& session, const MutationEnvelope& envelope,
                                                         const AssetId& id, RevisionExpectation expected,
                                                         const SerialIdentity& new_identity,
                                                         AssetClass new_class);

    // --- Import -------------------------------------------------------------

    /// Parses a canonical export document and returns the records it describes.
    /// Parsing never mutates the registry and never trusts the document: schema
    /// version, lengths, counts, enum domains, identity syntax, and integrity of
    /// every field are validated before anything is returned.
    [[nodiscard]] Outcome<std::vector<ImportedAsset>> parse_import_document(std::string_view document,
                                                                           const ImportOptions& options);

    /// Applies parsed records to the registry through the ordinary
    /// revision-checked mutation path, as one atomic transaction. Either every
    /// accepted record is published together or nothing is published.
    [[nodiscard]] Outcome<ImportReport> import_assets(WriterSession& session, const MutationEnvelope& envelope,
                                                      const std::vector<ImportedAsset>& records,
                                                      const ImportOptions& options);

    // --- Export -------------------------------------------------------------

    [[nodiscard]] Outcome<std::string> export_document(const ExportOptions& options, ExportReport& report) const;


private:
    friend class Snapshot;
    friend struct RegistryStateAccess;
    friend struct internal::RegistryImpl;

    /// Constructs an empty registry. Use create_detached or open instead.
    AssetRegistry();

    /// Per-instance implementation state. Defined by the implementation and not
    /// part of the API surface.
    std::unique_ptr<internal::RegistryImpl> impl_;
};

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_ASSET_REGISTRY_HPP
