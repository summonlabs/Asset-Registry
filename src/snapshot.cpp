// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_registry/snapshot.hpp"

#include <algorithm>
#include <map>
#include <unordered_set>

#include "registry_state.hpp"
#include "snapshot_internal.hpp"

namespace asset_registry {

namespace detail {

void visit_snapshot_body(const RegistryBody& body, const std::function<bool(const AssetRecord&)>& visitor) {
    for (const RecordPtr& record : body.state.records) {
        if (record != nullptr && !visitor(*record)) {
            return;
        }
    }
}

std::shared_ptr<const RegistryBody> make_body(const RegistryPolicy& policy, const RegistryLimits& bounds,
                                              RegistryEpoch epoch, TransactionSequence sequence,
                                              IndexedState state) {
    auto body = std::make_shared<RegistryBody>();
    body->policy = policy;
    body->bounds = bounds;
    body->epoch = epoch;
    body->sequence = sequence;
    body->state = std::move(state);
    body->serial_index.reserve(body->state.records.size());
    for (std::size_t index = 0; index < body->state.records.size(); ++index) {
        const AssetRecord& record = *body->state.records[index];
        body->serial_index.emplace(record.serial_identity.canonical_key(), index);
    }
    for (const RecordPtr& record : body->state.records) {
        if (record == nullptr || !record->supersedes.has_value()) {
            continue;
        }
        if (body->state.find(record->supersedes->predecessor).found) {
            ++body->successor_count[record->supersedes->predecessor.to_compact_string()];
        }
    }
    return body;
}

Snapshot snapshot_from_body(std::shared_ptr<const RegistryBody> body) {
    return Snapshot::from_body(std::move(body));
}

}  // namespace detail

Snapshot Snapshot::from_body(std::shared_ptr<const RegistryBody> body) {
    Snapshot snapshot;
    snapshot.body_ = std::move(body);
    return snapshot;
}

Snapshot Snapshot::empty(const RegistryPolicy& policy, const RegistryLimits& bounds) {
    return detail::snapshot_from_body(
        detail::make_body(policy, bounds, RegistryEpoch(0), TransactionSequence(0), IndexedState{}));
}

std::uint64_t Snapshot::size() const noexcept {
    return body_ == nullptr ? 0 : static_cast<std::uint64_t>(body_->state.records.size());
}

RegistryEpoch Snapshot::epoch() const noexcept {
    return body_ == nullptr ? RegistryEpoch(0) : body_->epoch;
}

TransactionSequence Snapshot::published_sequence() const noexcept {
    return body_ == nullptr ? TransactionSequence(0) : body_->sequence;
}

const RegistryPolicy& Snapshot::policy() const noexcept {
    static const RegistryPolicy kDefault = default_policy();
    return body_ == nullptr ? kDefault : body_->policy;
}

const RegistryLimits& Snapshot::bounds() const noexcept {
    static const RegistryLimits kDefault = default_limits();
    return body_ == nullptr ? kDefault : body_->bounds;
}

namespace {

[[nodiscard]] AssetView make_view(const IndexedState& state, std::size_t index) {
    const AssetRecord& record = *state.records[index];
    AssetView view;
    view.id = record.id;
    view.asset_class = record.asset_class;
    view.generation = record.generation;
    view.revision = record.revision;
    view.last_sequence = record.last_sequence;
    view.serial_identity = record.serial_identity;
    view.metadata = record.metadata;
    view.references = record.references;
    view.state = record.state;
    view.supersedes = record.supersedes;
    view.provenance = &record.provenance;
    view.history = &record.history;
    return view;
}

}  // namespace

std::optional<AssetView> Snapshot::find(const AssetId& id) const {
    if (body_ == nullptr) {
        return std::nullopt;
    }
    const RecordPlacement placement = body_->state.find(id);
    if (!placement.found) {
        return std::nullopt;
    }
    return make_view(body_->state, placement.index);
}

bool Snapshot::contains(const AssetId& id) const noexcept {
    return body_ != nullptr && body_->state.find(id).found;
}

std::optional<AssetId> Snapshot::find_by_serial(const SerialIdentity& identity) const {
    if (body_ == nullptr || identity.empty()) {
        return std::nullopt;
    }
    const auto found = body_->serial_index.find(identity.canonical_key());
    if (found == body_->serial_index.end() || found->second >= body_->state.ids.size()) {
        return std::nullopt;
    }
    return body_->state.ids[found->second];
}

std::vector<AssetId> Snapshot::ids() const {
    if (body_ == nullptr) {
        return {};
    }
    return body_->state.ids;
}

std::vector<AssetView> Snapshot::records() const {
    std::vector<AssetView> result;
    if (body_ == nullptr) {
        return result;
    }
    result.reserve(body_->state.records.size());
    for (std::size_t index = 0; index < body_->state.records.size(); ++index) {
        result.push_back(make_view(body_->state, index));
    }
    return result;
}

Outcome<QueryPage> Snapshot::query(const QueryFilter& filter, std::uint64_t offset, std::uint64_t limit) const {
    QueryPage page;
    page.offset = offset;
    if (body_ == nullptr) {
        return page;
    }
    for (const RecordPtr& record : body_->state.records) {
        if (record == nullptr || !record_matches(*record, filter)) {
            continue;
        }
        if (page.total_matches >= offset) {
            if (page.ids.size() < limit) {
                page.ids.push_back(record->id);
            } else {
                page.truncated = true;
            }
        }
        ++page.total_matches;
    }
    return page;
}

std::vector<AssetId> Snapshot::matching(const QueryFilter& filter) const {
    std::vector<AssetId> result;
    if (body_ == nullptr) {
        return result;
    }
    for (const RecordPtr& record : body_->state.records) {
        if (record != nullptr && record_matches(*record, filter)) {
            result.push_back(record->id);
        }
    }
    return result;
}

InventorySummary Snapshot::summary() const {
    InventorySummary summary;
    if (body_ == nullptr) {
        return summary;
    }
    summary.total_assets = static_cast<std::uint64_t>(body_->state.records.size());
    summary.published_sequence = body_->sequence;
    summary.epoch = body_->epoch;

    std::map<AssetClass, std::uint64_t> class_counts;
    std::map<LifecycleState, std::uint64_t> lifecycle_counts;
    std::map<InstallationState, std::uint64_t> installation_counts;
    std::map<OwnerId, std::uint64_t> owner_counts;

    for (const RecordPtr& record : body_->state.records) {
        if (record == nullptr) {
            continue;
        }
        ++class_counts[record->asset_class];
        ++lifecycle_counts[record->state.lifecycle];
        ++installation_counts[record->state.installation];
        if (record->metadata.owner.has_value()) {
            ++owner_counts[*record->metadata.owner];
        } else {
            ++summary.unowned_assets;
        }
        if (is_terminal_lifecycle(record->state.lifecycle)) {
            ++summary.terminal_assets;
        }
        if (permits_workload(record->state.lifecycle)) {
            ++summary.workload_capable_assets;
        }
        if (record->supersedes.has_value()) {
            ++summary.assets_with_predecessor;
            if (body_->state.find(record->supersedes->predecessor).found) {
                ++summary.assets_with_successor;
            }
        }
        summary.total_references += record->references.size();
        summary.total_provenance_steps += record->provenance.size();
        summary.estimated_bytes += record->estimated_bytes();
        if (summary.max_revision < record->revision) {
            summary.max_revision = record->revision;
        }
        for (const Reference& reference : record->references) {
            if (reference.evidence() == ReferenceEvidence::Unverified) {
                ++summary.assets_with_unverified_references;
                break;
            }
        }
    }
    summary.by_class.assign(class_counts.begin(), class_counts.end());
    summary.by_lifecycle.assign(lifecycle_counts.begin(), lifecycle_counts.end());
    summary.by_installation.assign(installation_counts.begin(), installation_counts.end());
    summary.by_owner.assign(owner_counts.begin(), owner_counts.end());
    return summary;
}

ConflictReport Snapshot::audit_conflicts() const {
    ConflictReport report;
    if (body_ == nullptr) {
        return report;
    }

    std::map<std::string, std::vector<std::size_t>> serial_claims;
    for (std::size_t index = 0; index < body_->state.records.size(); ++index) {
        const AssetRecord& record = *body_->state.records[index];
        serial_claims[record.serial_identity.canonical_key()].push_back(index);
    }
    for (const auto& claim : serial_claims) {
        if (claim.second.size() > 1) {
            report.duplicate_serial_keys.push_back(claim.first);
        }
        for (std::size_t first = 0; first < claim.second.size(); ++first) {
            for (std::size_t second = first + 1; second < claim.second.size(); ++second) {
                const AssetRecord& left = *body_->state.records[claim.second[first]];
                const AssetRecord& right = *body_->state.records[claim.second[second]];
                if (left.serial_identity.model_conflicts_with(right.serial_identity)) {
                    report.model_conflicts.emplace_back(left.id, right.id);
                }
            }
        }
    }

    for (const RecordPtr& record : body_->state.records) {
        if (record == nullptr) {
            continue;
        }
        if (record->supersedes.has_value() && !body_->state.find(record->supersedes->predecessor).found) {
            report.dangling_predecessors.push_back(record->id);
        }
        if (record->state.installation == InstallationState::Superseded &&
            !is_terminal_lifecycle(record->state.lifecycle)) {
            report.supersession_state_mismatches.push_back(record->id);
        }
        if (check_state_consistency(record->state.lifecycle, record->state.installation, body_->policy.consistency)
                .has_value()) {
            report.state_inconsistencies.push_back(record->id);
        }
    }
    return report;
}

Outcome<LineageChain> Snapshot::lineage(const AssetId& id, std::size_t max_depth) const {
    if (body_ == nullptr) {
        return make_error(ErrorCode::AssetNotFound, "the snapshot holds no inventory");
    }
    if (!body_->state.find(id).found) {
        return make_error(ErrorCode::AssetNotFound, "asset is not present in this snapshot").with_subject(id.to_string());
    }
    if (max_depth == 0) {
        return make_error(ErrorCode::InvalidInput, "lineage depth bound must be at least 1");
    }
    const std::size_t effective_depth = std::min(max_depth, limits::kMaxLineageDepth);

    LineageChain chain;
    std::unordered_set<std::string> visited;
    visited.insert(id.to_compact_string());
    AssetId cursor = id;
    while (chain.predecessors.size() < effective_depth) {
        const RecordPlacement current = body_->state.find(cursor);
        if (!current.found) {
            break;
        }
        const AssetRecord& record = *body_->state.records[current.index];
        if (!record.supersedes.has_value()) {
            chain.termination = LineageTermination::ReachedOrigin;
            break;
        }
        const AssetId predecessor = record.supersedes->predecessor;
        if (!visited.insert(predecessor.to_compact_string()).second) {
            // A cycle can only exist in hand-edited persisted state, because the
            // mutation API refuses to create one. Reported rather than traversed.
            return make_error(ErrorCode::ReplacementCycleDetected,
                              "replacement lineage revisits an identity; the retained state carries a cycle")
                .with_subject(id.to_string());
        }
        chain.predecessors.push_back(predecessor);
        cursor = predecessor;
        if (chain.predecessors.size() == effective_depth) {
            chain.termination = LineageTermination::DepthLimitReached;
        }
    }

    for (const RecordPtr& record : body_->state.records) {
        if (record == nullptr || !record->supersedes.has_value() ||
            !(record->supersedes->predecessor == id)) {
            continue;
        }
        if (chain.successors.size() >= limits::kMaxLineageFanout) {
            chain.successors_truncated = true;
            break;
        }
        chain.successors.push_back(record->id);
    }
    std::sort(chain.successors.begin(), chain.successors.end());
    return chain;
}

std::optional<AssetId> Snapshot::predecessor_of(const AssetId& id) const {
    if (body_ == nullptr) {
        return std::nullopt;
    }
    const RecordPlacement placement = body_->state.find(id);
    if (!placement.found) {
        return std::nullopt;
    }
    const AssetRecord& record = *body_->state.records[placement.index];
    if (!record.supersedes.has_value()) {
        return std::nullopt;
    }
    return record.supersedes->predecessor;
}

std::vector<AssetId> Snapshot::successors_of(const AssetId& id) const {
    std::vector<AssetId> result;
    if (body_ == nullptr) {
        return result;
    }
    const auto counted = body_->successor_count.find(id.to_compact_string());
    if (counted == body_->successor_count.end() || counted->second == 0) {
        return result;
    }
    for (const RecordPtr& record : body_->state.records) {
        if (record != nullptr && record->supersedes.has_value() && record->supersedes->predecessor == id) {
            result.push_back(record->id);
            if (result.size() >= limits::kMaxLineageFanout) {
                break;
            }
        }
    }
    return result;
}

Outcome<std::vector<ProvenanceStep>> Snapshot::provenance(const AssetId& id, std::size_t limit) const {
    if (body_ == nullptr) {
        return make_error(ErrorCode::AssetNotFound, "the snapshot holds no inventory");
    }
    const RecordPlacement placement = body_->state.find(id);
    if (!placement.found) {
        return make_error(ErrorCode::AssetNotFound, "asset is not present in this snapshot").with_subject(id.to_string());
    }
    const std::vector<ProvenanceStep>& steps = body_->state.records[placement.index]->provenance;
    const std::size_t take = std::min(limit, steps.size());
    return std::vector<ProvenanceStep>(steps.end() - static_cast<std::ptrdiff_t>(take), steps.end());
}

Outcome<std::vector<ProvenanceStep>> Snapshot::full_provenance(const AssetId& id) const {
    if (body_ == nullptr) {
        return make_error(ErrorCode::AssetNotFound, "the snapshot holds no inventory");
    }
    const RecordPlacement placement = body_->state.find(id);
    if (!placement.found) {
        return make_error(ErrorCode::AssetNotFound, "asset is not present in this snapshot").with_subject(id.to_string());
    }
    return body_->state.records[placement.index]->provenance;
}

std::uint64_t Snapshot::estimated_bytes() const noexcept {
    if (body_ == nullptr) {
        return 0;
    }
    std::uint64_t total = sizeof(RegistryBody);
    for (const RecordPtr& record : body_->state.records) {
        if (record != nullptr) {
            total += record->estimated_bytes();
        }
    }
    total += static_cast<std::uint64_t>(body_->state.ids.size()) * sizeof(AssetId);
    total += static_cast<std::uint64_t>(body_->state.records.size()) * sizeof(RecordPtr);
    for (const auto& entry : body_->serial_index) {
        total += static_cast<std::uint64_t>(entry.first.capacity()) + sizeof(entry);
    }
    for (const internal::StoredWriter& stored : body_->state.writers) {
        total += static_cast<std::uint64_t>(stored.writer.text().capacity());
        for (const internal::StoredIdempotency& record : stored.records) {
            total += static_cast<std::uint64_t>(record.key.capacity());
            total += static_cast<std::uint64_t>(record.error_message.capacity());
        }
    }
    return total;
}

}  // namespace asset_registry
