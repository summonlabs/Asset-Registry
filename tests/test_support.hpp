// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Shared helpers for the test suite: temporary directories, deterministic
// registries, and builders for the typed values the API requires.

#ifndef ASSET_REGISTRY_TEST_SUPPORT_HPP
#define ASSET_REGISTRY_TEST_SUPPORT_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_registry/asset_registry.hpp"

namespace asset_test {

/// A temporary directory that removes itself. Used instead of a fixed path so
/// concurrent test processes cannot collide and so a crashed run leaves nothing
/// behind in the source tree.
class TempDirectory {
public:
    explicit TempDirectory(std::string_view label);
    ~TempDirectory();
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path file(std::string_view name) const;

    /// Removes and recreates the directory, for a test that starts from nothing.
    void reset();

private:
    std::filesystem::path path_;
};

/// Deterministic clock: each call advances by a fixed step, so provenance
/// timestamps in a test are reproducible and never depend on wall time.
class SteppedClock final : public asset_registry::Clock {
public:
    explicit SteppedClock(std::int64_t start_unix_nanos = 1'700'000'000'000'000'000LL,
                          std::int64_t step_nanos = 1'000'000LL)
        : current_(start_unix_nanos), step_(step_nanos) {}

    [[nodiscard]] asset_registry::Timestamp now() const override {
        const std::int64_t value = current_;
        current_ += step_;
        return asset_registry::Timestamp(value);
    }

    [[nodiscard]] std::int64_t peek() const noexcept { return current_; }
    void advance(std::int64_t nanos) noexcept { current_ += nanos; }

private:
    mutable std::int64_t current_;
    std::int64_t step_;
};

/// A seeded, deterministic pseudo-random generator (xoshiro256**). The sequence
/// is reproducible from its seed and identical on every platform, which is what
/// makes a failing property test replayable.
class SeededRandom {
public:
    explicit SeededRandom(std::uint64_t seed) {
        // SplitMix64 expansion so that adjacent seeds produce unrelated streams.
        std::uint64_t state = seed;
        for (std::size_t index = 0; index < 4; ++index) {
            state += 0x9E3779B97F4A7C15ULL;
            std::uint64_t z = state;
            z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
            words_[index] = z ^ (z >> 31U);
        }
    }

    [[nodiscard]] std::uint64_t next() noexcept {
        const std::uint64_t result = rotate_left(words_[1] * 5U, 7U) * 9U;
        const std::uint64_t temporary = words_[1] << 17U;
        words_[2] ^= words_[0];
        words_[3] ^= words_[1];
        words_[1] ^= words_[2];
        words_[0] ^= words_[3];
        words_[2] ^= temporary;
        words_[3] = rotate_left(words_[3], 45U);
        return result;
    }

    /// Uniform value in [0, bound).
    [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept {
        return bound == 0 ? 0 : next() % bound;
    }

    [[nodiscard]] bool coin() noexcept { return (next() & 1U) == 1U; }

    [[nodiscard]] std::int64_t between(std::int64_t low, std::int64_t high) noexcept {
        const auto span = static_cast<std::uint64_t>(high - low + 1);
        return low + static_cast<std::int64_t>(below(span));
    }

private:
    [[nodiscard]] static std::uint64_t rotate_left(std::uint64_t value, unsigned amount) noexcept {
        return (value << amount) | (value >> (64U - amount));
    }

    std::uint64_t words_[4]{};
};

// --- Typed value builders ---------------------------------------------------

[[nodiscard]] asset_registry::WriterId writer(std::string_view name);
[[nodiscard]] asset_registry::OwnerId owner(std::string_view text);
[[nodiscard]] asset_registry::LocationId location(std::string_view text);
[[nodiscard]] asset_registry::RackId rack(std::string_view text);
[[nodiscard]] asset_registry::CapabilityReference capability(std::string_view text);
[[nodiscard]] asset_registry::SerialIdentity serial(std::string_view manufacturer, std::string_view number,
                                                    std::optional<std::string_view> model = std::nullopt);
[[nodiscard]] asset_registry::Reference capability_ref(std::string_view text,
                                                       asset_registry::ReferenceEvidence evidence =
                                                           asset_registry::ReferenceEvidence::Unverified);

/// A deterministic canonical identity derived from a label, so a test can name an
/// asset without hard-coding a hexadecimal constant.
[[nodiscard]] asset_registry::AssetId asset_id_for(std::string_view label);

/// A complete registration request with everything the API requires.
[[nodiscard]] asset_registry::RegisterAssetRequest make_request(std::string_view label,
                                                               asset_registry::AssetClass asset_class,
                                                               std::string_view manufacturer,
                                                               std::string_view serial_number);

/// An open durable registry plus an armed writer session, with the store open
/// transaction report attached.
struct OpenRegistry {
    std::shared_ptr<asset_registry::AssetRegistry> registry;
    asset_registry::WriterSession session;
    asset_registry::RecoveryReport report;
};

/// Opens or creates a store at `directory` and arms one writer. The optional
/// policy and bounds default to the documented defaults.
[[nodiscard]] OpenRegistry open_registry(const std::filesystem::path& directory,
                                         const asset_registry::RegistryPolicy& policy =
                                             asset_registry::default_policy(),
                                         const asset_registry::RegistryLimits& bounds =
                                             asset_registry::default_limits(),
                                         std::string_view writer_name = "test-writer");

/// A detached in-memory registry plus an armed writer.
[[nodiscard]] OpenRegistry detached_registry(const asset_registry::RegistryPolicy& policy =
                                                 asset_registry::default_policy(),
                                             const asset_registry::RegistryLimits& bounds =
                                                 asset_registry::default_limits(),
                                             std::string_view writer_name = {});

/// Reads an entire file, or an empty string when it cannot be read.
[[nodiscard]] std::string read_text_file(const std::filesystem::path& path);

/// Writes text to a file, creating parent directories.
void write_text_file(const std::filesystem::path& path, std::string_view text);

/// True when the path exists. Named to avoid colliding with
/// std::filesystem::exists when a test brings both names into scope.
[[nodiscard]] bool path_exists(const std::filesystem::path& path);

/// Lists the entries directly inside a directory, sorted ascending.
[[nodiscard]] std::vector<std::string> list_directory(const std::filesystem::path& path);

}  // namespace asset_test

#endif  // ASSET_REGISTRY_TEST_SUPPORT_HPP
