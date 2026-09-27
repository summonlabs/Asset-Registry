// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Consumer composition example: parses a canonical export document and answers
// questions from it without opening a store.
//
// This is the shape a later DCCP repository uses: it receives an immutable
// snapshot and never touches persistence, authority, or mutation.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "asset_registry/asset_registry.hpp"

namespace {

using namespace asset_registry;

/// Looks for assets that are candidates for a physical audit: they claim to be
/// active but their recorded installation evidence is not verified.
[[nodiscard]] std::vector<AssetId> unverified_active_assets(const Snapshot& snapshot) {
    std::vector<AssetId> result;
    snapshot.for_each([&result](const AssetRecord& record) -> bool {
        if (record.state.lifecycle == LifecycleState::Active &&
            record.state.installation == InstallationState::Installed) {
            return true;
        }
        const bool has_placement = std::any_of(record.references.begin(), record.references.end(),
                                               [](const Reference& reference) {
                                                   return reference.kind() == Reference::Kind::Rack;
                                               });
        if (has_placement) {
            bool verified = false;
            for (const Reference& reference : record.references) {
                if (reference.kind() == Reference::Kind::Rack && is_actionable(reference.evidence())) {
                    verified = true;
                    break;
                }
            }
            if (!verified) {
                result.push_back(record.id);
            }
        }
        return true;
    });
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <export-document.json>\n", argv[0]);
        return 2;
    }
    const std::string path = argv[1];
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return 2;
    }
    std::string document;
    char buffer[8192];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        document.append(buffer, read);
    }
    std::fclose(file);

    // Compose a read-only registry from the document. The consumer holds a real
    // registry, so every query, lineage walk, and aggregate the library offers is
    // available without any durable store.
    auto created = AssetRegistry::create_detached();
    if (!created) {
        std::fprintf(stderr, "cannot create a registry: %s\n", created.error().to_string().c_str());
        return 1;
    }
    std::shared_ptr<AssetRegistry> registry = created.value();
    auto session_result = registry->open_writer(WriterId::create("document-consumer").value());
    if (!session_result) {
        std::fprintf(stderr, "cannot arm a writer: %s\n", session_result.error().to_string().c_str());
        return 1;
    }
    WriterSession session = session_result.value();

    auto parsed = registry->parse_import_document(document, ImportOptions{});
    if (!parsed) {
        std::fprintf(stderr, "document refused: %s\n", parsed.error().to_string().c_str());
        return 1;
    }
    ImportOptions import_options;
    import_options.source = path;
    auto imported = registry->import_assets(session, session.advance(), parsed.value(), import_options);
    if (!imported) {
        std::fprintf(stderr, "import refused: %s\n", imported.error().to_string().c_str());
        return 1;
    }

    const Snapshot snapshot = registry->snapshot();
    const InventorySummary summary = snapshot.summary();
    std::printf("document:            %s\n", path.c_str());
    std::printf("schema version:      %u\n", kExportSchemaVersion);
    std::printf("assets:              %llu\n", static_cast<unsigned long long>(summary.total_assets));
    std::printf("workload capable:    %llu\n", static_cast<unsigned long long>(summary.workload_capable_assets));
    std::printf("terminal:            %llu\n", static_cast<unsigned long long>(summary.terminal_assets));
    std::printf("unowned:             %llu\n", static_cast<unsigned long long>(summary.unowned_assets));
    std::printf("records rejected:    %llu\n", static_cast<unsigned long long>(imported.value().records_rejected()));
    for (const ImportRejection& rejection : imported.value().rejections) {
        std::printf("  rejected record %llu: %s\n", static_cast<unsigned long long>(rejection.record_index),
                    rejection.message.c_str());
    }

    std::printf("class breakdown:\n");
    for (const auto& entry : summary.by_class) {
        std::printf("  %-30.*s %llu\n", static_cast<int>(to_string(entry.first).size()),
                    to_string(entry.first).data(), static_cast<unsigned long long>(entry.second));
    }
    std::printf("lifecycle breakdown:\n");
    for (const auto& entry : summary.by_lifecycle) {
        std::printf("  %-30.*s %llu\n", static_cast<int>(to_string(entry.first).size()),
                    to_string(entry.first).data(), static_cast<unsigned long long>(entry.second));
    }

    const std::vector<AssetId> audit_candidates = unverified_active_assets(snapshot);
    std::printf("assets needing a physical audit (unverified placement): %zu\n", audit_candidates.size());
    for (const AssetId& identifier : audit_candidates) {
        const auto view = snapshot.find(identifier);
        std::printf("  %s  %s\n", identifier.to_string().c_str(),
                    view.has_value() ? view->metadata.display_name.c_str() : "(absent)");
    }

    std::printf("lineage chains:\n");
    for (const AssetId& identifier : snapshot.ids()) {
        const auto chain = snapshot.lineage(identifier, 64);
        if (!chain || chain.value().predecessors.empty()) {
            continue;
        }
        std::printf("  %s has %zu predecessor(s), termination=%.*s\n", identifier.to_string().c_str(),
                    chain.value().predecessors.size(),
                    static_cast<int>(to_string(chain.value().termination).size()),
                    to_string(chain.value().termination).data());
    }

    const ConflictReport conflicts = snapshot.audit_conflicts();
    std::printf("conflict audit: %s\n", conflicts.clean() ? "clean" : "NOT CLEAN");
    if (!conflicts.clean()) {
        return 1;
    }
    (void)registry->close();
    return 0;
}
