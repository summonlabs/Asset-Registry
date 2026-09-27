// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Strongly typed scalars. None of these types convert implicitly to any other,
// and none of them expose an "invalid" sentinel: absence is modelled with
// std::optional at the call site.

#ifndef ASSET_REGISTRY_STRONG_TYPES_HPP
#define ASSET_REGISTRY_STRONG_TYPES_HPP

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "asset_registry/error.hpp"

namespace asset_registry {

namespace detail {

/// Common implementation for a strictly monotonic unsigned counter.
/// The counter type name is carried only for diagnostics; arithmetic between
/// different counter types is a compile error because the types differ.
template <typename Tag, typename Rep>
class Counter {
public:
    using rep_type = Rep;

    constexpr Counter() noexcept = default;
    constexpr explicit Counter(Rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }

    /// True for the value a default-constructed counter holds.
    [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

    [[nodiscard]] constexpr bool operator==(const Counter& other) const noexcept {
        return value_ == other.value_;
    }
    [[nodiscard]] constexpr bool operator!=(const Counter& other) const noexcept {
        return value_ != other.value_;
    }
    [[nodiscard]] constexpr bool operator<(const Counter& other) const noexcept {
        return value_ < other.value_;
    }
    [[nodiscard]] constexpr bool operator<=(const Counter& other) const noexcept {
        return value_ <= other.value_;
    }
    [[nodiscard]] constexpr bool operator>(const Counter& other) const noexcept {
        return value_ > other.value_;
    }
    [[nodiscard]] constexpr bool operator>=(const Counter& other) const noexcept {
        return value_ >= other.value_;
    }

    /// Strictly increasing successor. Returns std::nullopt instead of wrapping
    /// when the representation is exhausted, so callers can never observe a
    /// counter that silently restarted at zero.
    [[nodiscard]] constexpr std::optional<Counter> next() const noexcept {
        if (value_ == std::numeric_limits<Rep>::max()) {
            return std::nullopt;
        }
        return Counter(static_cast<Rep>(value_ + 1));
    }

    /// Saturating subtraction; the result is never negative.
    [[nodiscard]] constexpr Rep distance_from(const Counter& earlier) const noexcept {
        return value_ >= earlier.value_ ? static_cast<Rep>(value_ - earlier.value_) : Rep{0};
    }

    /// Parses a decimal representation. Leading zeros beyond a single "0",
    /// signs, whitespace and any non-digit character are rejected.
    [[nodiscard]] static std::optional<Counter> parse(std::string_view text) noexcept {
        if (text.empty() || text.size() > 20) {
            return std::nullopt;
        }
        if (text.size() > 1 && text.front() == '0') {
            return std::nullopt;
        }
        Rep value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            const Rep digit = static_cast<Rep>(c - '0');
            if (value > (std::numeric_limits<Rep>::max() - digit) / 10) {
                return std::nullopt;
            }
            value = static_cast<Rep>(value * 10 + digit);
        }
        return Counter(value);
    }

