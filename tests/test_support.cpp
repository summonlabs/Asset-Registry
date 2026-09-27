// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace asset_test {
namespace {

std::atomic<std::uint64_t> g_directory_counter{0};

[[nodiscard]] std::filesystem::path temp_root() {
    std::error_code error;
    std::filesystem::path root = std::filesystem::temp_directory_path(error);
    if (error) {
        root = std::filesystem::current_path();
    }
    return root / "asset-registry-tests";
}

/// Deterministic 128-bit identity derived from a label. Uses the same construction
/// the registry uses for derived identities so the helper stays honest about what
/// a canonical identity looks like.
[[nodiscard]] asset_registry::AssetId hash_identity(std::string_view label) {
    asset_registry::AssetId::bytes_type bytes{};
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
    // Version and variant bits, so the value is a well-formed RFC 9562 identifier
    // rather than an arbitrary 128-bit number.
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
    return asset_registry::AssetId::from_bytes(bytes);
}

}  // namespace

TempDirectory::TempDirectory(std::string_view label) {
    const std::uint64_t serial = g_directory_counter.fetch_add(1, std::memory_order_relaxed);
    path_ = temp_root() / (std::string(label) + "-" + std::to_string(serial));
    reset();
}

TempDirectory::~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

std::filesystem::path TempDirectory::file(std::string_view name) const {
    return path_ / std::string(name);
}

void TempDirectory::reset() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    std::filesystem::create_directories(path_, error);
}

asset_registry::WriterId writer(std::string_view name) {
    const auto parsed = asset_registry::WriterId::create(name);
    if (parsed.has_value()) {
        return *parsed;
    }
    return asset_registry::WriterId::create("writer-fallback").value();
}

asset_registry::OwnerId owner(std::string_view text) {
    return asset_registry::OwnerId::create(text).value();
}

asset_registry::LocationId location(std::string_view text) {
    return asset_registry::LocationId::create(text).value();
}

asset_registry::RackId rack(std::string_view text) {
    return asset_registry::RackId::create(text).value();
}

asset_registry::CapabilityReference capability(std::string_view text) {
    return asset_registry::CapabilityReference::create(text).value();
}

asset_registry::SerialIdentity serial(std::string_view manufacturer, std::string_view number,
                                      std::optional<std::string_view> model) {
    const auto manufacturer_id = asset_registry::ManufacturerIdentity::create(manufacturer).value();
    const auto serial_id = asset_registry::SerialNumber::create(number).value();
    std::optional<asset_registry::ModelIdentity> model_id;
    if (model.has_value()) {
        model_id = asset_registry::ModelIdentity::create(*model).value();
    }
    return asset_registry::SerialIdentity::create(manufacturer_id, serial_id, model_id).value();
}

asset_registry::Reference capability_ref(std::string_view text, asset_registry::ReferenceEvidence evidence) {
    return asset_registry::Reference::capability(capability(text), evidence);
}

asset_registry::AssetId asset_id_for(std::string_view label) {
    return hash_identity(label);
}

asset_registry::RegisterAssetRequest make_request(std::string_view label,
                                                 asset_registry::AssetClass asset_class,
                                                 std::string_view manufacturer,
                                                 std::string_view serial_number) {
    asset_registry::RegisterAssetRequest request;
    request.id_mode = asset_registry::AssetIdMode::CallerSupplied;
    request.id = asset_id_for(label);
    request.asset_class = asset_class;
    request.serial_identity = serial(manufacturer, serial_number);
    request.metadata.display_name = std::string(label);
    request.lifecycle = asset_registry::LifecycleState::Planned;
    request.installation = asset_registry::InstallationState::Unknown;
    return request;
}

OpenRegistry open_registry(const std::filesystem::path& directory, const asset_registry::RegistryPolicy& policy,
                           const asset_registry::RegistryLimits& bounds, std::string_view writer_name) {
    asset_registry::StoreOpenOptions options;
    options.policy = policy;
    options.limits = bounds;
    options.initial_writer = std::string(writer_name);
    OpenRegistry result;
    auto opened = asset_registry::AssetRegistry::open(directory, asset_registry::StoreOpenMode::CreateIfMissing,
                                                     options, result.report);
    if (!opened) {
        std::fprintf(stderr, "open_registry failed: %s\n", opened.error().to_string().c_str());
        std::abort();
    }
    result.registry = opened.value();
    auto session = result.registry->open_writer(writer(writer_name));
    if (!session) {
        std::fprintf(stderr, "open_writer failed: %s\n", session.error().to_string().c_str());
        std::abort();
    }
    result.session = session.value();
    return result;
}

OpenRegistry detached_registry(const asset_registry::RegistryPolicy& policy,
                               const asset_registry::RegistryLimits& bounds, std::string_view writer_name) {
    OpenRegistry result;
    // Each detached registry gets its own writer identity unless one is named, so
    // a case cannot inherit the mutation-sequence history of another case that
    // happened to use the same default name.
    static std::atomic<std::uint64_t> counter{0};
    std::string resolved(writer_name);
    if (resolved.empty()) {
        resolved = "detached-writer-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    }
    auto created = asset_registry::AssetRegistry::create_detached(policy, bounds);
    if (!created) {
        std::fprintf(stderr, "create_detached failed: %s\n", created.error().to_string().c_str());
        std::abort();
    }
    result.registry = created.value();
    auto session = result.registry->open_writer(writer(resolved));
    if (!session) {
        std::fprintf(stderr, "open_writer failed: %s\n", session.error().to_string().c_str());
        std::abort();
    }
    result.session = session.value();
    return result;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

void write_text_file(const std::filesystem::path& path, std::string_view text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

std::vector<std::string> list_directory(const std::filesystem::path& path) {
    std::vector<std::string> names;
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        return names;
    }
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(path, error)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace asset_test
