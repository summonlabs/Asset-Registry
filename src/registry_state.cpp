// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "registry_state.hpp"

#include "asset_registry/query.hpp"

namespace asset_registry {

std::size_t IndexedState::insert(RecordPtr record) {
    const AssetId id = record->id;
    const auto iterator = std::lower_bound(ids.begin(), ids.end(), id);
    const std::size_t index = static_cast<std::size_t>(iterator - ids.begin());
    ids.insert(iterator, id);
    records.insert(records.begin() + static_cast<std::ptrdiff_t>(index), std::move(record));
    return index;
}

void IndexedState::replace_at(std::size_t index, RecordPtr record) {
    if (index >= records.size()) {
        return;
    }
    records[index] = std::move(record);
}

void IndexedState::erase_at(std::size_t index) {
    if (index >= records.size()) {
        return;
    }
    records.erase(records.begin() + static_cast<std::ptrdiff_t>(index));
    ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(index));
}

const internal::StoredWriter* IndexedState::find_writer(const WriterId& writer) const noexcept {
    for (const internal::StoredWriter& stored : writers) {
        if (stored.writer == writer) {
            return &stored;
        }
    }
    return nullptr;
}

internal::StoredWriter* IndexedState::find_writer(const WriterId& writer) noexcept {
    for (internal::StoredWriter& stored : writers) {
        if (stored.writer == writer) {
            return &stored;
        }
    }
    return nullptr;
}

std::size_t IndexedState::writer_slot_for_insert(const WriterId& writer) const noexcept {
    std::size_t slot = 0;
    while (slot < writers.size() && writers[slot].writer < writer) {
        ++slot;
    }
    return slot;
}

bool record_matches(const AssetRecord& record, const QueryFilter& filter) {
    if (filter.asset_class.has_value() && record.asset_class != *filter.asset_class) {
        return false;
    }
    if (filter.lifecycle.has_value() && record.state.lifecycle != *filter.lifecycle) {
        return false;
    }
    if (filter.installation.has_value() && record.state.installation != *filter.installation) {
        return false;
    }
    if (filter.owner.has_value()) {
        if (!record.metadata.owner.has_value() || !(*record.metadata.owner == *filter.owner)) {
            return false;
        }
    }
    if (filter.unowned_only && record.metadata.owner.has_value()) {
        return false;
    }
    if (filter.site.has_value()) {
        if (!record.metadata.site.has_value() || !(*record.metadata.site == *filter.site)) {
            return false;
        }
    }
    if (filter.workload_capable_only && !permits_workload(record.state.lifecycle)) {
        return false;
    }
    if (filter.terminal_only && !is_terminal_lifecycle(record.state.lifecycle)) {
        return false;
    }
    if (filter.location.has_value() || filter.rack.has_value() || filter.capability.has_value()) {
        bool matched_location = !filter.location.has_value();
        bool matched_rack = !filter.rack.has_value();
        bool matched_capability = !filter.capability.has_value();
        for (const Reference& reference : record.references) {
            if (filter.location.has_value() && reference.kind() == Reference::Kind::Location &&
                reference.location() == *filter.location) {
                matched_location = true;
            }
            if (filter.rack.has_value() && reference.kind() == Reference::Kind::Rack &&
                reference.rack() == *filter.rack) {
                matched_rack = true;
            }
            if (filter.capability.has_value() && reference.kind() == Reference::Kind::Capability &&
                reference.capability() == *filter.capability) {
                matched_capability = true;
            }
        }
        if (!matched_location || !matched_rack || !matched_capability) {
            return false;
        }
    }
    if (filter.has_unverified_references_only) {
        bool found = false;
        for (const Reference& reference : record.references) {
            if (reference.evidence() == ReferenceEvidence::Unverified) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

std::shared_ptr<const SecondaryIndices> build_secondary_indices(const IndexedState& state,
                                                                TransactionSequence sequence) {
    auto indices = std::make_shared<SecondaryIndices>();
    indices->sequence = sequence;
    for (std::size_t index = 0; index < state.records.size(); ++index) {
        const AssetRecord& record = *state.records[index];
        const AssetId& id = state.ids[index];
        indices->by_class[record.asset_class].push_back(id);
        indices->by_lifecycle[record.state.lifecycle].push_back(id);
        indices->by_installation[record.state.installation].push_back(id);
        if (record.metadata.owner.has_value()) {
            indices->by_owner[*record.metadata.owner].push_back(id);
        } else {
            indices->unowned.push_back(id);
        }
        if (permits_workload(record.state.lifecycle)) {
            indices->workload_capable.push_back(id);
        }
        if (is_terminal_lifecycle(record.state.lifecycle)) {
            indices->terminal.push_back(id);
        }
        if (record.supersedes.has_value()) {
            indices->with_predecessor.push_back(id);
        }
        bool unverified = false;
        for (const Reference& reference : record.references) {
            switch (reference.kind()) {
                case Reference::Kind::Capability:
                    indices->by_capability[reference.capability().canonical()].push_back(id);
                    break;
                case Reference::Kind::Location:
                    indices->by_location[reference.location().text()].push_back(id);
                    break;
                case Reference::Kind::Rack:
                    indices->by_rack[reference.rack().text()].push_back(id);
                    break;
                case Reference::Kind::ExternalObject:
                    break;
            }
            if (reference.evidence() == ReferenceEvidence::Unverified) {
                unverified = true;
            }
        }
        if (unverified) {
            indices->with_unverified_references.push_back(id);
        }
    }
    return indices;
}

}  // namespace asset_registry
