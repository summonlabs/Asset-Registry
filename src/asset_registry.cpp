// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// AssetRegistry: implementation.
//
// Structure of one mutation:
//
//   1. Take the registry commit lock. All mutations are serialised through it, so
//      there is never a second writer inside the critical section and the base
//      revision a caller compares against cannot change under it.
//   2. Decide authority: a live grant held by this registry, a writer sequence
//      that advances, and an idempotency decision for retries.
//   3. Copy the published state. Records are shared and immutable, so the copy is
//      a pointer copy; only the records this mutation changes are duplicated.
//   4. Apply the change to the copy and validate every invariant the registry
//      asserts, using the same predicate the durable store applies on load.
//   5. Publish. For a durable registry this reserves a transaction identity,
//      writes a new generation, verifies the staged bytes by re-reading them,
//      atomically replaces the pointer, and only then swaps the in-memory
//      published body. A failure at any point leaves the published body exactly
//      as it was.
//
// No callback runs while the commit lock is held, and no lock is held while a
// snapshot is read, so a reader never blocks a writer and a writer never
// deadlocks against a reader.

#include "asset_registry/asset_registry.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "asset_registry/text.hpp"
#include "fingerprint.hpp"
#include "registry_state.hpp"
#include "snapshot_internal.hpp"
#include "store_internal.hpp"
#include "uuid.hpp"
namespace asset_registry {

namespace internal {

/// All mutable state of one registry instance. The public header names this type
/// in a friend declaration, so the implementation can reach it without exposing
/// it to consumers.
struct RegistryImpl {
    std::shared_ptr<Store> store;
    RegistryPolicy policy = default_policy();
    RegistryLimits bounds = default_limits();
    AuthorityMode authority_mode = AuthorityMode::RegistryAuthority;

    /// Serialises mutations from this process. Nothing else is held while it is
    /// held, and no callback runs while it is held.
    mutable std::mutex commit_mutex;

    /// Guards the published body pointer. Held only to read or swap the pointer,
    /// never across a mutation or a durable write.
    mutable std::mutex publish_mutex;
    std::shared_ptr<const RegistryBody> body;

    /// Local authority grants, used by a detached registry.
    struct Grant {
        RegistryEpoch epoch;
        bool revoked = false;
    };
    std::map<std::string, Grant> grants;
    AuthorityGeneration next_grant{1};
    RegistryEpoch epoch{1};
    std::atomic_bool closed{false};

