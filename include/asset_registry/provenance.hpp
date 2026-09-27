// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Provenance and time.
//
// Every authoritative change to an asset record appends a ProvenanceStep. The
// step records what changed, who changed it, under which authority epoch, and
// when. Timestamps are attributed evidence, never trusted ordering: ordering is
// carried by monotonic sequences so that a clock adjustment, a skewed remote
// writer, or an import of historical records can never reorder authority.

#ifndef ASSET_REGISTRY_PROVENANCE_HPP
#define ASSET_REGISTRY_PROVENANCE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_registry/error.hpp"
#include "asset_registry/reference.hpp"
#include "asset_registry/strong_types.hpp"

namespace asset_registry {

namespace limits {
inline constexpr std::size_t kMaxReasonBytes = 256;
inline constexpr std::size_t kMaxTimestampBytes = 32;
}  // namespace limits

/// Signed nanoseconds since the Unix epoch, UTC. Negative values are legitimate
/// pre-1970 evidence from imported historical records.
class ASSET_REGISTRY_API Timestamp {
public:
    Timestamp() = default;
    explicit Timestamp(std::int64_t unix_nanos) noexcept : unix_nanos_(unix_nanos) {}

    /// The range accepted from external input.
    ///
    /// The representation is signed 64-bit nanoseconds, so the span covered is
    /// 1677-09-21T00:12:43.145224192Z .. 2262-04-11T23:47:16.854775807Z. Those
    /// bounds are the representable limits of the type, not arbitrary policy:
    /// nanosecond resolution cannot express a wider range in 64 signed bits. A
    /// timestamp outside them is rejected rather than clamped, because a clamped
    /// timestamp is fabricated evidence. The two constants are written as literal
    /// values so that no intermediate expression can overflow, and the maximum is
    /// one nanosecond below the type's limit so that the value is always usable in
    /// arithmetic that adds a positive offset.
    static constexpr std::int64_t kMinUnixNanos = (-9223372036854775807LL - 1LL);
    static constexpr std::int64_t kMaxUnixNanos = 9223372036854775806LL;

    [[nodiscard]] static std::optional<Timestamp> create(std::int64_t unix_nanos) noexcept;

    /// Parses a strict RFC 3339 UTC timestamp with second or nanosecond
    /// precision: "YYYY-MM-DDTHH:MM:SSZ" or "YYYY-MM-DDTHH:MM:SS.fffffffffZ".
    /// Offsets other than Z are rejected so that the in-memory value is
    /// unambiguously UTC.
    [[nodiscard]] static std::optional<Timestamp> parse_rfc3339(std::string_view text) noexcept;

    [[nodiscard]] std::int64_t unix_nanos() const noexcept { return unix_nanos_; }
    [[nodiscard]] std::string to_rfc3339() const;

    friend bool operator==(const Timestamp& lhs, const Timestamp& rhs) noexcept {
        return lhs.unix_nanos_ == rhs.unix_nanos_;
    }
    friend bool operator!=(const Timestamp& lhs, const Timestamp& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const Timestamp& lhs, const Timestamp& rhs) noexcept {
        return lhs.unix_nanos_ < rhs.unix_nanos_;
    }

private:
    std::int64_t unix_nanos_ = 0;
};

/// Injectable time source. The default reads the system UTC clock. Tests inject
/// a fixed or stepped clock so that provenance assertions are reproducible.
class ASSET_REGISTRY_API Clock {
public:
    Clock() = default;
    virtual ~Clock();
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;

    [[nodiscard]] virtual Timestamp now() const = 0;
};

/// System UTC clock. Never returns a value outside the representable range; on a
/// system clock set beyond year 9999 it saturates at the maximum representable
/// instant rather than overflowing.
class ASSET_REGISTRY_API SystemClock final : public Clock {
public:
    [[nodiscard]] Timestamp now() const override;
};

/// How a change came to exist.
enum class ProvenanceOrigin : std::uint8_t {
    /// Applied through the mutation API by an authorised writer.
    ApiMutation = 0,
    /// Created by importing an external inventory document.
    Import = 1,
    /// Created by store recovery after an interrupted commit.
    Recovery = 2,
    /// Created by an explicit identity-reuse decision.
    IdentityReuse = 3,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(ProvenanceOrigin value) noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::optional<ProvenanceOrigin> provenance_origin_from_token(
    std::string_view token) noexcept;

/// Who or what performed a change. A recovery step has no writer and is
/// attributed to the registry store itself so that no operator is blamed for a
/// system action.
class ASSET_REGISTRY_API ActorRef {
public:
    ActorRef() = default;

    [[nodiscard]] static ActorRef writer(WriterId id);
    [[nodiscard]] static ActorRef system();

    [[nodiscard]] bool is_writer() const noexcept { return !writer_.empty(); }
    [[nodiscard]] bool is_system() const noexcept { return writer_.empty(); }
    /// The acting writer. Empty when the actor is the registry system.
    [[nodiscard]] const WriterId& writer_id() const noexcept { return writer_; }
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const ActorRef& lhs, const ActorRef& rhs) noexcept { return lhs.writer_ == rhs.writer_; }
    friend bool operator!=(const ActorRef& lhs, const ActorRef& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const ActorRef& lhs, const ActorRef& rhs) noexcept { return lhs.writer_ < rhs.writer_; }

private:
    WriterId writer_;
};

/// What a provenance step did.
enum class ProvenanceAction : std::uint8_t {
    Registered = 0,
    MetadataUpdated = 1,
    LifecycleTransitioned = 2,
    InstallationTransitioned = 3,
    ReferenceAttached = 4,
    ReferenceDetached = 5,
    ReferenceEvidenceChanged = 6,
    OwnerChanged = 7,
    PlacementChanged = 8,
    ReplacementLinked = 9,
    IdentityReused = 10,
    SerialIdentityUpdated = 11,
    ClassReclassified = 12,
    StateDeclared = 13,
    RecoveredFromStore = 14,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(ProvenanceAction value) noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::optional<ProvenanceAction> provenance_action_from_token(
    std::string_view token) noexcept;

/// One immutable step in an asset's authoritative history.
struct ASSET_REGISTRY_API ProvenanceStep {
    /// Global transaction sequence of the commit that published this step.
    /// Strictly increasing across the whole store, so ordering is total and
    /// independent of any clock.
    TransactionSequence sequence;

    /// Store epoch in force when the step was published. A step with a lower
    /// epoch than the current one was published by an earlier incarnation of the
    /// store and is historical evidence, not fresh.
    RegistryEpoch epoch;

    /// Asset revision produced by this step.
    AssetRevision revision;

    ProvenanceAction action = ProvenanceAction::Registered;
    ProvenanceOrigin origin = ProvenanceOrigin::ApiMutation;
    ActorRef actor;

    /// When the change happened, as reported by the configured clock. Absent
    /// for imported records that carried no usable timestamp; an absent
    /// timestamp is not replaced with the import time, because that would
    /// fabricate evidence.
    std::optional<Timestamp> recorded_at;

    /// Free-form operator explanation, bounded and validated.
    std::string reason;

    /// Machine-readable before/after rendering of the changed field, e.g.
    /// "lifecycle=active->maintenance". Empty when the step has no scalar
    /// change to describe.
    std::string change;
};

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_PROVENANCE_HPP
