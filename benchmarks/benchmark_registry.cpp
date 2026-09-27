// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Benchmarks.
//
// What is measured: completed operations, counted only after the operation has
// returned success. Where durability is part of the operation, the full commit
// cost is inside the measurement: registration, metadata update, and import all
// include writing a generation, flushing it, publishing the pointer, and flushing
// the metadata record. Nothing here measures submission or enqueue latency, and
// nothing here is a claim about production hardware: the numbers describe this
// machine, and the configuration each run used is printed alongside the results.
//
// What is not measured: any accelerator, network, PDU, UPS, cooling, or other
// facility hardware. This repository holds inventory records; it does not talk to
// devices, and no benchmark here implies that it does.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "asset_registry/asset_registry.hpp"

namespace {

using namespace asset_registry;
using SteadyClock = std::chrono::steady_clock;

struct Result {
    std::string name;
    std::uint64_t operations = 0;
    double milliseconds = 0.0;
    std::uint64_t checksum = 0;
};

std::vector<Result>& results() {
    static std::vector<Result> collected;
    return collected;
}

[[nodiscard]] double elapsed_ms(SteadyClock::time_point start) {
    return std::chrono::duration<double, std::milli>(SteadyClock::now() - start).count();
}

void report_result(std::string name, std::uint64_t operations, double milliseconds, std::uint64_t checksum) {
    results().push_back(Result{std::move(name), operations, milliseconds, checksum});
    const double per_operation_us = operations == 0 ? 0.0 : milliseconds * 1000.0 / static_cast<double>(operations);
    const double per_second = milliseconds == 0.0 ? 0.0 : static_cast<double>(operations) * 1000.0 / milliseconds;
    std::printf("  %-46s %10llu ops  %10.2f ms  %9.2f us/op  %12.0f ops/s  checksum=%llu\n",
                results().back().name.c_str(), static_cast<unsigned long long>(operations), milliseconds,
                per_operation_us, per_second, static_cast<unsigned long long>(checksum));
    std::fflush(stdout);
}

/// Deterministic identity, so a benchmark run is reproducible and the checksum is
/// meaningful.
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

[[nodiscard]] SerialIdentity serial_for(std::string_view label) {
    const auto manufacturer = ManufacturerIdentity::create("Benchmark Vendor").value();
    const auto number = SerialNumber::create("BM-" + std::string(label)).value();
    return SerialIdentity::create(manufacturer, number, std::nullopt).value();
}

[[nodiscard]] RegisterAssetRequest make_request(std::string_view label, bool with_references) {
    RegisterAssetRequest request;
    request.id_mode = AssetIdMode::CallerSupplied;
    request.id = identity_for(label);
    request.asset_class = AssetClass::Server;
    request.serial_identity = serial_for(label);
    request.metadata.display_name = std::string(label);
    request.metadata.owner = OwnerId::create("org.benchmark.platform").value();
    request.metadata.site = LocationId::create("site-bench.hall-1").value();
    request.metadata.labels = {{"tier", "gold"}, {"bench", "true"}};
    if (with_references) {
        request.references = {
            Reference::capability(CapabilityReference::create("asi:accelerator.scheduling").value(),
                                  ReferenceEvidence::Verified),
            Reference::location(LocationId::create("site-bench.hall-1.room-1").value(), ReferenceEvidence::Verified),
            Reference::rack(RackId::create("site-bench.hall-1.rack-1").value(), ReferenceEvidence::Verified)};
    }
    request.lifecycle = LifecycleState::Planned;
    request.installation = InstallationState::Unknown;
    return request;
}

/// Registers `count` assets into a store, reporting the completed count and the
/// total durable cost. Each registration is one durable commit.
[[nodiscard]] Outcome<std::shared_ptr<AssetRegistry>> seed_store(const std::filesystem::path& directory,
                                                                 std::string_view prefix, std::uint64_t count,
                                                                 bool with_references) {
    StoreOpenOptions options;
    options.initial_writer = "benchmark";
    RecoveryReport report;
    auto opened = AssetRegistry::open(directory, StoreOpenMode::CreateIfMissing, options, report);
    if (!opened) {
        return opened.error();
    }
    auto registry = opened.value();
    auto session = registry->open_writer(WriterId::create("benchmark").value());
    if (!session) {
        return session.error();
    }
    WriterSession writer = session.value();
    std::uint64_t completed = 0;
    const SteadyClock::time_point start = SteadyClock::now();
    for (std::uint64_t index = 0; index < count; ++index) {
        const std::string label = std::string(prefix) + "-" + std::to_string(index);
        auto registered = registry->register_asset(writer, writer.advance(), make_request(label, with_references));
        if (!registered) {
            std::fprintf(stderr, "seed refused at %llu: %s\n", static_cast<unsigned long long>(index),
                         registered.error().to_string().c_str());
            return registered.error();
        }
        ++completed;
    }
    const double milliseconds = elapsed_ms(start);
    std::uint64_t checksum = 0;
    for (const AssetId& identifier : registry->snapshot().ids()) {
        checksum = checksum * 31U + identifier.bytes()[0];
    }
    report_result("durable registration (one commit per asset)", completed, milliseconds, checksum);

    auto closed = registry->close();
    if (!closed) {
        return closed.error();
    }
    return registry;
}

void benchmark_registration(const std::filesystem::path& work, std::uint64_t count) {
    std::printf("registration: %llu assets, one durable commit each\n", static_cast<unsigned long long>(count));
    const std::filesystem::path directory = work / ("register-" + std::to_string(count));
    std::filesystem::remove_all(directory);
    auto seeded = seed_store(directory, "bench", count, true);
    if (!seeded) {
        std::fprintf(stderr, "seed failed: %s\n", seeded.error().to_string().c_str());
        std::exit(1);
    }
}

void benchmark_reopen(const std::filesystem::path& work, std::uint64_t count) {
    const std::filesystem::path directory = work / ("register-" + std::to_string(count));
    if (!std::filesystem::exists(directory)) {
        return;
    }
    std::printf("open and recover: %llu assets, %u retained generation(s)\n",
                static_cast<unsigned long long>(count), 0U);
    constexpr int kRounds = 5;
    const SteadyClock::time_point start = SteadyClock::now();
    std::uint64_t checksum = 0;
    std::uint64_t completed = 0;
    for (int round = 0; round < kRounds; ++round) {
        StoreOpenOptions options;
        RecoveryReport report;
        auto opened = AssetRegistry::open(directory, StoreOpenMode::CreateIfMissing, options, report);
        if (!opened) {
            std::fprintf(stderr, "reopen failed: %s\n", opened.error().to_string().c_str());
            std::exit(1);
        }
        const Snapshot snapshot = opened.value()->snapshot();
        checksum += snapshot.size();
        ++completed;
        auto closed = opened.value()->close();
        if (!closed) {
            std::fprintf(stderr, "close failed: %s\n", closed.error().to_string().c_str());
            std::exit(1);
        }
    }
    report_result("open + verify + load inventory", completed, elapsed_ms(start), checksum);
}

void benchmark_queries(const std::filesystem::path& work, std::uint64_t count) {
    const std::filesystem::path directory = work / ("register-" + std::to_string(count));
    if (!std::filesystem::exists(directory)) {
        return;
    }
    StoreOpenOptions options;
    RecoveryReport report;
    auto opened = AssetRegistry::open(directory, StoreOpenMode::ReadOnly, options, report);
    if (!opened) {
        std::fprintf(stderr, "read-only open failed: %s\n", opened.error().to_string().c_str());
        std::exit(1);
    }
    auto registry = opened.value();
    const Snapshot snapshot = registry->snapshot();
    std::printf("queries over %llu asset(s):\n", static_cast<unsigned long long>(snapshot.size()));

    // Point lookup by canonical identity.
    {
        constexpr std::uint64_t kOperations = 100000;
        std::uint64_t checksum = 0;
        std::uint64_t completed = 0;
        const SteadyClock::time_point start = SteadyClock::now();
        for (std::uint64_t index = 0; index < kOperations; ++index) {
            const std::string label = "bench-" + std::to_string(index % count);
            const auto view = snapshot.find(identity_for(label));
            if (view.has_value()) {
                ++completed;
                checksum += view->revision.value();
            }
        }
        report_result("point lookup by canonical identity", completed, elapsed_ms(start), checksum);
    }
    // Lookup by serial identity.
    {
        constexpr std::uint64_t kOperations = 100000;
        std::uint64_t checksum = 0;
        std::uint64_t completed = 0;
        const SteadyClock::time_point start = SteadyClock::now();
        for (std::uint64_t index = 0; index < kOperations; ++index) {
            const std::string label = "bench-" + std::to_string(index % count);
            const auto identifier = snapshot.find_by_serial(serial_for(label));
            if (identifier.has_value()) {
                ++completed;
                checksum += identifier->bytes()[0];
            }
        }
        report_result("point lookup by serial identity", completed, elapsed_ms(start), checksum);
    }
    // Filtered enumeration.
    {
        QueryFilter filter;
        filter.asset_class = AssetClass::Server;
        constexpr std::uint64_t kOperations = 20;
        std::uint64_t checksum = 0;
        std::uint64_t completed = 0;
        const SteadyClock::time_point start = SteadyClock::now();
        for (std::uint64_t index = 0; index < kOperations; ++index) {
            const std::vector<AssetId> matched = snapshot.matching(filter);
            checksum += matched.size();
            ++completed;
        }
        report_result("filtered enumeration (full scan, one class)", completed, elapsed_ms(start), checksum);
    }
    // Full enumeration through the visitor path.
    {
        constexpr std::uint64_t kOperations = 20;
        std::uint64_t checksum = 0;
        std::uint64_t completed = 0;
        const SteadyClock::time_point start = SteadyClock::now();
        for (std::uint64_t index = 0; index < kOperations; ++index) {
            std::uint64_t visited = 0;
            snapshot.for_each([&visited](const AssetRecord&) -> bool {
                ++visited;
                return true;
            });
            checksum += visited;
            ++completed;
        }
        report_result("full enumeration (no allocation per record)", completed, elapsed_ms(start), checksum);
    }
    // Aggregate summary.
    {
        constexpr std::uint64_t kOperations = 20;
        std::uint64_t checksum = 0;
        std::uint64_t completed = 0;
        const SteadyClock::time_point start = SteadyClock::now();
        for (std::uint64_t index = 0; index < kOperations; ++index) {
            const InventorySummary summary = snapshot.summary();
            checksum += summary.total_assets;
            ++completed;
        }
        report_result("inventory summary (exact aggregate)", completed, elapsed_ms(start), checksum);
    }
    // Lineage resolution.
    {
        constexpr std::uint64_t kOperations = 20000;
        std::uint64_t checksum = 0;
        std::uint64_t completed = 0;
        const SteadyClock::time_point start = SteadyClock::now();
        for (std::uint64_t index = 0; index < kOperations; ++index) {
            const auto chain = snapshot.lineage(identity_for("bench-" + std::to_string(index % count)), 64);
            if (chain) {
                ++completed;
                checksum += chain.value().successors.size();
            } else {
                // An asset that is not present is still a completed query: the
                // rejection is the operation's result.
                ++completed;
            }
        }
        report_result("lineage resolution and cycle check", completed, elapsed_ms(start), checksum);
    }
    // Canonical export of the whole inventory.
    {
        ExportOptions export_options;
        export_options.indent = 0;
        ExportReport export_report;
        constexpr std::uint64_t kOperations = 3;
        std::uint64_t checksum = 0;
        std::uint64_t completed = 0;
        const SteadyClock::time_point start = SteadyClock::now();
        for (std::uint64_t index = 0; index < kOperations; ++index) {
            auto document = registry->export_document(export_options, export_report);
            if (!document) {
                std::fprintf(stderr, "export failed: %s\n", document.error().to_string().c_str());
                std::exit(1);
            }
            checksum += document.value().size();
            ++completed;
        }
        report_result("canonical export (whole inventory)", completed, elapsed_ms(start), checksum);
    }
    auto closed = registry->close();
    if (!closed) {
        std::fprintf(stderr, "close failed: %s\n", closed.error().to_string().c_str());
        std::exit(1);
    }
}

void benchmark_updates(const std::filesystem::path& work, std::uint64_t count, std::uint64_t operations) {
    const std::filesystem::path directory = work / ("update-" + std::to_string(count));
    std::filesystem::remove_all(directory);
    auto seeded = seed_store(directory, "upd", count, false);
    if (!seeded) {
        std::fprintf(stderr, "seed failed: %s\n", seeded.error().to_string().c_str());
        std::exit(1);
    }
    std::printf("mutation over %llu asset(s):\n", static_cast<unsigned long long>(count));
    StoreOpenOptions options;
    RecoveryReport report;
    auto opened = AssetRegistry::open(directory, StoreOpenMode::CreateIfMissing, options, report);
    if (!opened) {
        std::fprintf(stderr, "open failed: %s\n", opened.error().to_string().c_str());
        std::exit(1);
    }
    auto registry = opened.value();
    auto session = registry->open_writer(WriterId::create("benchmark-updates").value());
    if (!session) {
        std::fprintf(stderr, "grant failed: %s\n", session.error().to_string().c_str());
        std::exit(1);
    }
    WriterSession writer = session.value();

    std::uint64_t checksum = 0;
    std::uint64_t completed = 0;
    const SteadyClock::time_point start = SteadyClock::now();
    for (std::uint64_t index = 0; index < operations; ++index) {
        const std::string label = "upd-" + std::to_string(index % count);
        MetadataPatch patch;
        patch.notes = "mutation " + std::to_string(index);
        auto updated = registry->update_metadata(writer, writer.advance(), identity_for(label), std::nullopt, patch);
        if (!updated) {
            std::fprintf(stderr, "update refused: %s\n", updated.error().to_string().c_str());
            std::exit(1);
        }
        checksum += updated.value().revision.value();
        ++completed;
    }
    report_result("metadata mutation (one durable commit each)", completed, elapsed_ms(start), checksum);

    // The final state must be internally consistent: a benchmark that leaves a
    // corrupt store has measured nothing useful.
    const Snapshot snapshot = registry->snapshot();
    const ConflictReport conflicts = snapshot.audit_conflicts();
    if (!conflicts.clean() || snapshot.size() != count) {
        std::fprintf(stderr, "post-benchmark state is not consistent\n");
        std::exit(1);
    }
    auto closed = registry->close();
    if (!closed) {
        std::fprintf(stderr, "close failed: %s\n", closed.error().to_string().c_str());
        std::exit(1);
    }
    std::printf("  post-benchmark integrity: %llu asset(s), audit clean, sequence=%s\n",
                static_cast<unsigned long long>(snapshot.size()),
                snapshot.published_sequence().to_string().c_str());
}

void benchmark_import(const std::filesystem::path& work, std::uint64_t count) {
    const std::filesystem::path directory = work / ("register-" + std::to_string(count));
    if (!std::filesystem::exists(directory)) {
        return;
    }
    StoreOpenOptions options;
    RecoveryReport report;
    auto opened = AssetRegistry::open(directory, StoreOpenMode::ReadOnly, options, report);
    if (!opened) {
        std::fprintf(stderr, "read-only open failed: %s\n", opened.error().to_string().c_str());
        std::exit(1);
    }
    ExportOptions export_options;
    export_options.indent = 0;
    ExportReport export_report;
    auto document = opened.value()->export_document(export_options, export_report);
    if (!document) {
        std::fprintf(stderr, "export failed: %s\n", document.error().to_string().c_str());
        std::exit(1);
    }
    (void)opened.value()->close();
    std::printf("document pipeline over %llu record(s):\n", static_cast<unsigned long long>(count));

    // Parse.
    {
        std::uint64_t checksum = 0;
        std::uint64_t completed = 0;
        const SteadyClock::time_point start = SteadyClock::now();
        for (int round = 0; round < 3; ++round) {
            auto parsed = parse_import_json(document.value(), default_limits());
            if (!parsed) {
                std::fprintf(stderr, "parse failed: %s\n", parsed.error().to_string().c_str());
                std::exit(1);
            }
            checksum += parsed.value().size();
            ++completed;
        }
        report_result("parse canonical document (validation included)", completed, elapsed_ms(start), checksum);
    }
    // Import into a fresh store as one transaction.
    {
        const std::filesystem::path target = work / ("import-" + std::to_string(count));
        std::filesystem::remove_all(target);
        StoreOpenOptions target_options;
        RecoveryReport target_report;
        auto registry = AssetRegistry::open(target, StoreOpenMode::CreateIfMissing, target_options, target_report);
        if (!registry) {
            std::fprintf(stderr, "import target open failed: %s\n", registry.error().to_string().c_str());
            std::exit(1);
        }
        auto session = registry.value()->open_writer(WriterId::create("benchmark-import").value());
        if (!session) {
            std::fprintf(stderr, "grant failed: %s\n", session.error().to_string().c_str());
            std::exit(1);
        }
        WriterSession writer = session.value();
        auto parsed = registry.value()->parse_import_document(document.value(), ImportOptions{});
        if (!parsed) {
            std::fprintf(stderr, "parse failed: %s\n", parsed.error().to_string().c_str());
            std::exit(1);
        }
        const SteadyClock::time_point start = SteadyClock::now();
        auto imported = registry.value()->import_assets(writer, writer.advance(), parsed.value(), ImportOptions{});
        const double milliseconds = elapsed_ms(start);
        if (!imported) {
            std::fprintf(stderr, "import failed: %s\n", imported.error().to_string().c_str());
            std::exit(1);
        }
        report_result("import as one atomic transaction (one commit)", imported.value().records_registered,
                      milliseconds, imported.value().committed_sequence.value());
        const ConflictReport conflicts = registry.value()->snapshot().audit_conflicts();
        if (!conflicts.clean()) {
            std::fprintf(stderr, "imported state is not consistent\n");
            std::exit(1);
        }
        auto closed = registry.value()->close();
        if (!closed) {
            std::fprintf(stderr, "close failed: %s\n", closed.error().to_string().c_str());
            std::exit(1);
        }
    }
}

void print_environment() {
    std::printf("Asset Registry benchmarks\n");
    std::printf("measurement: completed operations; durable commit cost included where the operation is durable\n");
    std::printf("store format version: %u, payload schema version: %u, export schema version: %u\n",
                kStoreFormatVersion, kPayloadSchemaVersion, kExportSchemaVersion);
    std::printf("default bounds: max_assets=%llu max_retained_generations=%u max_provenance_per_asset=%u\n",
                static_cast<unsigned long long>(default_limits().max_assets),
                default_limits().max_retained_generations, default_limits().max_provenance_per_asset);
    std::printf("evidence class: SYNTHETIC inventory records on this machine; no facility hardware is involved\n\n");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string scale_argument = argc > 1 ? argv[1] : "standard";
    std::vector<std::uint64_t> scales;
    if (scale_argument == "small") {
        scales = {100, 1000};
    } else if (scale_argument == "large") {
        scales = {1000, 10000, 100000};
    } else {
        scales = {100, 1000, 10000};
    }

    const std::filesystem::path work = std::filesystem::temp_directory_path() / "asset-registry-benchmarks";
    std::filesystem::remove_all(work);
    std::filesystem::create_directories(work);

    print_environment();
    const SteadyClock::time_point overall = SteadyClock::now();
    for (const std::uint64_t scale : scales) {
        benchmark_registration(work, scale);
        benchmark_reopen(work, scale);
        benchmark_queries(work, scale);
        benchmark_updates(work, scale, 200);
        benchmark_import(work, scale);
        std::printf("\n");
    }

    std::printf("summary (name, operations, ms, us/op, ops/s):\n");
    for (const Result& result : results()) {
        const double per_operation_us =
            result.operations == 0 ? 0.0 : result.milliseconds * 1000.0 / static_cast<double>(result.operations);
        const double per_second =
            result.milliseconds == 0.0 ? 0.0 : static_cast<double>(result.operations) * 1000.0 / result.milliseconds;
        std::printf("  %-46s %10llu %10.2f %9.2f %12.0f\n", result.name.c_str(),
                    static_cast<unsigned long long>(result.operations), result.milliseconds, per_operation_us,
                    per_second);
    }
    std::printf("total wall time: %.2f ms\n", elapsed_ms(overall));
    std::printf("benchmark residue: %s (removed by the harness conclusion)\n", work.string().c_str());

    std::error_code error;
    std::filesystem::remove_all(work, error);
    if (error) {
        std::fprintf(stderr, "warning: could not remove benchmark residue at %s: %s\n", work.string().c_str(),
                     error.message().c_str());
        return 1;
    }
    return 0;
}