    [[nodiscard]] bool detached() const noexcept { return store == nullptr; }
};

}  // namespace internal

using RegistryImpl = internal::RegistryImpl;

namespace {

[[nodiscard]] Error reject(ErrorCode code, std::string message) { return Error(code, std::move(message)); }

[[nodiscard]] std::shared_ptr<const RegistryBody> read_published(const RegistryImpl& impl) {
    const std::lock_guard<std::mutex> guard(impl.publish_mutex);
    return impl.body;
}

void install_published(RegistryImpl& impl, std::shared_ptr<const RegistryBody> body) {
    const std::lock_guard<std::mutex> guard(impl.publish_mutex);
    impl.body = std::move(body);
}

// ---------------------------------------------------------------------------
// Request fingerprint material
//
// A fingerprint answers one question: "is this the request that produced the
// recorded outcome?". Fields are length-prefixed so concatenation is
// unambiguous. The material is never parsed back and never persisted as text.
// ---------------------------------------------------------------------------

void append_field(std::string& target, std::string_view value) {
    target += std::to_string(value.size());
    target += ':';
    target.append(value.data(), value.size());
    target += ';';
}

[[nodiscard]] std::string join_fields(std::initializer_list<std::string_view> parts) {
    std::string material;
    for (const std::string_view part : parts) {
        append_field(material, part);
    }
    return material;
}

[[nodiscard]] std::string material_metadata(const AssetMetadata& metadata) {
    std::string material;
    append_field(material, metadata.display_name);
    append_field(material, metadata.owner.has_value() ? metadata.owner->text() : std::string());
    append_field(material, metadata.site.has_value() ? metadata.site->text() : std::string());
    append_field(material, metadata.notes);
    for (const auto& label : metadata.labels) {
        append_field(material, label.first);
        append_field(material, label.second);
    }
    return material;
}

[[nodiscard]] std::string material_references(const std::vector<Reference>& references) {
    std::string material;
    for (const Reference& reference : references) {
        append_field(material, reference.canonical());
        append_field(material, to_string(reference.evidence()));
    }
    return material;
}

[[nodiscard]] std::string material_serial(const SerialIdentity& identity) {
    std::string material;
    append_field(material, identity.manufacturer().name());
    append_field(material, identity.serial().text());
    append_field(material, identity.model().has_value() ? identity.model()->text() : std::string());
    return material;
}

[[nodiscard]] std::string material_patch(const MetadataPatch& patch) {
    std::string material;
    append_field(material, patch.display_name.has_value() ? *patch.display_name : std::string());
    append_field(material, patch.display_name.has_value() ? "set" : "unset");
    append_field(material, patch.owner.has_value() ? patch.owner->text() : std::string());
    append_field(material, patch.clear_owner ? "clear" : "keep");
    append_field(material, patch.site.has_value() ? patch.site->text() : std::string());
    append_field(material, patch.clear_site ? "clear" : "keep");
    append_field(material, patch.notes.has_value() ? *patch.notes : std::string());
    append_field(material, patch.clear_notes ? "clear" : "keep");
    for (const auto& label : patch.set_labels) {
        append_field(material, label.first);
        append_field(material, label.second);
    }
    for (const std::string& key : patch.remove_labels) {
        append_field(material, key);
    }
    return material;
}

[[nodiscard]] std::string material_placement(const PlacementChange& change) {
    std::string material;
    append_field(material, change.location.has_value() ? change.location->text() : std::string());
    append_field(material, change.clear_location ? "clear" : "keep");
    append_field(material, change.rack.has_value() ? change.rack->text() : std::string());
    append_field(material, change.clear_rack ? "clear" : "keep");
    append_field(material, change.units.has_value() ? change.units->to_string() : std::string());
    append_field(material, change.clear_units ? "clear" : "keep");
    append_field(material, to_string(change.evidence));
    return material;
}

[[nodiscard]] std::string material_register(const RegisterAssetRequest& request) {
    std::string material;
    append_field(material, request.id.to_string());
    append_field(material, to_string(request.id_mode));
    append_field(material, to_string(request.asset_class));
    append_field(material, material_serial(request.serial_identity));
    append_field(material, material_metadata(request.metadata));
    append_field(material, material_references(request.references));
    append_field(material, to_string(request.lifecycle));
    append_field(material, to_string(request.installation));
    append_field(material, request.supersedes.has_value() ? request.supersedes->to_string() : std::string());
    append_field(material, to_string(request.supersession_cause));
    append_field(material, request.supersession_note);
    append_field(material, request.allow_dangling_predecessor ? "dangling_allowed" : "dangling_forbidden");
    return material;
}

[[nodiscard]] std::string describe_transition(LifecycleState from, LifecycleState to) {
    std::string change = "lifecycle=";
    change += to_string(from);
    change += "->";
    change += to_string(to);
    return change;
}

[[nodiscard]] std::string describe_transition(InstallationState from, InstallationState to) {
    std::string change = "installation=";
    change += to_string(from);
    change += "->";
    change += to_string(to);
    return change;
}
}  // namespace

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------

[[nodiscard]] std::string grant_key(const WriterId& writer, AuthorityGeneration generation) {
    return writer.text() + "#" + generation.to_string();
}

[[nodiscard]] bool token_is_live(const RegistryImpl& impl, const AuthorityToken& token) {
    if (impl.closed.load() || !token.is_present()) {
        return false;
    }
    if (!impl.detached()) {
        return StoreAccess::token_is_live(*impl.store, token);
    }
    if (!(token.epoch() == impl.epoch)) {
        return false;
    }
    const auto found = impl.grants.find(grant_key(token.writer(), token.generation()));
    if (found == impl.grants.end() || found->second.revoked) {
        return false;
    }
    if (token.expires_at().has_value() && SystemClock{}.now().unix_nanos() > token.expires_at()->unix_nanos()) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Idempotency bookkeeping
// ---------------------------------------------------------------------------

enum class SequenceDecision { Apply, Replay };

struct SequenceVerdict {
    SequenceDecision decision = SequenceDecision::Apply;
    const internal::StoredIdempotency* replay = nullptr;
    Error error;
};

/// Decides whether a request is a fresh mutation or a replay of one already
/// applied, and refuses anything it cannot prove safe.
[[nodiscard]] SequenceVerdict classify_sequence(IndexedState& state, const RegistryLimits& bounds,
                                                const WriterId& writer, const MutationEnvelope& envelope,
                                                const std::array<std::uint8_t, 16>& fingerprint) {
    SequenceVerdict verdict;
    internal::StoredWriter* tracker = state.find_writer(writer);
    if (tracker == nullptr) {
        // A writer with no recorded history may present any sequence it likes, because
        // there is nothing recorded against it to replay: the rule everywhere else is
        // that a sequence must advance past the writer's recorded high-water mark, and a
        // writer with no record has a high-water mark of zero. Requiring exactly one
        // here would refuse a writer whose attempts were all rejected before they
        // reached the transaction — an invalid request would poison the session that
        // sent it — and it would refuse a writer whose record was evicted to keep the
        // writer table bounded, which the eviction below relies on being able to do.
        if (state.writers.size() >= bounds.max_tracked_writers) {
            // Evicting the writer with the lowest high-water mark keeps the table
            // bounded. Nothing the evicted writer already applied can be replayed,
            // because a replay needs a retained idempotency record.
            auto victim = state.writers.begin();
            for (auto iterator = state.writers.begin(); iterator != state.writers.end(); ++iterator) {
                if (iterator->high_water < victim->high_water) {
                    victim = iterator;
                }
            }
            state.writers.erase(victim);
        }
        return verdict;
    }

    if (tracker->high_water < envelope.sequence) {
        return verdict;
    }
    if (!envelope.idempotency_key.has_value()) {
        verdict.error = reject(ErrorCode::StaleMutationSequence,
                               "the sequence has already been applied and the request carries no idempotency key");
        verdict.error.with_detail("high_water", tracker->high_water.to_string());
        return verdict;
    }
    for (const internal::StoredIdempotency& record : tracker->records) {
        if (!(record.sequence == envelope.sequence)) {
            continue;
        }
        if (record.key != *envelope.idempotency_key) {
            verdict.error = reject(ErrorCode::IdempotencyConflict,
                                   "the sequence was applied under a different idempotency key");
            verdict.error.with_detail("recorded_key", record.key);
            return verdict;
        }
        if (record.fingerprint != fingerprint) {
            verdict.error = reject(ErrorCode::IdempotencyConflict,
                                   "the idempotency key was reused for a different request");
            return verdict;
        }
        verdict.decision = SequenceDecision::Replay;
        verdict.replay = &record;
        return verdict;
    }
    verdict.error = reject(ErrorCode::StaleMutationSequence,
                           "the sequence was applied but its recorded outcome has been pruned from the retained "
                           "history");
    verdict.error.with_detail("retained_from", tracker->low_water.to_string());
    return verdict;
}

void record_outcome(IndexedState& state, const RegistryLimits& bounds, const WriterId& writer,
                    const MutationEnvelope& envelope, const std::array<std::uint8_t, 16>& fingerprint,
                    internal::StoredIdempotency outcome) {
    internal::StoredWriter* tracker = state.find_writer(writer);
    if (tracker == nullptr) {
        internal::StoredWriter fresh;
        fresh.writer = writer;
        fresh.high_water = MutationSequence(0);
        fresh.low_water = MutationSequence(0);
        const std::size_t slot = state.writer_slot_for_insert(writer);
        state.writers.insert(state.writers.begin() + static_cast<std::ptrdiff_t>(slot), std::move(fresh));
        tracker = state.find_writer(writer);
        if (tracker == nullptr) {
            return;
        }
    }
    if (tracker->high_water < envelope.sequence) {
        tracker->high_water = envelope.sequence;
    }
    if (envelope.idempotency_key.has_value()) {
        internal::StoredIdempotency record = std::move(outcome);
        record.sequence = envelope.sequence;
        record.key = *envelope.idempotency_key;
        record.fingerprint = fingerprint;
        tracker->records.push_back(std::move(record));
        while (tracker->records.size() > bounds.max_idempotency_records_per_writer) {
            tracker->records.erase(tracker->records.begin());
        }
    }
    tracker->low_water = tracker->records.empty() ? MutationSequence(tracker->high_water.value() + 1)
                                                  : tracker->records.front().sequence;
}

[[nodiscard]] internal::StoredIdempotency outcome_of(AssetRevision revision, const AssetId& subject) {
    internal::StoredIdempotency stored;
    stored.outcome_kind = internal::StoredOutcomeKind::Revision;
    stored.revision = revision.value();
    stored.asset = subject;
    return stored;
}

[[nodiscard]] internal::StoredIdempotency outcome_of_error(const Error& error) {
    internal::StoredIdempotency stored;
    stored.outcome_kind = internal::StoredOutcomeKind::Error;
    stored.error_code = error.code();
    stored.error_message = error.message();
    return stored;
}

/// Rebuilds the originating Error from a recorded rejection. The message is the
/// message the original call produced, so a retry observes the same explanation.
[[nodiscard]] Error error_from(const internal::StoredIdempotency& recorded) {
    return Error(recorded.error_code, recorded.error_message);
}

// ---------------------------------------------------------------------------
// Record helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::optional<Error> canonicalise_references(std::vector<Reference>& references,
                                                          const RegistryLimits& bounds) {
    if (references.size() > bounds.max_references_per_asset) {
        return reject(ErrorCode::ReferenceCountExceeded,
                      "the record would carry more references than the configured bound");
    }
    std::sort(references.begin(), references.end());
    for (std::size_t index = 1; index < references.size(); ++index) {
        if (references[index - 1] == references[index]) {
            return reject(ErrorCode::ReferenceAlreadyAttached,
                          "the reference set repeats: " + references[index].canonical());
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<Error> canonicalise_labels(std::vector<std::pair<std::string, std::string>>& labels,
                                                      const RegistryLimits& bounds) {
    if (labels.size() > bounds.max_labels_per_asset) {
        return reject(ErrorCode::CapacityExceeded, "the record would carry more labels than the configured bound");
    }
    std::sort(labels.begin(), labels.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    for (std::size_t index = 1; index < labels.size(); ++index) {
        if (labels[index - 1].first == labels[index].first) {
            return reject(ErrorCode::InvalidInput, "the label set repeats the key: " + labels[index].first);
        }
    }
    return std::nullopt;
}

/// Appends a provenance step, compacting the middle of the history when the
/// retained bound is reached.
///
/// Layout at the bound: the origin step, one compaction step that states how many
/// steps it replaced, then the newest steps. Appending at the bound therefore frees
/// the two slots it needs — one for the compaction step, one for the step being
/// appended — by dropping the oldest steps that sit between the origin and the steps
/// that survive, and the compaction step then takes the place of the last of them.
/// Because that step is the one the marker stands in for, the marker carries its
/// sequence and revision, which is what keeps the retained history strictly ascending
/// in both. The origin survives whenever the bound leaves room for it beside the
/// newest step; a bound of one cannot hold both ends, and the newest step is the one
/// the record's own revision and sequence are checked against, so it is kept.
void push_provenance(AssetRecord& record, const RegistryLimits& bounds, ProvenanceStep step) {
    record.revision = step.revision;
    record.last_sequence = step.sequence;
    const std::size_t bound = bounds.max_provenance_per_asset;
    if (record.provenance.size() < bound) {
        record.provenance.push_back(std::move(step));
        return;
    }
    constexpr std::string_view kMarker = "compacted ";
    std::uint64_t dropped = 0;
    const bool marker_present = record.provenance.size() >= 2 &&
                                record.provenance[1].reason.rfind(kMarker, 0) == 0;
    if (marker_present) {
        const std::string_view text(record.provenance[1].reason);
        const std::size_t space = text.find(' ', kMarker.size());
        if (space != std::string_view::npos) {
            const auto parsed = parse_unsigned_decimal(text.substr(kMarker.size(), space - kMarker.size()));
            if (parsed.has_value()) {
                dropped = *parsed;
            }
        }
    }

    // The origin and one compaction step are kept when the bound leaves room for them beside at
    // least one newest step.
    const std::size_t marker_slots = bound >= 3 ? 1 : 0;
    const std::size_t origin_slots = bound >= 2 ? 1 : 0;
    const std::size_t newest_slots = bound - marker_slots - origin_slots;
    const std::size_t keep_newest = newest_slots - 1;  // the incoming step takes one of them
    const std::size_t first_newest =
        record.provenance.size() > keep_newest ? record.provenance.size() - keep_newest : origin_slots;

    // The steps between the origin and the steps that survive are the ones being dropped, and the
    // last of them is the step the compaction marker replaces.
    TransactionSequence replaced_sequence = step.sequence;
    AssetRevision replaced_revision = step.revision;
    for (std::size_t index = origin_slots; index < first_newest && index < record.provenance.size(); ++index) {
        const ProvenanceStep& replaced = record.provenance[index];
        replaced_sequence = replaced.sequence;
        replaced_revision = replaced.revision;
        const bool is_previous_marker = marker_present && index == 1;
        if (!is_previous_marker) {
            ++dropped;
        }
    }

    std::vector<ProvenanceStep> retained;
    retained.reserve(bound);
    if (origin_slots != 0) {
        retained.push_back(std::move(record.provenance.front()));
    }
    if (marker_slots != 0) {
        ProvenanceStep compaction = step;
        compaction.sequence = replaced_sequence;
        compaction.revision = replaced_revision;
        compaction.action = ProvenanceAction::MetadataUpdated;
        compaction.reason = "compacted " + std::to_string(dropped) + " intermediate provenance steps";
        compaction.change = "retained_provenance=" + std::to_string(bound);
        retained.push_back(std::move(compaction));
    }
    for (std::size_t index = first_newest; index < record.provenance.size(); ++index) {
        retained.push_back(std::move(record.provenance[index]));
    }
    retained.push_back(std::move(step));
    record.provenance = std::move(retained);
}

[[nodiscard]] ProvenanceStep make_step(TransactionSequence sequence, RegistryEpoch epoch, AssetRevision revision,
                                       ProvenanceAction action, ProvenanceOrigin origin, const ActorRef& actor,
                                       const Timestamp& now, std::string reason, std::string change) {
    ProvenanceStep step;
    step.sequence = sequence;
    step.epoch = epoch;
    step.revision = revision;
    step.action = action;
    step.origin = origin;
    step.actor = actor;
    step.recorded_at = now;
    step.reason = std::move(reason);
    step.change = std::move(change);
    return step;
}

[[nodiscard]] std::optional<Error> check_expectations(const AssetRecord& record, RevisionExpectation expected_revision,
                                                      GenerationExpectation expected_generation) {
    // An error carries one structured detail pair, and the value a caller can act on is the one it
    // asked for: it re-reads the record, compares against that expectation, and retries. The
    // observed value goes into the message so the reason is complete without a second detail.
    if (expected_revision.has_value() && !(record.revision == *expected_revision)) {
        Error error(ErrorCode::StaleRevision,
                    "the record is at revision " + record.revision.to_string() +
                        ", not the revision this mutation was prepared against");
        error.with_subject(record.id.to_string());
        error.with_detail("expected_revision", expected_revision->to_string());
        return error;
    }
    if (expected_generation.has_value() && !(record.generation == *expected_generation)) {
        Error error(ErrorCode::StaleGeneration,
                    "the record is at identity generation " + record.generation.to_string() +
                        ", not the generation this mutation expects");
        error.with_subject(record.id.to_string());
        error.with_detail("expected_generation", expected_generation->to_string());
        return error;
    }
    return std::nullopt;
}

[[nodiscard]] AssetId derive_asset_id(AssetClass asset_class, const SerialIdentity& identity) {
    std::string name = std::string(to_string(asset_class));
    name += '|';
    name += identity.canonical_key();
    return internal::uuid_v5(internal::asset_namespace_uuid(), name);
}

/// True when a reference names a target of its own kind. A reference built with an empty
/// target still carries the canonical key of its kind ("capability:", "location:", ...), so
/// the target is what decides whether the reference can be stored.
[[nodiscard]] bool reference_has_target(const Reference& reference) {
    switch (reference.kind()) {
        case Reference::Kind::Capability:
            return !reference.capability().empty();
        case Reference::Kind::Location:
            return !reference.location().empty();
        case Reference::Kind::Rack:
            return !reference.rack().empty();
        case Reference::Kind::ExternalObject:
            return !reference.external_object().empty();
    }
    return false;
}

/// Collects the identities a mutation touched, keeping validation order
/// deterministic.
class Touched {
public:
    void add(const AssetId& id) {
        if (std::find(ids_.begin(), ids_.end(), id) == ids_.end()) {
            ids_.push_back(id);
        }
    }
    [[nodiscard]] const std::vector<AssetId>& ids() const noexcept { return ids_; }

private:
    std::vector<AssetId> ids_;
};

/// The description a mutator produces for the provenance change field.
struct ChangeText {
    ProvenanceAction action = ProvenanceAction::MetadataUpdated;
    std::string text;
};


// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------

/// One mutation attempt: the commit lock, the base state, the working copy, the
/// authority and idempotency decision, and the publish step, in one object so no
/// command can forget a stage.
class Txn {
public:
    Txn(RegistryImpl& impl, WriterSession& session, const MutationEnvelope& envelope, std::string_view domain,
        std::string_view request_material)
        : impl_(impl),
          session_(session),
          envelope_(envelope),
          lock_(impl.commit_mutex),
          base_(read_published(impl)),
          working_(base_->state),
          fingerprint_(internal::request_fingerprint(domain, request_material)) {}

    Txn(const Txn&) = delete;
    Txn& operator=(const Txn&) = delete;

    /// Authority, sequence, and idempotency decision. A false return means the
    /// request must not proceed and failure() explains why.
    [[nodiscard]] bool begin() {
        if (impl_.closed.load()) {
            failure_ = reject(ErrorCode::RegistryClosed, "the registry is closed");
            return false;
        }
        if (session_.writer().empty() || !session_.is_armed()) {
            failure_ = reject(ErrorCode::AuthorityRevoked,
                              "the writer session holds no authority token; open a writer before mutating");
            return false;
        }
        if (!(session_.token().writer() == session_.writer())) {
            failure_ =
                reject(ErrorCode::AuthorityRevoked, "the session token was minted for a different writer identity");
            return false;
        }
        if (envelope_.sequence.is_zero()) {
            failure_ = reject(ErrorCode::InvalidInput, "mutation sequence must be at least one");
            return false;
        }
        if (envelope_.reason.size() > limits::kMaxReasonBytes ||
            validate_utf8(envelope_.reason, true) != Utf8Status::Valid) {
            failure_ = reject(ErrorCode::MalformedText, "mutation reason violates text rules");
            return false;
        }
        if (envelope_.idempotency_key.has_value()) {
            const std::string& key = *envelope_.idempotency_key;
            if (key.empty() || key.size() > 128 || validate_utf8(key, false) != Utf8Status::Valid) {
                failure_ =
                    reject(ErrorCode::InvalidInput, "idempotency key must be 1 to 128 bytes of structurally valid text");
                return false;
            }
        }
        if (!token_is_live(impl_, session_.token())) {
            failure_ = impl_.detached()
                           ? reject(ErrorCode::AuthorityRevoked, "the authority token is not live for this registry")
                           : reject(ErrorCode::StaleAuthorityEpoch,
                                    "the authority token was minted under an earlier store epoch; the store has been "
                                    "reopened since it was issued");
            return false;
        }

        SequenceVerdict verdict = classify_sequence(working_, impl_.bounds, session_.writer(), envelope_, fingerprint_);
        if (verdict.decision == SequenceDecision::Replay) {
            replay_ = verdict.replay;
            return true;
        }
        if (verdict.error.code() != ErrorCode::None) {
            failure_ = std::move(verdict.error);
            return false;
        }
        return true;
    }

    [[nodiscard]] const RegistryBody& base() const noexcept { return *base_; }
    [[nodiscard]] IndexedState& state() noexcept { return working_; }
    [[nodiscard]] const IndexedState& state() const noexcept { return working_; }
    [[nodiscard]] const RegistryPolicy& policy() const noexcept { return impl_.policy; }
    [[nodiscard]] const RegistryLimits& bounds() const noexcept { return impl_.bounds; }
    [[nodiscard]] TransactionSequence next_sequence() const noexcept {
        return TransactionSequence(base_->sequence.value() + 1);
    }
    [[nodiscard]] RegistryEpoch epoch() const noexcept { return base_->epoch; }
    [[nodiscard]] const WriterId& writer() const noexcept { return session_.writer(); }
    [[nodiscard]] const std::string& reason() const noexcept { return envelope_.reason; }
    [[nodiscard]] bool replayed() const noexcept { return replay_ != nullptr; }
    [[nodiscard]] const internal::StoredIdempotency& replay() const noexcept { return *replay_; }
    [[nodiscard]] const Error& failure() const noexcept { return failure_; }

    /// Copies the record out of the working state. Absent when it is not present.
    [[nodiscard]] std::optional<AssetRecord> load(const AssetId& id) const {
        const RecordPlacement placement = working_.find(id);
        if (!placement.found) {
            return std::nullopt;
        }
        return *working_.records[placement.index];
    }

    /// Stores an updated record in place. Identity is immutable, so this never
    /// changes the canonical ordering.
    void store(std::shared_ptr<const AssetRecord> record) {
        const RecordPlacement placement = working_.find(record->id);
        if (placement.found) {
            working_.replace_at(placement.index, std::move(record));
        }
    }

    /// Adds a new record and rebuilds the identity index, because an insertion
    /// shifts every slot after it.
    void insert(std::shared_ptr<const AssetRecord> record) {
        (void)working_.insert(std::move(record));
        refresh_serial_index();
    }

    /// The record that currently claims a serial identity, if any.
    [[nodiscard]] std::optional<AssetId> serial_claimant(const SerialIdentity& identity) const {
        const auto& index = serial_view();
        const auto found = index.find(identity.canonical_key());
        if (found == index.end() || found->second >= working_.ids.size()) {
            return std::nullopt;
        }
        return working_.ids[found->second];
    }

    /// Validates every modified record with the same predicate the durable store
    /// applies on load, then publishes and records the idempotency outcome. `kind`
    /// says what a replay of this request must report: the revision a record mutation
    /// produced, or the identity a registration created. The identity travels in the
    /// outcome so a retry that arrives after a restart still names the asset the
    /// original call registered.
    Outcome<TransactionSequence> commit(AssetRevision revision, const AssetId& subject,
                                        const std::vector<AssetId>& touched,
                                        internal::StoredOutcomeKind kind = internal::StoredOutcomeKind::Revision) {
        for (const AssetId& id : touched) {
            const RecordPlacement placement = working_.find(id);
            if (!placement.found) {
                return reject(ErrorCode::InternalInvariantViolation,
                              "a mutation reported touching a record that is not present");
            }
            const auto validation =
                validate_asset_record(*working_.records[placement.index], impl_.bounds, impl_.policy.consistency);
            if (validation.has_value()) {
                return reject(ErrorCode::InternalInvariantViolation,
                              "the mutation would produce an invalid record: " + *validation);
            }
        }
        if (working_.records.size() > impl_.bounds.max_assets) {
            return reject(ErrorCode::CapacityExceeded,
                          "the registry would hold more assets than the configured bound");
        }
        std::uint64_t footprint = 0;
        for (const RecordPtr& record : working_.records) {
            footprint += record->estimated_bytes();
        }
        if (footprint > impl_.bounds.max_registry_bytes) {
            return reject(ErrorCode::CapacityExceeded, "the inventory would exceed the configured in-memory bound");
        }

        // The outcome is written into the working state before it is published, so
        // the writer's sequence history travels in the same body the mutation
        // produced. Recording it after publication would leave the published body
        // without it, and every later mutation from that writer would look like a
        // first mutation and be refused.
        internal::StoredIdempotency outcome = outcome_of(revision, subject);
        outcome.outcome_kind = kind;
        record_outcome(working_, impl_.bounds, session_.writer(), envelope_, fingerprint_, std::move(outcome));
        auto published = publish(next_sequence());
        if (!published) {
            return published.error();
        }
        return published.value();
    }

    /// Durably records a rejection as the outcome for this sequence, so a later
    /// retry observes the same answer instead of silently succeeding against
    /// changed state. A failure to record is deliberately not propagated: the
    /// mutation did not happen either way, and the caller is told the original
    /// reason.
    void record_rejection(const Error& error) {
        if (impl_.closed.load() || published_) {
            return;
        }
        record_outcome(working_, impl_.bounds, session_.writer(), envelope_, fingerprint_, outcome_of_error(error));
        auto recorded = publish(next_sequence());
        (void)recorded;
    }

private:
    [[nodiscard]] const std::unordered_map<std::string, std::size_t>& serial_view() const {
        return rebuilt_serial_.empty() ? base_->serial_index : rebuilt_serial_;
    }

    void refresh_serial_index() {
        rebuilt_serial_.clear();
        rebuilt_serial_.reserve(working_.records.size());
        for (std::size_t slot = 0; slot < working_.records.size(); ++slot) {
            rebuilt_serial_.emplace(working_.records[slot]->serial_identity.canonical_key(), slot);
        }
    }

    Outcome<TransactionSequence> publish(TransactionSequence expected) {
        if (!impl_.detached()) {
            internal::StorePayload payload;
            payload.policy = impl_.policy;
            payload.limits = impl_.bounds;
            payload.records = working_.records;
            payload.writers = working_.writers;
            auto committed = StoreAccess::commit(*impl_.store, payload, session_.writer());
            if (!committed) {
                return committed.error();
            }
            if (!(committed.value() == expected)) {
                return reject(ErrorCode::InternalInvariantViolation,
                              "the store published a transaction sequence other than the one the mutation intended");
            }
            install_published(impl_, detail::make_body(impl_.policy, impl_.bounds,
                                                       StoreAccess::current_epoch(*impl_.store), committed.value(),
                                                       working_));
            published_ = true;
            return committed.value();
        }
        install_published(impl_, detail::make_body(impl_.policy, impl_.bounds, impl_.epoch, expected, working_));
        return expected;
    }

    RegistryImpl& impl_;
    WriterSession& session_;
    const MutationEnvelope& envelope_;
    std::unique_lock<std::mutex> lock_;
    std::shared_ptr<const RegistryBody> base_;
    IndexedState working_;
    std::array<std::uint8_t, 16> fingerprint_{};
    std::unordered_map<std::string, std::size_t> rebuilt_serial_;
    const internal::StoredIdempotency* replay_ = nullptr;
    Error failure_;
    /// True once this attempt has published a generation, so a mutation can never
    /// publish twice and a late rejection cannot overwrite a published outcome.
    bool published_ = false;
};

/// Shared shape of every record mutation: replay, lookup, expectation check,
/// apply, publish. `mutator` returns the provenance change description, or an
/// Error when the requested change is not permitted.
template <typename Mutator>
Outcome<MutationResult> run_record_mutation(RegistryImpl& impl, WriterSession& session,
                                            const MutationEnvelope& envelope, std::string_view domain,
                                            std::string_view request_material, const AssetId& id,
                                            RevisionExpectation expected_revision,
                                            GenerationExpectation expected_generation, Mutator&& mutator) {
    Txn txn(impl, session, envelope, domain, request_material);
    if (!txn.begin()) {
        txn.record_rejection(txn.failure());
        return txn.failure();
    }
    if (txn.replayed()) {
        const internal::StoredIdempotency& recorded = txn.replay();
        if (recorded.outcome_kind == internal::StoredOutcomeKind::Error) {
            return error_from(recorded);
        }
        MutationResult result;
        result.idempotent_replay = true;
        result.revision = AssetRevision(recorded.revision);
        result.subject = recorded.asset.is_nil() ? id : recorded.asset;
        return result;
    }

    auto existing = txn.load(id);
    if (!existing.has_value()) {
        Error error(ErrorCode::AssetNotFound, "asset is not present in the registry");
        error.with_subject(id.to_string());
        txn.record_rejection(error);
        return error;
    }
    if (const auto failure = check_expectations(*existing, expected_revision, expected_generation);
        failure.has_value()) {
        txn.record_rejection(*failure);
        return *failure;
    }
    const auto revision = existing->revision.next();
    if (!revision.has_value()) {
        const Error error = reject(ErrorCode::CapacityExceeded, "the record revision counter is exhausted");
        txn.record_rejection(error);
        return error;
    }

    auto updated = std::make_shared<AssetRecord>(*existing);
    const Timestamp now = SystemClock{}.now();
    const ActorRef actor = ActorRef::writer(txn.writer());
    auto change = mutator(txn, *updated, actor, now, *revision);
    if (!change) {
        txn.record_rejection(change.error());
        return change.error();
    }
    if (change.value().text.empty()) {
        const Error error = reject(ErrorCode::InvalidInput, "the mutation would not change the record");
        txn.record_rejection(error);
        return error;
    }
    push_provenance(*updated, impl.bounds,
                    make_step(txn.next_sequence(), txn.epoch(), *revision, change.value().action,
                              ProvenanceOrigin::ApiMutation, actor, now, txn.reason(), change.value().text));
    txn.store(std::move(updated));

    Touched touched;
    touched.add(id);
    const auto published = txn.commit(*revision, id, touched.ids());
    if (!published) {
        return published.error();
    }
    MutationResult result;
    result.revision = *revision;
    result.subject = id;
    result.published_sequence = published.value();
    return result;
}


// ---------------------------------------------------------------------------
// register_asset
// ---------------------------------------------------------------------------

/// Validates and canonicalises a register request, so the fingerprint covers the
/// canonical spelling rather than the caller's ordering of labels and references.
[[nodiscard]] Outcome<RegisterAssetRequest> canonicalise_register(RegisterAssetRequest request,
                                                                 const RegistryLimits& bounds) {
    if (request.id_mode == AssetIdMode::CallerSupplied) {
        if (request.id.is_nil()) {
            return reject(ErrorCode::MalformedAssetId,
                          "a caller-supplied canonical identity must not be the nil identifier");
        }
    } else if (!request.id.is_nil()) {
        return reject(ErrorCode::InvalidInput,
                      "the request supplies both a derived identity mode and an explicit identity; exactly one "
                      "must be used");
    }
    if (!is_known(request.asset_class)) {
        return reject(ErrorCode::UnknownAssetClass,
                      "the asset class must be a known physical class; an object whose class is not recognised is "
                      "rejected rather than stored as unclassified");
    }
    if (request.serial_identity.empty()) {
        return reject(ErrorCode::MalformedSerialIdentity,
                      "a manufacturer and serial identity is required to register a physical asset");
    }
    if (request.metadata.display_name.empty()) {
        return reject(ErrorCode::EmptyRequiredField, "the asset display name must not be empty");
    }
    if (!is_display_text(request.metadata.display_name, bounds.max_display_name_bytes)) {
        return reject(ErrorCode::MalformedText,
                      "the asset display name must be structurally valid UTF-8 without control characters and "
                      "within the configured length bound");
    }
    if (request.metadata.notes.size() > limits::kMaxNotesBytes ||
        validate_utf8(request.metadata.notes, true) != Utf8Status::Valid) {
        return reject(ErrorCode::MalformedText, "asset notes are too long or are not structurally valid UTF-8");
    }
    for (const auto& label : request.metadata.labels) {
        if (!is_label_key(label.first)) {
            return reject(ErrorCode::MalformedLabelKey, "label key violates label syntax: " + label.first);
        }
        if (label.second.size() > bounds.max_label_value_bytes ||
            validate_utf8(label.second, true) != Utf8Status::Valid) {
            return reject(ErrorCode::MalformedText, "label value violates text rules for key: " + label.first);
        }
    }
    if (const auto failure = canonicalise_labels(request.metadata.labels, bounds); failure.has_value()) {
        return *failure;
    }
    if (const auto failure = canonicalise_references(request.references, bounds); failure.has_value()) {
        return *failure;
    }
    if (request.supersession_note.size() > limits::kMaxReplacementReasonBytes ||
        validate_utf8(request.supersession_note, true) != Utf8Status::Valid) {
        return reject(ErrorCode::MalformedText,
                      "the replacement note is too long or is not structurally valid UTF-8");
    }
    if (const auto consistency =
            check_state_consistency(request.lifecycle, request.installation, default_consistency_rules());
        consistency.has_value()) {
        return reject(ErrorCode::LifecycleInvariantViolation,
                      "the requested initial state violates the consistency rules: " + *consistency);
    }
    if (request.lifecycle != LifecycleState::Planned && request.lifecycle != LifecycleState::Provisioned &&
        request.lifecycle != LifecycleState::Unknown) {
        return reject(ErrorCode::LifecycleInvariantViolation,
                      "a newly registered asset cannot begin in a later lifecycle state; registration starts at "
                      "planned, provisioned, or an explicit unknown state");
    }
    return request;
}

Outcome<RegisterResult> AssetRegistry::register_asset(WriterSession& session, const MutationEnvelope& envelope,
                                                      const RegisterAssetRequest& request) {
    auto canonical = canonicalise_register(request, impl_->bounds);
    if (!canonical) {
        return canonical.error();
    }
    const RegisterAssetRequest prepared = canonical.value();

    Txn txn(*impl_, session, envelope, "register_asset", material_register(prepared));
    if (!txn.begin()) {
        txn.record_rejection(txn.failure());
        return txn.failure();
    }
    if (txn.replayed()) {
        const internal::StoredIdempotency& recorded = txn.replay();
        if (recorded.outcome_kind == internal::StoredOutcomeKind::Error) {
            return error_from(recorded);
        }
        RegisterResult result;
        result.id = recorded.asset;
        result.revision = AssetRevision(recorded.revision);
        result.idempotent_replay = true;
        result.derived_identity = prepared.id_mode == AssetIdMode::DerivedFromSerialIdentity;
        return result;
    }

    AssetId id = prepared.id;
    bool derived = false;
    if (prepared.id_mode == AssetIdMode::DerivedFromSerialIdentity) {
        id = derive_asset_id(prepared.asset_class, prepared.serial_identity);
        derived = true;
    } else if (prepared.id_mode == AssetIdMode::GeneratedRandom) {
        const auto generated = AssetId::generate();
        if (!generated) {
            return generated.error();
        }
        id = generated.value();
    }

    if (const auto existing = txn.load(id); existing.has_value()) {
        if (derived && existing->serial_identity == prepared.serial_identity &&
            existing->asset_class == prepared.asset_class) {
            // The same physical object under the same derivation: a re-import,
            // not a conflict.
            RegisterResult result;
            result.id = id;
            result.revision = existing->revision;
            result.already_present = true;
            result.derived_identity = true;
            return result;
        }
        Error error(ErrorCode::DuplicateAssetId, "the canonical identity is already claimed by another record");
        error.with_subject(id.to_string());
        txn.record_rejection(error);
        return error;
    }
    if (txn.state().records.size() >= impl_->bounds.max_assets) {
        const Error error = reject(ErrorCode::CapacityExceeded, "the registry is at its configured asset bound");
        txn.record_rejection(error);
        return error;
    }

    const std::optional<AssetId> claimant = txn.serial_claimant(prepared.serial_identity);
    std::optional<AssetRecord> predecessor;
    if (claimant.has_value()) {
        const auto holder = txn.load(*claimant);
        if (!holder.has_value()) {
            return reject(ErrorCode::InternalInvariantViolation,
                          "the serial identity index names a record that is not present");
        }
        if (impl_->policy.model_conflict == ModelConflictPolicy::Reject &&
            holder->serial_identity.model_conflicts_with(prepared.serial_identity)) {
            Error error(ErrorCode::DuplicateSerialIdentity,
                        "the manufacturer and serial are already claimed by a record whose model differs");
            error.with_subject(claimant->to_string());
            txn.record_rejection(error);
            return error;
        }
        if (impl_->policy.serial_collision == SerialCollisionPolicy::Reject) {
            Error error(ErrorCode::DuplicateSerialIdentity,
                        "the manufacturer and serial identity is already claimed by another asset; a duplicate "
                        "identity is rejected rather than stored");
            error.with_subject(claimant->to_string());
            error.with_detail("existing_asset", claimant->to_string());
            txn.record_rejection(error);
            return error;
        }
        if (!is_terminal_lifecycle(holder->state.lifecycle)) {
            Error error(ErrorCode::DuplicateSerialIdentity,
                        "the serial identity is claimed by an asset that is not in a terminal lifecycle state, so "
                        "a replacement is not permitted");
            error.with_subject(claimant->to_string());
            txn.record_rejection(error);
            return error;
        }
        if (!prepared.supersedes.has_value() || !(*prepared.supersedes == *claimant)) {
            Error error(ErrorCode::DuplicateSerialIdentity,
                        "the serial identity is claimed by a retired asset; a replacement registration must "
                        "declare that asset as its predecessor");
            error.with_subject(claimant->to_string());
            txn.record_rejection(error);
            return error;
        }
        predecessor = holder;
    }

    std::optional<ReplacementLink> link;
    if (prepared.supersedes.has_value()) {
        const AssetId target = *prepared.supersedes;
        if (target == id) {
            const Error error = reject(ErrorCode::SelfReplacementForbidden, "an asset cannot supersede itself");
            txn.record_rejection(error);
            return error;
        }
        if (!predecessor.has_value()) {
            const auto existing = txn.load(target);
            if (!existing.has_value()) {
                if (!prepared.allow_dangling_predecessor) {
                    Error error(ErrorCode::AssetNotFound,
                                "the declared predecessor is not present in this registry; a replacement that "
                                "names an unknown asset is rejected unless the request explicitly allows a "
                                "dangling link");
                    error.with_subject(target.to_string());
                    txn.record_rejection(error);
                    return error;
                }
            } else if (!is_terminal_lifecycle(existing->state.lifecycle) &&
                       impl_->policy.require_terminal_before_supersede) {
                Error error(ErrorCode::LifecycleInvariantViolation,
                            "the declared predecessor is not in a terminal lifecycle state, so it cannot be "
                            "superseded; decommission it first");
                error.with_subject(target.to_string());
                txn.record_rejection(error);
                return error;
            } else {
                predecessor = existing;
            }
        }
        // A predecessor that is itself a replacement is legitimate: a unit that was replaced
        // once can be replaced again, and the successor names the object it physically took
        // over from. What is not legitimate is a link that closes a cycle, and a cycle is
        // reachable: a predecessor may be recorded as a dangling claim before the asset it
        // names exists, and that asset can later name the claimant. The chain is therefore
        // walked here, bounded, exactly as link_replacement walks it, so registration cannot
        // publish a lineage the audit would have to call clean while traversals report a
        // cycle.
        {
            std::unordered_set<std::string> visited;
            visited.insert(id.to_compact_string());
            AssetId cursor = predecessor.has_value() ? predecessor->id : target;
            for (std::size_t hop = 0;; ++hop) {
                if (!visited.insert(cursor.to_compact_string()).second) {
                    Error error(ErrorCode::ReplacementCycleDetected,
                                "the link would close a cycle in replacement lineage");
                    error.with_subject(id.to_string());
                    txn.record_rejection(error);
                    return error;
                }
                const auto current = txn.load(cursor);
                if (!current.has_value() || !current->supersedes.has_value()) {
                    break;
                }
                if (hop >= limits::kMaxLineageDepth) {
                    Error error(ErrorCode::LineageTraversalLimitExceeded,
                                "the predecessor chain is longer than the traversal bound, so the link cannot be "
                                "proved acyclic");
                    error.with_subject(id.to_string());
                    txn.record_rejection(error);
                    return error;
                }
                cursor = current->supersedes->predecessor;
            }
        }
        ReplacementLink replacement;
        replacement.predecessor = predecessor.has_value() ? predecessor->id : target;
        replacement.predecessor_generation = predecessor.has_value() ? predecessor->generation : AssetGeneration(1);
        replacement.predecessor_final_revision =
            predecessor.has_value() ? predecessor->revision : AssetRevision(1);
        replacement.cause = prepared.supersession_cause;
        replacement.linked_at = txn.next_sequence();
        replacement.note = prepared.supersession_note;
        link = std::move(replacement);
    }

    auto record = std::make_shared<AssetRecord>();
    record->id = id;
    record->asset_class = prepared.asset_class;
    record->generation = AssetGeneration(1);
    record->revision = AssetRevision(1);
    record->last_sequence = txn.next_sequence();
    record->serial_identity = prepared.serial_identity;
    record->metadata = prepared.metadata;
    record->references = prepared.references;
    record->state.lifecycle = prepared.lifecycle;
    record->state.installation = prepared.installation;
    record->supersedes = link;
    record->provenance.push_back(make_step(txn.next_sequence(), txn.epoch(), AssetRevision(1),
                                           ProvenanceAction::Registered, ProvenanceOrigin::ApiMutation,
                                           ActorRef::writer(txn.writer()), SystemClock{}.now(),
                                           txn.reason().empty() ? std::string("asset registered") : txn.reason(),
                                           std::string("origin=new")));
    if (const auto validation = validate_asset_record(*record, impl_->bounds, impl_->policy.consistency);
        validation.has_value()) {
        const Error error =
            reject(ErrorCode::InvalidInput, "the registration would produce an invalid record: " + *validation);
        txn.record_rejection(error);
        return error;
    }

    Touched touched;
    touched.add(id);
    if (predecessor.has_value()) {
        // The predecessor keeps its own revision: the link lives on the successor,
        // so nothing about the predecessor's authority changes.
        touched.add(predecessor->id);
    }
    txn.insert(std::move(record));

    // A registration's outcome carries the identity it created, so a retry that arrives after a
    // restart replays the same answer: the identity is derived from the request, but a caller
    // supplied identity is not, and only the recorded outcome names it.
    const auto published = txn.commit(AssetRevision(1), id, touched.ids(),
                                      internal::StoredOutcomeKind::RegisteredAsset);
    if (!published) {
        return published.error();
    }
    RegisterResult result;
    result.id = id;
    result.revision = AssetRevision(1);
    result.derived_identity = derived;
    result.published_sequence = published.value();
    return result;
}
Outcome<MutationResult> AssetRegistry::update_metadata(WriterSession& session, const MutationEnvelope& envelope,
                                                       const AssetId& id, RevisionExpectation expected,
                                                       const MetadataPatch& patch) {
    return run_record_mutation(
        *impl_, session, envelope, "update_metadata", join_fields({id.to_compact_string(), material_patch(patch)}),
        id, expected, std::nullopt,
        [&patch](Txn& txn, AssetRecord& record, const ActorRef& actor, const Timestamp& now,
                 AssetRevision revision) -> Outcome<ChangeText> {
            (void)actor;
            (void)now;
            (void)revision;
            AssetMetadata updated = record.metadata;
            std::string change;
            const auto note = [&change](std::string piece) {
                if (!change.empty()) {
                    change += ';';
                }
                change += std::move(piece);
            };
            if (patch.display_name.has_value()) {
                if (!is_display_text(*patch.display_name, txn.bounds().max_display_name_bytes)) {
                    return reject(ErrorCode::MalformedText,
                                  "the display name must be structurally valid UTF-8 without control characters and "
                                  "within the configured length bound");
                }
                updated.display_name = *patch.display_name;
                note("display_name");
            }
            if (patch.clear_owner) {
                if (!updated.owner.has_value()) {
                    return reject(ErrorCode::AssetNotFound, "the record has no owner to clear");
                }
                updated.owner.reset();
                note("owner=cleared");
            } else if (patch.owner.has_value()) {
                updated.owner = *patch.owner;
                note("owner=" + patch.owner->text());
            }
            if (patch.clear_site) {
                if (!updated.site.has_value()) {
                    return reject(ErrorCode::AssetNotFound, "the record has no site to clear");
                }
                updated.site.reset();
                note("site=cleared");
            } else if (patch.site.has_value()) {
                updated.site = *patch.site;
                note("site=" + patch.site->text());
            }
            if (patch.clear_notes) {
                if (updated.notes.empty()) {
                    return reject(ErrorCode::InvalidInput, "the record has no notes to clear");
                }
                updated.notes.clear();
                note("notes=cleared");
            } else if (patch.notes.has_value()) {
                if (patch.notes->size() > limits::kMaxNotesBytes ||
                    validate_utf8(*patch.notes, true) != Utf8Status::Valid) {
                    return reject(ErrorCode::MalformedText,
                                  "notes are too long or are not structurally valid UTF-8");
                }
                updated.notes = *patch.notes;
                note("notes");
            }
            for (const std::string& key : patch.remove_labels) {
                if (!is_label_key(key)) {
                    return reject(ErrorCode::MalformedLabelKey, "label key violates label syntax: " + key);
                }
                const auto before = updated.labels.size();
                updated.labels.erase(std::remove_if(updated.labels.begin(), updated.labels.end(),
                                                    [&key](const auto& entry) { return entry.first == key; }),
                                     updated.labels.end());
                if (updated.labels.size() == before) {
                    return reject(ErrorCode::AssetNotFound, "the record has no label with key: " + key);
                }
                note("label-" + key);
            }
            for (const auto& entry : patch.set_labels) {
                if (!is_label_key(entry.first)) {
                    return reject(ErrorCode::MalformedLabelKey, "label key violates label syntax: " + entry.first);
                }
                if (entry.second.size() > txn.bounds().max_label_value_bytes ||
                    validate_utf8(entry.second, true) != Utf8Status::Valid) {
                    return reject(ErrorCode::MalformedText, "label value violates text rules for key: " + entry.first);
                }
                const auto position = std::find_if(updated.labels.begin(), updated.labels.end(),
                                                   [&entry](const auto& item) { return item.first == entry.first; });
                if (position == updated.labels.end()) {
                    updated.labels.emplace_back(entry.first, entry.second);
                } else {
                    position->second = entry.second;
                }
                note("label+" + entry.first);
            }
            if (const auto failure = canonicalise_labels(updated.labels, txn.bounds()); failure.has_value()) {
                return *failure;
            }
            if (updated == record.metadata) {
                return reject(ErrorCode::InvalidInput, "the patch would not change any metadata field");
            }
            const bool owner_changed = !(updated.owner == record.metadata.owner);
            record.metadata = std::move(updated);
            ChangeText text;
            text.action = owner_changed ? ProvenanceAction::OwnerChanged : ProvenanceAction::MetadataUpdated;
            text.text = change.empty() ? std::string("metadata") : std::move(change);
            return text;
        });
}

Outcome<MutationResult> AssetRegistry::reclassify_asset(WriterSession& session, const MutationEnvelope& envelope,
                                                        const AssetId& id, RevisionExpectation expected,
                                                        AssetClass new_class) {
    if (!is_known(new_class)) {
        return reject(ErrorCode::UnknownAssetClass,
                      "the target class must be a known physical class; downstream consumers key physical "
                      "behaviour off the class, so reclassifying to unclassified is rejected");
    }
    return run_record_mutation(
        *impl_, session, envelope, "reclassify_asset", join_fields({id.to_compact_string(), to_string(new_class)}), id,
        expected, std::nullopt,
        [new_class](Txn& txn, AssetRecord& record, const ActorRef& actor, const Timestamp& now,
                    AssetRevision revision) -> Outcome<ChangeText> {
            (void)txn;
            (void)actor;
            (void)now;
            (void)revision;
            if (record.asset_class == new_class) {
                return reject(ErrorCode::InvalidInput, "the record is already in the requested class");
            }
            std::string change = "class=";
            change += to_string(record.asset_class);
            change += "->";
            change += to_string(new_class);
            record.asset_class = new_class;
            return ChangeText{ProvenanceAction::ClassReclassified, std::move(change)};
        });
}

Outcome<MutationResult> AssetRegistry::update_serial_identity(WriterSession& session,
                                                              const MutationEnvelope& envelope, const AssetId& id,
                                                              RevisionExpectation expected,
                                                              const SerialIdentity& identity) {
    if (identity.empty()) {
        return reject(ErrorCode::MalformedSerialIdentity,
                      "the new serial identity must carry a manufacturer and a serial number");
    }
    return run_record_mutation(
        *impl_, session, envelope, "update_serial_identity",
        join_fields({id.to_compact_string(), material_serial(identity)}), id, expected, std::nullopt,
        [&identity](Txn& txn, AssetRecord& record, const ActorRef& actor, const Timestamp& now,
                    AssetRevision revision) -> Outcome<ChangeText> {
            (void)actor;
            (void)now;
            (void)revision;
            if (record.serial_identity == identity) {
                return reject(ErrorCode::InvalidInput, "the record already carries that serial identity");
            }
            const std::optional<AssetId> claimant = txn.serial_claimant(identity);
            if (claimant.has_value() && !(*claimant == record.id)) {
                Error error(ErrorCode::DuplicateSerialIdentity,
                            "the serial identity is already claimed by another asset");
                error.with_subject(claimant->to_string());
                return error;
            }
            const std::string change =
                "serial_identity=" + record.serial_identity.to_string() + "->" + identity.to_string();
            record.serial_identity = identity;
            return ChangeText{ProvenanceAction::SerialIdentityUpdated, change};
        });
}

Outcome<MutationResult> AssetRegistry::set_placement(WriterSession& session, const MutationEnvelope& envelope,
                                                     const AssetId& id, RevisionExpectation expected,
                                                     const PlacementChange& change) {
    const std::string units = change.units.has_value() ? change.units->to_string() : std::string();
    return run_record_mutation(
        *impl_, session, envelope, "set_placement", join_fields({id.to_compact_string(), material_placement(change)}),
        id, expected, std::nullopt,
        [&change, &units](Txn& txn, AssetRecord& record, const ActorRef& actor, const Timestamp& now,
                          AssetRevision revision) -> Outcome<ChangeText> {
            (void)actor;
            (void)now;
            (void)revision;
            const auto has_kind = [&record](Reference::Kind kind) {
                return std::any_of(record.references.begin(), record.references.end(),
                                   [kind](const Reference& reference) { return reference.kind() == kind; });
            };
            const auto has_span = [&record]() {
                return std::any_of(record.references.begin(), record.references.end(),
                                   [](const Reference& reference) {
                                       return reference.kind() == Reference::Kind::ExternalObject &&
                                              reference.external_object().kind() == "rack-unit";
                                   });
            };
            if (change.units.has_value() && !change.rack.has_value() && !has_kind(Reference::Kind::Rack)) {
                return reject(ErrorCode::InvalidInput,
                              "a rack-unit span requires a rack reference on the record or in the same change");
            }
            if (change.clear_units && !change.clear_rack && !has_span()) {
                return reject(ErrorCode::AssetNotFound, "the record has no rack-unit span to clear");
            }
            if (change.clear_location && !has_kind(Reference::Kind::Location)) {
                return reject(ErrorCode::AssetNotFound, "the record has no location reference to clear");
            }
            if (change.clear_rack && !has_kind(Reference::Kind::Rack)) {
                return reject(ErrorCode::AssetNotFound, "the record has no rack reference to clear");
            }

            std::vector<Reference> updated;
            updated.reserve(record.references.size() + 4);
            std::string text;
            const auto note = [&text](std::string piece) {
                if (!text.empty()) {
                    text += ';';
                }
                text += std::move(piece);
            };
            for (const Reference& reference : record.references) {
                switch (reference.kind()) {
                    case Reference::Kind::Location:
                        if (change.clear_location) {
                            note("location=cleared");
                            continue;
                        }
                        if (change.location.has_value()) {
                            note("location=" + change.location->text());
                            continue;
                        }
                        break;
                    case Reference::Kind::Rack:
                        if (change.clear_rack) {
                            note("rack=cleared");
                            continue;
                        }
                        if (change.rack.has_value()) {
                            note("rack=" + change.rack->text());
                            continue;
                        }
                        break;
                    case Reference::Kind::ExternalObject:
                        if (reference.external_object().kind() == "rack-unit") {
                            if (change.clear_units || change.clear_rack) {
                                note("rack_units=cleared");
                                continue;
                            }
                            if (change.units.has_value()) {
                                note("rack_units=" + units);
                                continue;
                            }
                        }
                        break;
                    case Reference::Kind::Capability:
                        break;
                }
                updated.push_back(reference);
            }
            if (change.location.has_value()) {
                updated.push_back(Reference::location(*change.location, change.evidence));
            }
            if (change.rack.has_value()) {
                updated.push_back(Reference::rack(*change.rack, change.evidence));
            }
            if (change.units.has_value()) {
                const auto span = ExternalObjectReference::create("rack-unit", units);
                if (!span.has_value()) {
                    return reject(ErrorCode::MalformedSlotPosition, "the rack-unit span is not representable");
                }
                updated.push_back(Reference::external_object(*span, change.evidence));
            }
            if (const auto failure = canonicalise_references(updated, txn.bounds()); failure.has_value()) {
                return *failure;
            }
            if (updated == record.references) {
                return reject(ErrorCode::InvalidInput, "the change would not alter the record's placement");
            }
            record.references = std::move(updated);
            if (text.empty()) {
                text = "placement";
            }
            return ChangeText{ProvenanceAction::PlacementChanged, std::move(text)};
        });
}
namespace {

[[nodiscard]] bool has_reference_kind(const std::vector<Reference>& references, Reference::Kind kind) {
    return std::any_of(references.begin(), references.end(),
                       [kind](const Reference& reference) { return reference.kind() == kind; });
}

}  // namespace

Outcome<MutationResult> AssetRegistry::transition_lifecycle(WriterSession& session, const MutationEnvelope& envelope,
                                                            const AssetId& id, RevisionExpectation expected,
                                                            LifecycleState target) {
    return run_record_mutation(
        *impl_, session, envelope, "transition_lifecycle", join_fields({id.to_compact_string(), to_string(target)}), id,
        expected, std::nullopt,
        [target](Txn& txn, AssetRecord& record, const ActorRef& actor, const Timestamp& now,
                 AssetRevision revision) -> Outcome<ChangeText> {
            (void)actor;
            (void)now;
            (void)revision;
            const LifecycleState from = record.state.lifecycle;
            if (from == target) {
                Error error(ErrorCode::IllegalLifecycleTransition,
                            "the record is already in the requested lifecycle state; a no-op transition is "
                            "rejected so a caller cannot mistake it for an applied change");
                error.with_subject(record.id.to_string());
                return error;
            }
            if (!is_legal_lifecycle_transition(from, target)) {
                Error error(ErrorCode::IllegalLifecycleTransition, explain_lifecycle_rejection(from, target));
                error.with_subject(record.id.to_string());
                return error;
            }
            const auto consistency =
                check_state_consistency(target, record.state.installation, txn.policy().consistency);
            if (consistency.has_value()) {
                Error error(ErrorCode::LifecycleInvariantViolation,
                            "the transition would leave the record in an inconsistent state: " + *consistency);
                error.with_subject(record.id.to_string());
                return error;
            }
            record.state.lifecycle = target;
            return ChangeText{ProvenanceAction::LifecycleTransitioned, describe_transition(from, target)};
        });
}

Outcome<MutationResult> AssetRegistry::transition_installation(WriterSession& session,
                                                               const MutationEnvelope& envelope, const AssetId& id,
                                                               RevisionExpectation expected,
                                                               InstallationState target) {
    return run_record_mutation(
        *impl_, session, envelope, "transition_installation",
        join_fields({id.to_compact_string(), to_string(target)}), id, expected, std::nullopt,
        [target](Txn& txn, AssetRecord& record, const ActorRef& actor, const Timestamp& now,
                 AssetRevision revision) -> Outcome<ChangeText> {
            (void)actor;
            (void)now;
            (void)revision;
            const InstallationState from = record.state.installation;
            if (from == target) {
                Error error(ErrorCode::IllegalInstallationTransition,
                            "the record is already in the requested installation state");
                error.with_subject(record.id.to_string());
                return error;
            }
            if (!is_legal_installation_transition(from, target)) {
                Error error(ErrorCode::IllegalInstallationTransition, explain_installation_rejection(from, target));
                error.with_subject(record.id.to_string());
                return error;
            }
            const auto consistency =
                check_state_consistency(record.state.lifecycle, target, txn.policy().consistency);
            if (consistency.has_value()) {
                Error error(ErrorCode::LifecycleInvariantViolation,
                            "the transition would leave the record in an inconsistent state: " + *consistency);
                error.with_subject(record.id.to_string());
                return error;
            }
            record.state.installation = target;
            return ChangeText{ProvenanceAction::InstallationTransitioned, describe_transition(from, target)};
        });
}

Outcome<MutationResult> AssetRegistry::attach_reference(WriterSession& session, const MutationEnvelope& envelope,
                                                        const AssetId& id, RevisionExpectation expected,
                                                        const Reference& reference) {
    return run_record_mutation(
        *impl_, session, envelope, "attach_reference",
        join_fields({id.to_compact_string(), reference.canonical(), to_string(reference.evidence())}), id, expected,
        std::nullopt,
        [&reference](Txn& txn, AssetRecord& record, const ActorRef& actor, const Timestamp& now,
                     AssetRevision revision) -> Outcome<ChangeText> {
            (void)actor;
            (void)now;
            (void)revision;
            // A reference whose kind-specific target is empty names nothing. Its canonical
            // key is not empty — it still spells the kind, as in "capability:" — so the
            // target itself is what has to be checked.
            if (!reference_has_target(reference)) {
                return reject(ErrorCode::InvalidInput, "the reference names no target");
            }
            if (std::find(record.references.begin(), record.references.end(), reference) != record.references.end()) {
                Error error(ErrorCode::ReferenceAlreadyAttached,
                            "the reference is already attached to this asset: " + reference.canonical());
                error.with_subject(record.id.to_string());
                return error;
            }
            // At most one reference per kind, so "the location of this asset" stays
            // unambiguous. Capability references are exempt because an asset uses as many
            // capabilities as it has workloads, and external object references are exempt
            // per kind, because a record legitimately carries several references of
            // different kinds.
            for (const Reference& existing : record.references) {
                if (existing.kind() != reference.kind() || existing.canonical() == reference.canonical()) {
                    continue;
                }
                if (reference.kind() == Reference::Kind::Capability ||
                    (reference.kind() == Reference::Kind::ExternalObject &&
                     existing.external_object().kind() != reference.external_object().kind())) {
                    continue;
                }
                Error error(ErrorCode::AliasConflict,
                            "the record already carries a reference of this kind: " + existing.canonical());
                error.with_subject(record.id.to_string());
                return error;
            }
            std::vector<Reference> updated = record.references;
            updated.push_back(reference);
            if (const auto failure = canonicalise_references(updated, txn.bounds()); failure.has_value()) {
                return *failure;
            }
            record.references = std::move(updated);
            std::string text = "attached ";
            text += reference.canonical();
            text += " evidence=";
            text += to_string(reference.evidence());
            return ChangeText{ProvenanceAction::ReferenceAttached, std::move(text)};
        });
}

Outcome<MutationResult> AssetRegistry::detach_reference(WriterSession& session, const MutationEnvelope& envelope,
                                                        const AssetId& id, RevisionExpectation expected,
                                                        const Reference& reference) {
    return run_record_mutation(
        *impl_, session, envelope, "detach_reference", join_fields({id.to_compact_string(), reference.canonical()}), id,
        expected, std::nullopt,
        [&reference](Txn& txn, AssetRecord& record, const ActorRef& actor, const Timestamp& now,
                     AssetRevision revision) -> Outcome<ChangeText> {
            (void)txn;
            (void)actor;
            (void)now;
            (void)revision;
            const auto position = std::find(record.references.begin(), record.references.end(), reference);
            if (position == record.references.end()) {
                Error error(ErrorCode::ReferenceNotAttached,
                            "the reference is not attached to this asset: " + reference.canonical());
                error.with_subject(record.id.to_string());
                return error;
            }
            record.references.erase(position);
            return ChangeText{ProvenanceAction::ReferenceDetached, "detached " + reference.canonical()};
        });
}

Outcome<MutationResult> AssetRegistry::set_reference_evidence(WriterSession& session,
                                                              const MutationEnvelope& envelope, const AssetId& id,
                                                              RevisionExpectation expected, const Reference& reference,
                                                              ReferenceEvidence evidence) {
    return run_record_mutation(
        *impl_, session, envelope, "set_reference_evidence",
        join_fields({id.to_compact_string(), reference.canonical(), to_string(evidence)}), id, expected,
        std::nullopt,
        [&reference, evidence](Txn& txn, AssetRecord& record, const ActorRef& actor, const Timestamp& now,
                               AssetRevision revision) -> Outcome<ChangeText> {
            (void)txn;
            (void)actor;
            (void)now;
            (void)revision;
            const auto position = std::find_if(
                record.references.begin(), record.references.end(),
                [&reference](const Reference& existing) { return existing.canonical() == reference.canonical(); });
            if (position == record.references.end()) {
                Error error(ErrorCode::ReferenceNotAttached,
                            "the reference is not attached to this asset: " + reference.canonical());
                error.with_subject(record.id.to_string());
                return error;
            }
            if (position->evidence() == evidence) {
                return reject(ErrorCode::InvalidInput, "the reference already carries that evidence level");
            }
            Reference replacement;
            switch (position->kind()) {
                case Reference::Kind::Capability:
                    replacement = Reference::capability(position->capability(), evidence);
                    break;
                case Reference::Kind::Location:
                    replacement = Reference::location(position->location(), evidence);
                    break;
                case Reference::Kind::Rack:
                    replacement = Reference::rack(position->rack(), evidence);
                    break;
                case Reference::Kind::ExternalObject:
                    replacement = Reference::external_object(position->external_object(), evidence);
                    break;
            }
            const auto index = static_cast<std::size_t>(position - record.references.begin());
            record.references[index] = replacement;
            std::string text = "reference ";
            text += reference.canonical();
            text += " evidence=";
            text += to_string(evidence);
            return ChangeText{ProvenanceAction::ReferenceEvidenceChanged, std::move(text)};
        });
}
Outcome<MutationResult> AssetRegistry::link_replacement(WriterSession& session, const MutationEnvelope& envelope,
                                                        const AssetId& successor, RevisionExpectation expected,
                                                        const AssetId& predecessor, ReplacementCause cause,
                                                        std::string_view note) {
    if (successor == predecessor) {
        return reject(ErrorCode::SelfReplacementForbidden, "an asset cannot supersede itself");
    }
    if (note.size() > limits::kMaxReplacementReasonBytes || validate_utf8(note, true) != Utf8Status::Valid) {
        return reject(ErrorCode::MalformedText,
                      "the replacement note is too long or is not structurally valid UTF-8");
    }
    const std::string note_text(note);
    return run_record_mutation(
        *impl_, session, envelope, "link_replacement",
        join_fields({successor.to_compact_string(), predecessor.to_compact_string(), to_string(cause), note_text}),
        successor, expected, std::nullopt,
        [&predecessor, cause, &note_text](Txn& txn, AssetRecord& record, const ActorRef& actor, const Timestamp& now,
                                          AssetRevision revision) -> Outcome<ChangeText> {
            (void)actor;
            (void)now;
            (void)revision;
            if (record.supersedes.has_value()) {
                Error error(ErrorCode::AliasConflict,
                            "the record already declares a predecessor; an asset supersedes exactly one asset");
                error.with_subject(record.id.to_string());
                return error;
            }
            const auto target = txn.load(predecessor);
            if (!target.has_value()) {
                Error error(ErrorCode::AssetNotFound, "the predecessor is not present in this registry");
                error.with_subject(predecessor.to_string());
                return error;
            }
            if (txn.policy().require_terminal_before_supersede && !is_terminal_lifecycle(target->state.lifecycle)) {
                Error error(ErrorCode::LifecycleInvariantViolation,
                            "the predecessor is not in a terminal lifecycle state; decommission it before "
                            "recording a replacement");
                error.with_subject(predecessor.to_string());
                return error;
            }
            // Walk the predecessor chain to reject a link that would close a cycle.
            // The walk is bounded, and reaching the bound is reported rather than
            // silently truncating the check.
            std::unordered_set<std::string> visited;
            visited.insert(record.id.to_compact_string());
            AssetId cursor = predecessor;
            for (std::size_t hop = 0;; ++hop) {
                if (!visited.insert(cursor.to_compact_string()).second) {
                    Error error(ErrorCode::ReplacementCycleDetected,
                                "the link would close a cycle in replacement lineage");
                    error.with_subject(record.id.to_string());
                    return error;
                }
                const auto current = txn.load(cursor);
                if (!current.has_value() || !current->supersedes.has_value()) {
                    break;
                }
                if (hop >= limits::kMaxLineageDepth) {
                    Error error(ErrorCode::LineageTraversalLimitExceeded,
                                "the predecessor chain is longer than the traversal bound, so the link cannot be "
                                "proved acyclic");
                    error.with_subject(record.id.to_string());
                    return error;
                }
                cursor = current->supersedes->predecessor;
            }
            ReplacementLink link;
            link.predecessor = target->id;
            link.predecessor_generation = target->generation;
            link.predecessor_final_revision = target->revision;
            link.cause = cause;
            link.linked_at = txn.next_sequence();
            link.note = note_text;
            record.supersedes = std::move(link);
            std::string text = "supersedes=";
            text += predecessor.to_string();
            text += " cause=";
            text += to_string(cause);
            return ChangeText{ProvenanceAction::ReplacementLinked, std::move(text)};
        });
}

Outcome<MutationResult> AssetRegistry::reuse_identity(WriterSession& session, const MutationEnvelope& envelope,
                                                      const AssetId& id, RevisionExpectation expected,
                                                      const SerialIdentity& new_identity, AssetClass new_class) {
    if (!is_known(new_class)) {
        return reject(ErrorCode::UnknownAssetClass, "the new incarnation must carry a known physical class");
    }
    if (new_identity.empty()) {
        return reject(ErrorCode::MalformedSerialIdentity,
                      "the new incarnation must carry a manufacturer and serial identity");
    }
    return run_record_mutation(
        *impl_, session, envelope, "reuse_identity",
        join_fields({id.to_compact_string(), material_serial(new_identity), to_string(new_class)}), id, expected,
        std::nullopt,
        [&new_identity, new_class](Txn& txn, AssetRecord& record, const ActorRef& actor, const Timestamp& now,
                                   AssetRevision revision) -> Outcome<ChangeText> {
            (void)actor;
            (void)revision;
            if (txn.policy().identity_reuse != IdentityReusePolicy::AllowAfterTerminal) {
                Error error(ErrorCode::IdentityReuseForbidden,
                            "the configured policy forbids reusing a canonical identity; each physical object keeps "
                            "its own identity forever");
                error.with_subject(record.id.to_string());
                return error;
            }
            if (!is_terminal_lifecycle(record.state.lifecycle)) {
                Error error(ErrorCode::IdentityReuseForbidden,
                            "the identity can only be reused once the current incarnation reaches a terminal "
                            "lifecycle state");
                error.with_subject(record.id.to_string());
                return error;
            }
            if (record.history.size() >= txn.bounds().max_generations_per_asset) {
                Error error(ErrorCode::CapacityExceeded,
                            "the record has reached the configured bound on retained identity generations");
                error.with_subject(record.id.to_string());
                return error;
            }
            const std::optional<AssetId> claimant = txn.serial_claimant(new_identity);
            if (claimant.has_value() && !(*claimant == record.id)) {
                Error error(ErrorCode::DuplicateSerialIdentity, "the new serial identity is claimed by another asset");
                error.with_subject(claimant->to_string());
                return error;
            }
            const auto next_generation = record.generation.next();
            if (!next_generation.has_value()) {
                return reject(ErrorCode::CapacityExceeded, "the identity generation counter is exhausted");
            }

            AssetIncarnation closed;
            closed.generation = record.generation;
            closed.asset_class = record.asset_class;
            closed.serial_identity = record.serial_identity;
            closed.metadata = record.metadata;
            closed.references = record.references;
            closed.state = record.state;
            closed.final_revision = record.revision;
            closed.closed_at = txn.next_sequence();
            closed.closed_time = now;

            std::string text = "generation=";
            text += record.generation.to_string();
            text += "->";
            text += next_generation->to_string();
            text += ";previous_serial=";
            text += record.serial_identity.to_string();

            record.history.push_back(std::move(closed));
            record.generation = *next_generation;
            record.asset_class = new_class;
            record.serial_identity = new_identity;
            record.metadata = AssetMetadata{};
            record.metadata.display_name = new_identity.to_string();
            record.references.clear();
            record.state = AssetState{};
            record.supersedes.reset();
            return ChangeText{ProvenanceAction::IdentityReused, std::move(text)};
        });
}
Outcome<ImportReport> AssetRegistry::import_assets(WriterSession& session, const MutationEnvelope& envelope,
                                                   const std::vector<ImportedAsset>& records,
                                                   const ImportOptions& options) {
    ImportReport report;
    report.records_seen = static_cast<std::uint64_t>(records.size());
    if (records.size() > impl_->bounds.max_import_records) {
        return reject(ErrorCode::CapacityExceeded,
                      "the batch carries more records than the configured import bound");
    }
    if (options.source.size() > limits::kMaxReasonBytes || validate_utf8(options.source, true) != Utf8Status::Valid) {
        return reject(ErrorCode::MalformedText,
                      "the import source attribution is too long or is not structurally valid UTF-8");
    }

    std::string material;
    append_field(material, options.source);
    append_field(material, to_string(options.conflict));
    append_field(material, to_string(options.on_error));
    append_field(material, options.drop_unverified_references ? "drop" : "keep");
    for (const ImportedAsset& record : records) {
        append_field(material, record.id.to_string());
        append_field(material, material_serial(record.serial_identity));
        append_field(material, material_metadata(record.metadata));
        append_field(material, material_references(record.references));
        append_field(material, to_string(record.asset_class));
        append_field(material, to_string(record.state.lifecycle));
        append_field(material, to_string(record.state.installation));
        append_field(material, record.supersedes.has_value() ? record.supersedes->to_string() : std::string());
        append_field(material, to_string(record.supersession_cause));
    }

    Txn txn(*impl_, session, envelope, "import_assets", material);
    if (!txn.begin()) {
        txn.record_rejection(txn.failure());
        return txn.failure();
    }
    if (txn.replayed()) {
        const internal::StoredIdempotency& recorded = txn.replay();
        if (recorded.outcome_kind == internal::StoredOutcomeKind::Error) {
            return error_from(recorded);
        }
        report.committed = true;
        report.committed_sequence = TransactionSequence(recorded.revision);
        return report;
    }

    const ActorRef actor = ActorRef::writer(txn.writer());
    const Timestamp now = SystemClock{}.now();
    Touched touched;
    std::vector<std::string> batch_serial_claims;
    // Identities this batch has already handled. One batch applies each record at most
    // once: a second occurrence of the same identity would either mutate a record the batch
    // has already mutated — two provenance steps carrying one transaction sequence, which
    // the record predicate rejects — or silently overwrite what the first occurrence
    // decided. A batch is a set of records, and a repeat is reported rather than merged.
    std::vector<AssetId> batch_ids;

    for (std::size_t index = 0; index < records.size(); ++index) {
        const ImportedAsset& imported = records[index];
        const auto reject_record = [&](ErrorCode code, std::string message) -> std::optional<Error> {
            ImportRejection rejection;
            rejection.record_index = index;
            rejection.claimed_id = imported.id.is_nil() ? std::string() : imported.id.to_string();
            rejection.code = code;
            rejection.message = message;
            report.rejections.push_back(std::move(rejection));
            if (options.on_error == ImportErrorPolicy::RejectBatch) {
                // A wholesale rejection is one failure of the batch, so it carries the
                // batch-level code and says which record stopped it. The per-record code is
                // still recorded in the rejection, which is where a caller looks when the
                // policy is RejectRecord and the batch continues.
                return reject(ErrorCode::ImportRecordInvalid,
                              "the import batch was rejected at record " + std::to_string(index) + ": " + message);
            }
            return std::nullopt;
        };

        if (imported.id.is_nil()) {
            if (const auto failure = reject_record(ErrorCode::MalformedAssetId,
                                                   "the record carries no canonical identity");
                failure.has_value()) {
                return *failure;
            }
            continue;
        }
        if (std::find(batch_ids.begin(), batch_ids.end(), imported.id) != batch_ids.end()) {
            if (const auto failure = reject_record(ErrorCode::DuplicateAssetId,
                                                   "the batch imports the same asset identity twice");
                failure.has_value()) {
                return *failure;
            }
            continue;
        }
        batch_ids.push_back(imported.id);
        if (!is_known(imported.asset_class)) {
            if (const auto failure = reject_record(ErrorCode::UnknownAssetClass,
                                                   "the record carries an unrecognised asset class");
                failure.has_value()) {
                return *failure;
            }
            continue;
        }
        if (imported.serial_identity.empty()) {
            if (const auto failure = reject_record(ErrorCode::MalformedSerialIdentity,
                                                   "the record carries no manufacturer and serial identity");
                failure.has_value()) {
                return *failure;
            }
            continue;
        }
        if (!is_display_text(imported.metadata.display_name, impl_->bounds.max_display_name_bytes)) {
            if (const auto failure =
                    reject_record(ErrorCode::MalformedText, "the record carries an unusable display name");
                failure.has_value()) {
                return *failure;
            }
            continue;
        }
        if (const auto consistency = check_state_consistency(imported.state.lifecycle, imported.state.installation,
                                                             impl_->policy.consistency);
            consistency.has_value()) {
            if (const auto failure = reject_record(ErrorCode::LifecycleInvariantViolation,
                                                   "the record state violates the consistency rules: " + *consistency);
                failure.has_value()) {
                return *failure;
            }
            continue;
        }
        // A record may only supersede an asset that already exists. Rejecting the
        // rest keeps lineage acyclic by construction: a link can never point
        // forward at a record that is still being imported.
        if (imported.supersedes.has_value()) {
            if (*imported.supersedes == imported.id) {
                if (const auto failure = reject_record(ErrorCode::SelfReplacementForbidden,
                                                       "the record names itself as its own predecessor");
                    failure.has_value()) {
                    return *failure;
                }
                continue;
            }
            if (!txn.state().find(*imported.supersedes).found) {
                if (const auto failure =
                        reject_record(ErrorCode::AssetNotFound,
                                      "the record names a predecessor that is not present in this registry");
                    failure.has_value()) {
                    return *failure;
                }
                continue;
            }
        }

        std::vector<Reference> references = imported.references;
        std::uint64_t dropped = 0;
        if (options.drop_unverified_references) {
            const auto removed = std::remove_if(references.begin(), references.end(), [](const Reference& reference) {
                return reference.evidence() == ReferenceEvidence::Unverified;
            });
            dropped = static_cast<std::uint64_t>(std::distance(removed, references.end()));
            references.erase(removed, references.end());
        }
        if (const auto failure = canonicalise_references(references, impl_->bounds); failure.has_value()) {
            if (const auto rejected = reject_record(failure->code(), failure->message()); rejected.has_value()) {
                return *rejected;
            }
            continue;
        }
        std::vector<std::pair<std::string, std::string>> labels = imported.metadata.labels;
        if (const auto failure = canonicalise_labels(labels, impl_->bounds); failure.has_value()) {
            if (const auto rejected = reject_record(failure->code(), failure->message()); rejected.has_value()) {
                return *rejected;
            }
            continue;
        }

        const std::optional<AssetId> claimant = txn.serial_claimant(imported.serial_identity);
        const bool serial_conflict = claimant.has_value() && !(*claimant == imported.id);
        const auto existing = txn.load(imported.id);

        if (existing.has_value()) {
            if (options.conflict == ImportConflictPolicy::Reject) {
                if (const auto failure = reject_record(ErrorCode::DuplicateAssetId,
                                                       "the identity is already present in this registry");
                    failure.has_value()) {
                    return *failure;
                }
                continue;
            }
            if (options.conflict == ImportConflictPolicy::SkipExisting) {
                ++report.records_skipped;
                continue;
            }
            if (serial_conflict) {
                if (const auto failure =
                        reject_record(ErrorCode::DuplicateSerialIdentity,
                                      "the record's serial identity is claimed by a different asset");
                    failure.has_value()) {
                    return *failure;
                }
                continue;
            }
            AssetMetadata metadata = imported.metadata;
            metadata.labels = std::move(labels);
            if (existing->metadata == metadata && existing->references == references &&
                existing->state == imported.state && existing->asset_class == imported.asset_class &&
                existing->serial_identity == imported.serial_identity) {
                ++report.records_skipped;
                continue;
            }
            const auto revision = existing->revision.next();
            if (!revision.has_value()) {
                if (const auto failure =
                        reject_record(ErrorCode::CapacityExceeded, "the record revision counter is exhausted");
                    failure.has_value()) {
                    return *failure;
                }
                continue;
            }
            auto updated = std::make_shared<AssetRecord>(*existing);
            updated->asset_class = imported.asset_class;
            updated->serial_identity = imported.serial_identity;
            updated->metadata = std::move(metadata);
            updated->references = std::move(references);
            updated->state = imported.state;
            push_provenance(*updated, impl_->bounds,
                            make_step(txn.next_sequence(), txn.epoch(), *revision,
                                      ProvenanceAction::MetadataUpdated, ProvenanceOrigin::Import, actor,
                                      imported.observed_at.has_value() ? *imported.observed_at : now,
                                      options.source.empty() ? std::string("imported") : options.source,
                                      std::string("import=update")));
            txn.store(std::move(updated));
            touched.add(imported.id);
            ++report.records_updated;
            report.references_dropped += dropped;
            continue;
        }

        if (serial_conflict) {
            if (const auto failure =
                    reject_record(ErrorCode::DuplicateSerialIdentity,
                                  "the serial identity is already claimed by another asset");
                failure.has_value()) {
                return *failure;
            }
            continue;
        }
        if (txn.state().records.size() >= impl_->bounds.max_assets) {
            if (const auto failure =
                    reject_record(ErrorCode::CapacityExceeded, "the registry is at its configured asset bound");
                failure.has_value()) {
                return *failure;
            }
            continue;
        }
        if (std::find(batch_serial_claims.begin(), batch_serial_claims.end(),
                      imported.serial_identity.canonical_key()) != batch_serial_claims.end()) {
            if (const auto failure = reject_record(ErrorCode::DuplicateSerialIdentity,
                                                   "the batch imports the same serial identity twice");
                failure.has_value()) {
                return *failure;
            }
            continue;
        }

        auto record = std::make_shared<AssetRecord>();
        record->id = imported.id;
        record->asset_class = imported.asset_class;
        record->generation = AssetGeneration(1);
        record->revision = AssetRevision(1);
        record->last_sequence = txn.next_sequence();
        record->serial_identity = imported.serial_identity;
        record->metadata = imported.metadata;
        record->metadata.labels = std::move(labels);
        record->references = std::move(references);
        record->state = imported.state;
        if (imported.supersedes.has_value()) {
            const auto target = txn.load(*imported.supersedes);
            if (target.has_value()) {
                ReplacementLink link;
                link.predecessor = target->id;
                link.predecessor_generation = target->generation;
                link.predecessor_final_revision = target->revision;
                link.cause = imported.supersession_cause;
                link.linked_at = txn.next_sequence();
                record->supersedes = std::move(link);
            }
        }
        record->provenance.push_back(make_step(txn.next_sequence(), txn.epoch(), AssetRevision(1),
                                               ProvenanceAction::Registered, ProvenanceOrigin::Import, actor,
                                               imported.observed_at.has_value() ? *imported.observed_at : now,
                                               options.source.empty() ? std::string("imported") : options.source,
                                               std::string("origin=import")));
        if (const auto validation = validate_asset_record(*record, impl_->bounds, impl_->policy.consistency);
            validation.has_value()) {
            if (const auto failure = reject_record(ErrorCode::ImportRecordInvalid,
                                                   "the record would be invalid: " + *validation);
                failure.has_value()) {
                return *failure;
            }
            continue;
        }
        batch_serial_claims.push_back(record->serial_identity.canonical_key());
        const AssetId inserted_id = record->id;
        txn.insert(std::move(record));
        touched.add(inserted_id);
        ++report.records_registered;
        report.references_dropped += dropped;
    }

    if (touched.ids().empty()) {
        // Nothing was accepted. The batch is not a mutation, so no transaction
        // identity is consumed and the caller sees exactly which records failed.
        report.committed = false;
        if (options.on_error == ImportErrorPolicy::RejectBatch && !report.rejections.empty()) {
            return reject(ErrorCode::ImportRecordInvalid, "the import batch was rejected in full");
        }
        return report;
    }

    const auto published = txn.commit(AssetRevision(1), AssetId{}, touched.ids());
    if (!published) {
        return published.error();
    }
    report.committed = true;
    report.committed_sequence = published.value();
    return report;
}

Outcome<std::string> AssetRegistry::export_document(const ExportOptions& options, ExportReport& report) const {
    return export_snapshot(snapshot(), options, report);
}

Outcome<std::vector<ImportedAsset>> AssetRegistry::parse_import_document(std::string_view document,
                                                                        const ImportOptions& options) {
    (void)options;
    return parse_import_json(document, impl_->bounds);
}
// ---------------------------------------------------------------------------
// Construction and status
// ---------------------------------------------------------------------------

AssetRegistry::AssetRegistry() : impl_(std::make_unique<internal::RegistryImpl>()) {}

AssetRegistry::~AssetRegistry() {
    if (impl_ != nullptr) {
        (void)close();
    }
}

Outcome<std::shared_ptr<AssetRegistry>> AssetRegistry::create_detached(const RegistryPolicy& policy,
                                                                      const RegistryLimits& bounds) {
    if (const auto failure = validate_limits(bounds); failure.has_value()) {
        return reject(ErrorCode::InvalidLimits, "registry bounds are not usable: " + *failure);
    }
    auto registry = std::shared_ptr<AssetRegistry>(new AssetRegistry());
    registry->impl_->policy = policy;
    registry->impl_->bounds = bounds;
    registry->impl_->authority_mode = AuthorityMode::LocalAuthority;
    registry->impl_->epoch = RegistryEpoch(1);
    install_published(*registry->impl_,
                      detail::make_body(policy, bounds, RegistryEpoch(1), TransactionSequence(0), IndexedState{}));
    return registry;
}

Outcome<std::shared_ptr<AssetRegistry>> AssetRegistry::open(const std::filesystem::path& directory,
                                                            StoreOpenMode mode, const StoreOpenOptions& options,
                                                            RecoveryReport& report) {
    auto opened = Store::open(directory, mode, options, report);
    if (!opened) {
        return opened.error();
    }
    auto registry = std::shared_ptr<AssetRegistry>(new AssetRegistry());
    registry->impl_->store = opened.value();
    registry->impl_->policy = options.policy;
    registry->impl_->bounds = opened.value()->limits();
    registry->impl_->authority_mode = AuthorityMode::RegistryAuthority;

    const StoreLoadedState& loaded = StoreAccess::loaded_state(*opened.value());
    IndexedState state;
    state.records = loaded.records;
    state.ids.reserve(loaded.records.size());
    for (const RecordPtr& record : loaded.records) {
        state.ids.push_back(record->id);
    }
    state.writers = loaded.writers;
    install_published(*registry->impl_,
                      detail::make_body(options.policy, registry->impl_->bounds,
                                        StoreAccess::current_epoch(*opened.value()), loaded.sequence,
                                        std::move(state)));
    return registry;
}

AuthorityMode AssetRegistry::authority_mode() const noexcept {
    return impl_->authority_mode;
}

bool AssetRegistry::is_open() const noexcept {
    return !impl_->closed.load();
}

RegistryEpoch AssetRegistry::epoch() const noexcept {
    if (!impl_->detached()) {
        return StoreAccess::current_epoch(*impl_->store);
    }
    return impl_->epoch;
}

TransactionSequence AssetRegistry::published_sequence() const noexcept {
    const std::shared_ptr<const RegistryBody> body = read_published(*impl_);
    return body == nullptr ? TransactionSequence(0) : body->sequence;
}

const RegistryPolicy& AssetRegistry::policy() const noexcept {
    return impl_->policy;
}

const RegistryLimits& AssetRegistry::limits() const noexcept {
    return impl_->bounds;
}

std::shared_ptr<Store> AssetRegistry::store() const noexcept {
    return impl_->store;
}

Snapshot AssetRegistry::snapshot() const {
    return detail::snapshot_from_body(read_published(*impl_));
}

Outcome<void> AssetRegistry::flush() {
    if (impl_->closed.load()) {
        return reject(ErrorCode::RegistryClosed, "the registry is closed");
    }
    // Every accepted mutation is durable before it returns, so there is nothing to
    // flush. The call still reports whether the backing store would accept a
    // commit, which is what a readiness probe asks.
    if (impl_->detached()) {
        return Outcome<void>{};
    }
    if (!impl_->store->is_open()) {
        return reject(ErrorCode::RegistryClosed, "the backing store is closed");
    }
    return Outcome<void>{};
}

Outcome<void> AssetRegistry::close() {
    if (impl_ == nullptr) {
        return Outcome<void>{};
    }
    {
        const std::lock_guard<std::mutex> guard(impl_->commit_mutex);
        if (impl_->closed.exchange(true)) {
            return Outcome<void>{};
        }
        impl_->grants.clear();
    }
    if (impl_->store != nullptr) {
        return impl_->store->close();
    }
    return Outcome<void>{};
}

Outcome<WriterSession> AssetRegistry::open_writer(const WriterId& writer, const AuthorityOptions& options) {
    if (impl_->closed.load()) {
        return reject(ErrorCode::RegistryClosed, "cannot open a writer on a closed registry");
    }
    if (writer.empty()) {
        return reject(ErrorCode::MalformedWriterId, "a writer identity is required; WriterId::generate mints one");
    }
    WriterSession session = WriterSession::create(writer);
    if (impl_->detached()) {
        const std::lock_guard<std::mutex> guard(impl_->commit_mutex);
        if (options.reason.size() > limits::kMaxReasonBytes ||
            validate_utf8(options.reason, true) != Utf8Status::Valid) {
            return reject(ErrorCode::MalformedText, "authority grant reason violates text rules");
        }
        AuthorityToken token;
        token.writer_ = session.writer();
        token.generation_ = impl_->next_grant;
        token.epoch_ = impl_->epoch;
        token.reason_ = options.reason;
        if (options.ttl_nanos.has_value()) {
            if (*options.ttl_nanos <= 0) {
                return reject(ErrorCode::InvalidInput, "authority lifetime must be a positive duration");
            }
            const Timestamp now = SystemClock{}.now();
            const auto expiry = Timestamp::create(now.unix_nanos() + *options.ttl_nanos);
            if (!expiry.has_value()) {
                return reject(ErrorCode::InvalidInput,
                              "authority lifetime extends beyond the representable timestamp range");
            }
            token.expires_at_ = *expiry;
        }
        const auto next = impl_->next_grant.next();
        if (!next.has_value()) {
            return reject(ErrorCode::CapacityExceeded, "the authority grant counter is exhausted");
        }
        impl_->grants[grant_key(token.writer_, token.generation_)] = internal::RegistryImpl::Grant{token.epoch_, false};
        impl_->next_grant = *next;
        session.attach_token(std::move(token));
        return session;
    }

    auto token = impl_->store->grant(session.writer(), options);
    if (!token) {
        return token.error();
    }
    // A writer that has mutated this store before continues from its recorded
    // high-water mark, so a request reissued after a restart is recognised as a
    // retry rather than applied a second time.
    const auto high_water = impl_->store->writer_high_water(session.writer());
    if (high_water.has_value()) {
        const auto next = high_water->next();
        if (next.has_value()) {
            session = WriterSession::resume(session.writer(), *next);
        }
    }
    session.attach_token(std::move(token).value());
    return session;
}

Outcome<void> AssetRegistry::close_writer(WriterSession& session) {
    if (impl_->detached()) {
        const std::lock_guard<std::mutex> guard(impl_->commit_mutex);
        const auto found = impl_->grants.find(grant_key(session.writer(), session.token().generation()));
        if (found != impl_->grants.end()) {
            found->second.revoked = true;
            impl_->grants.erase(found);
        }
        return Outcome<void>{};
    }
    return impl_->store->revoke(session.token());
}

}  // namespace asset_registry
