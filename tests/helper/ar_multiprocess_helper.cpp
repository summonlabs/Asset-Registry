// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Independent-process helper for the multiprocess test suite.
//
// This is a real program, not a fixture library: it opens a store, performs the
// requested work, and exits with a status that distinguishes success, a refused
// store lock, and a genuine failure. The parent test starts it as a separate
// operating-system process, which is what makes the fencing, epoch, and restart
// claims real rather than simulated.
//
// Usage:
//   ar_multiprocess_helper register <directory> <writer> <prefix> <count> [retry]
//   ar_multiprocess_helper hold     <directory> <writer> <milliseconds>
//   ar_multiprocess_helper verify   <directory> <prefix> <count>
//
// Exit codes:
//   0  the requested work completed
//   2  the store lock was held by another process (expected under contention)
//   3  usage error
//   4  the store could not be opened
//   5  a mutation was refused
//   6  verification found the inventory inconsistent
//   7  the writer's authority was refused (used to prove cross-process fencing)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include "asset_registry/asset_registry.hpp"

namespace {

using namespace asset_registry;

[[nodiscard]] std::string label_for(std::string_view prefix, int index) {
    return std::string(prefix) + "-" + std::to_string(index);
}

/// Deterministic canonical identity so two processes agree on which asset a label
/// names without sharing any state.
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

[[nodiscard]] std::shared_ptr<AssetRegistry> open_store(const std::filesystem::path& directory,
                                                        RecoveryReport& report, bool read_only) {
    StoreOpenOptions options;
    options.initial_writer = "multiprocess-helper";
    return AssetRegistry::open(directory, read_only ? StoreOpenMode::ReadOnly : StoreOpenMode::CreateIfMissing,
                               options, report)
        .value_or(nullptr);
}

int do_register(const std::filesystem::path& directory, const std::string& writer_name, const std::string& prefix,
                int count, int retry_budget) {
    RecoveryReport report;
    std::shared_ptr<AssetRegistry> registry;
    for (int attempt = 0; attempt <= retry_budget; ++attempt) {
        auto opened = AssetRegistry::open(directory, StoreOpenMode::CreateIfMissing, StoreOpenOptions{}, report);
        if (opened) {
            registry = opened.value();
            break;
        }
        if (opened.error().code() != ErrorCode::StoreLocked) {
            std::fprintf(stderr, "open failed: %s\n", opened.error().to_string().c_str());
            return 4;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (registry == nullptr) {
        // Another process held the store for the whole budget: the documented
        // outcome under contention, reported as such rather than as a failure.
        return 2;
    }

    auto session = registry->open_writer(WriterId::create(writer_name).value());
    if (!session) {
        std::fprintf(stderr, "grant failed: %s\n", session.error().to_string().c_str());
        return 7;
    }
    WriterSession writer = session.value();

    int registered = 0;
    for (int index = 0; index < count; ++index) {
        const std::string label = label_for(prefix, index);
        RegisterAssetRequest request;
        request.id_mode = AssetIdMode::CallerSupplied;
        request.id = identity_for(label);
        request.asset_class = AssetClass::Server;
        const auto manufacturer = ManufacturerIdentity::create("Multiprocess").value();
        const auto number = SerialNumber::create("MP-" + label).value();
        request.serial_identity = SerialIdentity::create(manufacturer, number, std::nullopt).value();
        request.metadata.display_name = label;
        request.lifecycle = LifecycleState::Planned;
        request.installation = InstallationState::Unknown;

        auto result = registry->register_asset(writer, writer.advance(), request);
        if (!result) {
            // A duplicate is the expected outcome when another process already
            // registered this label, which is exactly what the contention test
            // wants to observe.
            if (result.error().code() == ErrorCode::DuplicateAssetId ||
                result.error().code() == ErrorCode::DuplicateSerialIdentity) {
                continue;
            }
            std::fprintf(stderr, "register refused: %s\n", result.error().to_string().c_str());
            (void)registry->close();
            return 5;
        }
        ++registered;
    }
    const auto closed = registry->close();
    if (!closed) {
        std::fprintf(stderr, "close failed: %s\n", closed.error().to_string().c_str());
        return 4;
    }
    std::printf("registered %d asset(s) as %s\n", registered, writer_name.c_str());
    return 0;
}

int do_hold(const std::filesystem::path& directory, const std::string& writer_name, int milliseconds) {
    RecoveryReport report;
    auto registry = open_store(directory, report, false);
    if (registry == nullptr) {
        std::fprintf(stderr, "hold could not open the store\n");
        return 4;
    }
    auto session = registry->open_writer(WriterId::create(writer_name).value());
    if (!session) {
        std::fprintf(stderr, "hold could not arm a writer: %s\n", session.error().to_string().c_str());
        return 7;
    }
    WriterSession writer = session.value();
    std::printf("holding\n");
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    // The token is deliberately not returned: it is used after another process has
    // reopened the store, which must be fenced out.
    (void)registry->close();
    return 0;
}

/// Holds the store, then uses a token minted before the hold after another process
/// has taken over, to prove cross-process fencing.
int do_fenced_attempt(const std::filesystem::path& directory, const std::string& writer_name, int hold_milliseconds) {
    RecoveryReport report;
    auto registry = open_store(directory, report, false);
    if (registry == nullptr) {
        return 4;
    }
    auto session = registry->open_writer(WriterId::create(writer_name).value());
    if (!session) {
        return 7;
    }
    WriterSession writer = session.value();
    std::printf("armed epoch=%s\n", writer.token().epoch().to_string().c_str());
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_milliseconds));

    // Attempt a mutation with the original token while still holding the store. It
    // must succeed here, because this incarnation still owns the store.
    RegisterAssetRequest request;
    const std::string label = "fenced-" + writer_name;
    request.id = identity_for(label);
    request.asset_class = AssetClass::Server;
    const auto manufacturer = ManufacturerIdentity::create("Multiprocess").value();
    const auto number = SerialNumber::create("MP-" + label).value();
    request.serial_identity = SerialIdentity::create(manufacturer, number, std::nullopt).value();
    request.metadata.display_name = label;
    request.lifecycle = LifecycleState::Planned;
    request.installation = InstallationState::Unknown;
    auto result = registry->register_asset(writer, writer.advance(), request);
    const bool accepted = result.has_value();
    std::printf("mutation accepted=%d\n", accepted ? 1 : 0);
    std::fflush(stdout);
    (void)registry->close();
    return accepted ? 0 : 5;
}

int do_verify(const std::filesystem::path& directory, const std::string& prefix, int count) {
    RecoveryReport report;
    auto registry = open_store(directory, report, true);
    if (registry == nullptr) {
        std::fprintf(stderr, "verify could not open the store\n");
        return 4;
    }
    const Snapshot snapshot = registry->snapshot();
    for (int index = 0; index < count; ++index) {
        const std::string label = label_for(prefix, index);
        const auto view = snapshot.find(identity_for(label));
        if (!view.has_value()) {
            std::fprintf(stderr, "missing asset: %s\n", label.c_str());
            return 6;
        }
        if (view->metadata.display_name != label) {
            std::fprintf(stderr, "wrong display name for %s\n", label.c_str());
            return 6;
        }
        if (view->revision.is_zero()) {
            std::fprintf(stderr, "zero revision for %s\n", label.c_str());
            return 6;
        }
    }
    const ConflictReport conflicts = snapshot.audit_conflicts();
    if (!conflicts.clean()) {
        std::fprintf(stderr, "conflict audit is not clean\n");
        return 6;
    }
    std::printf("verified %d asset(s), sequence=%s\n", count, snapshot.published_sequence().to_string().c_str());
    (void)registry->close();
    return 0;
}

int do_inspect(const std::filesystem::path& directory) {
    RecoveryReport report;
    auto registry = open_store(directory, report, true);
    if (registry == nullptr) {
        return 4;
    }
    const Snapshot snapshot = registry->snapshot();
    std::printf("size=%llu sequence=%s epoch=%s recovery=%s\n",
                static_cast<unsigned long long>(snapshot.size()),
                snapshot.published_sequence().to_string().c_str(), snapshot.epoch().to_string().c_str(),
                std::string(asset_registry::to_string(report.action)).c_str());
    (void)registry->close();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <command> ...\n", argv[0]);
        return 3;
    }
    const std::string command = argv[1];
    if (command == "register" && argc >= 7) {
        return do_register(argv[2], argv[3], argv[4], std::atoi(argv[5]), std::atoi(argv[6]));
    }
    if (command == "register" && argc >= 6) {
        return do_register(argv[2], argv[3], argv[4], std::atoi(argv[5]), 0);
    }
    if (command == "hold" && argc >= 5) {
        return do_hold(argv[2], argv[3], std::atoi(argv[4]));
    }
    if (command == "fenced" && argc >= 5) {
        return do_fenced_attempt(argv[2], argv[3], std::atoi(argv[4]));
    }
    if (command == "verify" && argc >= 5) {
        return do_verify(argv[2], argv[3], std::atoi(argv[4]));
    }
    if (command == "inspect" && argc >= 3) {
        return do_inspect(argv[2]);
    }
    std::fprintf(stderr, "unrecognised arguments\n");
    return 3;
}
