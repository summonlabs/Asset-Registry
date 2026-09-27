// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Lifecycle walkthrough: registers a small inventory, installs and activates one
// asset, records a failure and its replacement, and prints the resulting lineage
// and provenance.
//
// This example uses only the stable public API: no persistence structure, no
// internal type, and no authority shortcut.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "asset_registry/asset_registry.hpp"

namespace {

using namespace asset_registry;

/// Builds an identity deterministically from a label, so the example prints the
/// same identifiers on every run.
[[nodiscard]] AssetId identity_for(std::string_view label) {
    AssetId::bytes_type bytes{};
    std::uint64_t first = 0xCBF29CE484222325ULL;
    std::uint64_t second = 0x9E3779B97F4A7C15ULL;
    for (const char character : label) {
        first = (first ^ static_cast<std::uint8_t>(character)) * 0x100000001B3ULL;
        second = (second + static_cast<std::uint8_t>(character)) * 0x100000001B3ULL;
        second ^= second >> 29U;
    }
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[index] = static_cast<std::uint8_t>((first >> (index * 8U)) & 0xFFU);
        bytes[8 + index] = static_cast<std::uint8_t>((second >> (index * 8U)) & 0xFFU);
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
    return AssetId::from_bytes(bytes);
}

[[nodiscard]] SerialIdentity serial_for(std::string_view manufacturer, std::string_view number) {
    const auto manufacturer_id = ManufacturerIdentity::create(manufacturer).value();
    const auto serial_id = SerialNumber::create(number).value();
    return SerialIdentity::create(manufacturer_id, serial_id, std::nullopt).value();
}

void report(const Error& error) {
    std::fprintf(stderr, "  refused: %s\n", error.to_string().c_str());
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path directory =
        argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() / "asset-registry-example";
    std::printf("store directory: %s\n", directory.string().c_str());

    RegistryPolicy policy;
    // A physical replacement carries a new serial number, so the default
    // duplicate-rejecting policy is the right one here.
    StoreOpenOptions options;
    options.policy = policy;
    options.initial_writer = "lifecycle-example";

    RecoveryReport report_out;
    auto opened = AssetRegistry::open(directory, StoreOpenMode::CreateIfMissing, options, report_out);
    if (!opened) {
        report(opened.error());
        return 1;
    }
    std::shared_ptr<AssetRegistry> registry = opened.value();
    std::printf("epoch=%s recovery=%.*s\n", registry->epoch().to_string().c_str(),
                static_cast<int>(to_string(report_out.action).size()), to_string(report_out.action).data());

    const auto writer_id = WriterId::create("lifecycle-example");
    auto session_result = registry->open_writer(writer_id.value());
    if (!session_result) {
        report(session_result.error());
        return 1;
    }
    WriterSession session = session_result.value();

    // 1. Register a server with a typed capability reference and a placement.
    const AssetId server = identity_for("example-server");
    RegisterAssetRequest request;
    request.id_mode = AssetIdMode::CallerSupplied;
    request.id = server;
    request.asset_class = AssetClass::Server;
    request.serial_identity = serial_for("Example Compute", "SRV-0001");
    request.metadata.display_name = "example-server";
    request.metadata.owner = OwnerId::create("org.example.platform").value();
    request.metadata.site = LocationId::create("site-alpha.hall-1").value();
    request.metadata.labels = {{"tier", "gold"}};
    request.references = {Reference::capability(
        CapabilityReference::create("asi:accelerator.scheduling").value(), ReferenceEvidence::Verified)};
    request.lifecycle = LifecycleState::Planned;
    request.installation = InstallationState::Unknown;

    auto registered = registry->register_asset(session, session.advance(), request);
    if (!registered) {
        report(registered.error());
        return 1;
    }
    std::printf("registered %s at revision %s\n", registered.value().id.to_string().c_str(),
                registered.value().revision.to_string().c_str());

    // 2. Install it: staged, then installed. The registry refuses the shortcut.
    for (const InstallationState state :
         {InstallationState::NotInstalled, InstallationState::Staged, InstallationState::Installed}) {
        auto transitioned =
            registry->transition_installation(session, session.advance(), server, std::nullopt, state);
        if (!transitioned) {
            report(transitioned.error());
            return 1;
        }
    }
    // 3. Bring it into service.
    for (const LifecycleState state : {LifecycleState::Provisioned, LifecycleState::Active}) {
        auto transitioned = registry->transition_lifecycle(session, session.advance(), server, std::nullopt, state);
        if (!transitioned) {
            report(transitioned.error());
            return 1;
        }
    }
    // 4. Attach the rack the server physically occupies.
    auto placed = registry->set_placement(session, session.advance(), server, std::nullopt,
                                          PlacementChange{.rack = RackId::create("site-alpha.hall-1.rack-4").value(),
                                                          .units = UnitSpan::create(10, 12).value(),
                                                          .evidence = ReferenceEvidence::Verified});
    if (!placed) {
        report(placed.error());
        return 1;
    }

    // 5. A stale update is refused: the caller must re-read before retrying.
    MetadataPatch stale_patch;
    stale_patch.notes = "written against a revision that no longer exists";
    auto stale = registry->update_metadata(session, session.advance(), server, AssetRevision(1), stale_patch);
    if (stale) {
        std::fprintf(stderr, "unexpected: a stale revision was accepted\n");
        return 1;
    }
    std::printf("stale update refused as expected: %s\n", error_code_name(stale.error().code()).data());

    // 6. Retire the server and register its replacement with an explicit lineage.
    auto decommissioned =
        registry->transition_lifecycle(session, session.advance(), server, std::nullopt, LifecycleState::Decommissioned);
    if (!decommissioned) {
        report(decommissioned.error());
        return 1;
    }
    const AssetId replacement = identity_for("example-server-replacement");
    RegisterAssetRequest replacement_request;
    replacement_request.id_mode = AssetIdMode::CallerSupplied;
    replacement_request.id = replacement;
    replacement_request.asset_class = AssetClass::Server;
    replacement_request.serial_identity = serial_for("Example Compute", "SRV-0002");
    replacement_request.metadata.display_name = "example-server-replacement";
    replacement_request.metadata.owner = OwnerId::create("org.example.platform").value();
    replacement_request.lifecycle = LifecycleState::Planned;
    replacement_request.installation = InstallationState::Unknown;
    replacement_request.supersedes = server;
    replacement_request.supersession_cause = ReplacementCause::Failure;
    replacement_request.supersession_note = "power supply failure in rack 4";

    auto replaced = registry->register_asset(session, session.advance(), replacement_request);
    if (!replaced) {
        report(replaced.error());
        return 1;
    }
    std::printf("registered replacement %s superseding %s\n", replaced.value().id.to_string().c_str(),
                server.to_string().c_str());

    // 7. Read the result through an immutable snapshot.
    const Snapshot snapshot = registry->snapshot();
    const InventorySummary summary = snapshot.summary();
    std::printf("\ninventory: %llu asset(s), %llu reference(s), %llu retained provenance step(s)\n",
                static_cast<unsigned long long>(summary.total_assets),
                static_cast<unsigned long long>(summary.total_references),
                static_cast<unsigned long long>(summary.total_provenance_steps));

    auto chain = snapshot.lineage(replacement);
    if (chain) {
        std::printf("lineage of %s: termination=%.*s\n", replacement.to_string().c_str(),
                    static_cast<int>(to_string(chain.value().termination).size()),
                    to_string(chain.value().termination).data());
        for (const AssetId& ancestor : chain.value().predecessors) {
            std::printf("  supersedes %s\n", ancestor.to_string().c_str());
        }
    }

    auto provenance = snapshot.full_provenance(server);
    if (provenance) {
        std::printf("provenance of %s (%zu step(s)):\n", server.to_string().c_str(), provenance.value().size());
        for (const ProvenanceStep& step : provenance.value()) {
            std::printf("  %s rev=%-3s %-24.*s %s\n", step.sequence.to_string().c_str(),
                        step.revision.to_string().c_str(), static_cast<int>(to_string(step.action).size()),
                        to_string(step.action).data(), step.change.c_str());
        }
    }

    // 8. Canonical export.
    ExportOptions export_options;
    ExportReport export_report;
    auto document = registry->export_document(export_options, export_report);
    if (!document) {
        report(document.error());
        return 1;
    }
    std::printf("canonical export: %llu record(s), %llu byte(s)\n",
                static_cast<unsigned long long>(export_report.records_exported),
                static_cast<unsigned long long>(export_report.bytes_written));

    auto closed = registry->close();
    if (!closed) {
        report(closed.error());
        return 1;
    }
    std::printf("store closed cleanly\n");
    return 0;
}