    [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

private:
    Rep value_ = 0;
};

}  // namespace detail

/// Number of mutations a single asset handle has accepted. Starts at 1 for a
/// freshly registered asset and advances by exactly one per published mutation.
/// Revision 0 means "not yet registered" and is never observable on a record.
struct AssetRevisionTag;
using AssetRevision = detail::Counter<AssetRevisionTag, std::uint64_t>;

/// Incarnation number of a canonical AssetId. Generation 1 is the asset's first
/// incarnation. A later generation exists only when the identity-reuse policy
/// explicitly permits reuse after retirement; the previous incarnation is
/// preserved in history and bounded by the configured history limit.
struct AssetGenerationTag;
using AssetGeneration = detail::Counter<AssetGenerationTag, std::uint32_t>;

/// Epoch of the registry store. Every successful open of a store publishes a
/// strictly greater epoch. Any writer holding a token minted under an earlier
/// epoch is fenced out, including across process restarts.
struct RegistryEpochTag;
using RegistryEpoch = detail::Counter<RegistryEpochTag, std::uint64_t>;

/// Monotonic counter identifying a transaction identity reserved against the
/// durable store. A published generation always carries a higher transaction
/// sequence than the generation it superseded.
struct TransactionSequenceTag;
using TransactionSequence = detail::Counter<TransactionSequenceTag, std::uint64_t>;

/// Per-writer mutation counter. A writer must advance its sequence for every
/// attempt; a repeated sequence with a matching idempotency key replays the
/// recorded outcome, and a repeated sequence without one is rejected.
struct MutationSequenceTag;
using MutationSequence = detail::Counter<MutationSequenceTag, std::uint64_t>;

/// Lifetime counter for authority grants issued by one open store. Grant 1 is
/// the first token minted in an epoch.
struct AuthorityGenerationTag;
using AuthorityGeneration = detail::Counter<AuthorityGenerationTag, std::uint64_t>;

/// Format version of the durable store layout.
struct StoreFormatVersionTag;
using StoreFormatVersion = detail::Counter<StoreFormatVersionTag, std::uint32_t>;

/// Schema version of the canonical export document.
struct ExportSchemaVersionTag;
using ExportSchemaVersion = detail::Counter<ExportSchemaVersionTag, std::uint32_t>;

/// Byte offset inside a durable artefact, used by codecs to bound reads.
struct ByteOffsetTag;
using ByteOffset = detail::Counter<ByteOffsetTag, std::uint64_t>;

/// A validated canonical asset identity: 128 bits rendered as lowercase
/// hyphenated hexadecimal "8-4-4-4-12". The value is opaque; callers must not
/// interpret bit patterns.
class ASSET_REGISTRY_API AssetId {
public:
    using bytes_type = std::array<std::uint8_t, 16>;

    /// The all-zero identifier. This is a real value for "the nil identifier",
    /// not a sentinel for "no identifier"; APIs use std::optional<AssetId> when
    /// absence is meaningful.
    [[nodiscard]] static const AssetId& nil() noexcept;

    /// Generates a version-4 (random) identifier from the process CSPRNG.
    /// Reports AllocationFailed when the operating system entropy source is
    /// unavailable, rather than substituting a predictable value.
    [[nodiscard]] static Outcome<AssetId> generate();

    /// Generates an identifier without reporting entropy failure; returns the nil
    /// identifier in that case. Provided for diagnostics and for tests that do not
    /// care whether entropy was available. Product paths use generate().
    [[nodiscard]] static AssetId generate_unchecked() noexcept;

    /// Builds an identifier from 16 bytes.
    [[nodiscard]] static AssetId from_bytes(const bytes_type& bytes) noexcept;

    /// Parses the canonical lowercase hyphenated form. Returns std::nullopt for
    /// any other spelling, including uppercase, braces, URN prefixes, and
    /// unhyphenated forms.
    [[nodiscard]] static std::optional<AssetId> parse(std::string_view text) noexcept;

    [[nodiscard]] const bytes_type& bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool is_nil() const noexcept;
    [[nodiscard]] std::string to_string() const;
    /// Fixed-width hex without hyphens, for compact ordering keys and indexes.
    [[nodiscard]] std::string to_compact_string() const;

    friend bool operator==(const AssetId& lhs, const AssetId& rhs) noexcept {
        return lhs.bytes_ == rhs.bytes_;
    }
    friend bool operator!=(const AssetId& lhs, const AssetId& rhs) noexcept { return !(lhs == rhs); }
    /// Total order by raw byte sequence, most significant byte first. This is
    /// the single canonical iteration order for every public enumeration.
    friend bool operator<(const AssetId& lhs, const AssetId& rhs) noexcept {
        return lhs.bytes_ < rhs.bytes_;
    }

private:
    bytes_type bytes_{};
};

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_STRONG_TYPES_HPP
