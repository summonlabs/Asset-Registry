// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_registry/limits.hpp"

#include <cstdio>
#include <cstdlib>

namespace asset_registry {

std::optional<std::string> validate_limits(const RegistryLimits& limits) {
    const auto reject = [](std::string_view message) { return std::optional<std::string>(std::string(message)); };

    if (limits.max_assets == 0) {
        return reject("max_assets must be at least 1");
    }
    if (limits.max_references_per_asset == 0) {
        return reject("max_references_per_asset must be at least 1");
    }
    if (limits.max_labels_per_asset == 0) {
        return reject("max_labels_per_asset must be at least 1");
    }
    if (limits.max_provenance_per_asset == 0) {
        return reject("max_provenance_per_asset must be at least 1; a record with no retained provenance "
                      "cannot demonstrate where its state came from");
    }
    if (limits.max_generations_per_asset == 0) {
        return reject("max_generations_per_asset must be at least 1");
    }
    if (limits.max_display_name_bytes == 0 || limits.max_label_value_bytes == 0) {
        return reject("text bounds must be non-zero");
    }
    if (limits.max_import_records == 0) {
        return reject("max_import_records must be at least 1");
    }
    if (limits.max_export_records == 0) {
        return reject("max_export_records must be at least 1");
    }
    if (limits.max_export_records < limits.max_assets) {
        return reject("max_export_records must be at least max_assets, otherwise a full export is impossible");
    }
    // An import bound below the asset bound is usable: a batch is bounded by the import
    // bound and a larger inventory is restored in several batches, each of which is
    // committed atomically. Only the export relation makes a documented operation
    // impossible outright, so only that one is rejected here.
    if (limits.max_document_bytes == 0 || limits.max_document_bytes > kAbsoluteMaxDocumentBytes) {
        return reject("max_document_bytes is outside the supported range");
    }
    if (limits.max_registry_bytes == 0) {
        return reject("max_registry_bytes must be at least 1");
    }
    if (limits.max_idempotency_records_per_writer == 0) {
        return reject("max_idempotency_records_per_writer must be at least 1");
    }
    if (limits.max_tracked_writers == 0) {
        return reject("max_tracked_writers must be at least 1");
    }
    if (limits.max_retained_generations < 2) {
        return reject("max_retained_generations must be at least 2, otherwise a commit would delete the "
                      "generation it is replacing before the replacement is published");
    }
    if (limits.max_traversal_depth == 0) {
        return reject("max_traversal_depth must be at least 1");
    }
    return std::nullopt;
}

const RegistryLimits& default_limits() noexcept {
    // The import and export bounds must be able to carry a full inventory,
    // otherwise a full restore or a full export would be impossible; the defaults
    // are therefore derived from the asset bound rather than chosen independently.
    // The values are checked once here so that an inconsistent default cannot be
    // shipped silently.
    static const RegistryLimits kLimits = [] {
        RegistryLimits limits;
        limits.max_assets = 1'000'000;
        limits.max_import_records = 1'000'000;
        limits.max_export_records = 5'000'000;
        return limits;
    }();
    static const bool validated = [] {
        const auto failure = validate_limits(kLimits);
        if (failure.has_value()) {
            std::fprintf(stderr, "asset registry default bounds are inconsistent: %s\n", failure->c_str());
            std::abort();
        }
        return true;
    }();
    (void)validated;
    return kLimits;
}

}  // namespace asset_registry
