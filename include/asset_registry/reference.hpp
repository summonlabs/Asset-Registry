// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Ownership, writer, and reference identifiers.
//
// Everything in this header names an object owned by a different system. The
// Asset Registry stores the identifier, orders it deterministically, and treats
// its meaning as owned elsewhere. None of these types convert to another.

#ifndef ASSET_REGISTRY_REFERENCE_HPP
#define ASSET_REGISTRY_REFERENCE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_registry/capability.hpp"
#include "asset_registry/error.hpp"

namespace asset_registry {

namespace limits {
inline constexpr std::size_t kMaxOwnerIdBytes = 128;
inline constexpr std::size_t kMaxWriterIdBytes = 128;
inline constexpr std::size_t kMaxLocationIdBytes = 192;
inline constexpr std::size_t kMaxRackIdBytes = 192;
inline constexpr std::size_t kMaxExternalObjectIdBytes = 192;
inline constexpr std::size_t kMaxExternalObjectKindBytes = 48;
}  // namespace limits

/// Opaque organisational owner identifier, e.g. "org.summonlabs.platform".
/// Ownership is a reference into a tenancy/ownership system owned elsewhere;
/// the Asset Registry does not evaluate ownership policy.
class ASSET_REGISTRY_API OwnerId {
public:
    OwnerId() = default;

    [[nodiscard]] static std::optional<OwnerId> create(std::string_view text);

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

    friend bool operator==(const OwnerId& lhs, const OwnerId& rhs) noexcept { return lhs.text_ == rhs.text_; }
    friend bool operator!=(const OwnerId& lhs, const OwnerId& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const OwnerId& lhs, const OwnerId& rhs) noexcept { return lhs.text_ < rhs.text_; }

private:
    std::string text_;
};

/// Identity of a writer that may hold mutation authority over a store.
class ASSET_REGISTRY_API WriterId {
public:
    WriterId() = default;

    [[nodiscard]] static std::optional<WriterId> create(std::string_view text);

    /// Convenience constructor for a locally generated writer identity.
    [[nodiscard]] static WriterId generate(std::string_view prefix);

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

    friend bool operator==(const WriterId& lhs, const WriterId& rhs) noexcept { return lhs.text_ == rhs.text_; }
    friend bool operator!=(const WriterId& lhs, const WriterId& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const WriterId& lhs, const WriterId& rhs) noexcept { return lhs.text_ < rhs.text_; }

private:
    std::string text_;
};

/// Opaque physical-location identifier owned by the facility topology
/// repository, e.g. "site-a.hall-2.room-4".
class ASSET_REGISTRY_API LocationId {
public:
    LocationId() = default;

    [[nodiscard]] static std::optional<LocationId> create(std::string_view text);

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

    friend bool operator==(const LocationId& lhs, const LocationId& rhs) noexcept { return lhs.text_ == rhs.text_; }
    friend bool operator!=(const LocationId& lhs, const LocationId& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const LocationId& lhs, const LocationId& rhs) noexcept { return lhs.text_ < rhs.text_; }

private:
    std::string text_;
};

/// Opaque rack identifier owned by the facility topology repository.
class ASSET_REGISTRY_API RackId {
public:
    RackId() = default;

    [[nodiscard]] static std::optional<RackId> create(std::string_view text);

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

    friend bool operator==(const RackId& lhs, const RackId& rhs) noexcept { return lhs.text_ == rhs.text_; }
    friend bool operator!=(const RackId& lhs, const RackId& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const RackId& lhs, const RackId& rhs) noexcept { return lhs.text_ < rhs.text_; }

private:
    std::string text_;
};

/// Half-open rack-unit span inside a rack, expressed in whole rack units with a
/// bounded height. The registry records placement evidence; it does not compute
/// occupancy or resolve conflicts, because rack occupancy belongs to the
/// facility topology repository.
class ASSET_REGISTRY_API UnitSpan {
public:
    UnitSpan() = default;

    /// Maximum rack unit index this registry will represent. Real facility racks
    /// are far below this; the bound exists so that externally supplied spans
    /// cannot request absurd arithmetic.
    static constexpr std::uint32_t kMaxRackUnit = 512;

    /// Low unit is inclusive, high unit is exclusive. Requires lower >= 1,
    /// high <= kMaxRackUnit + 1, and lower < high.
    [[nodiscard]] static std::optional<UnitSpan> create(std::uint32_t low, std::uint32_t high) noexcept;

    /// Parses "low-high" (both decimal, low < high) or "unit" for a single unit.
    [[nodiscard]] static std::optional<UnitSpan> parse(std::string_view text) noexcept;

    [[nodiscard]] std::uint32_t low() const noexcept { return low_; }
    [[nodiscard]] std::uint32_t high() const noexcept { return high_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return high_ - low_; }
    [[nodiscard]] bool empty() const noexcept { return high_ <= low_; }

    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const UnitSpan& lhs, const UnitSpan& rhs) noexcept {
        return lhs.low_ == rhs.low_ && lhs.high_ == rhs.high_;
    }
    friend bool operator!=(const UnitSpan& lhs, const UnitSpan& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const UnitSpan& lhs, const UnitSpan& rhs) noexcept {
        return lhs.low_ != rhs.low_ ? lhs.low_ < rhs.low_ : lhs.high_ < rhs.high_;
    }

private:
    std::uint32_t low_ = 0;
    std::uint32_t high_ = 0;
};

/// Reference to an object owned by another DCCP repository (an ASI accelerator
/// pool, a DFI fabric path, a firmware baseline, a maintenance plan). The
/// `kind` is a lowercase-hyphen token naming the owning repository's object
/// family. The registry never interprets `id`.
class ASSET_REGISTRY_API ExternalObjectReference {
public:
    ExternalObjectReference() = default;

    [[nodiscard]] static std::optional<ExternalObjectReference> create(std::string_view kind, std::string_view id);

    [[nodiscard]] const std::string& kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] bool empty() const noexcept { return kind_.empty(); }

    friend bool operator==(const ExternalObjectReference& lhs, const ExternalObjectReference& rhs) noexcept {
        return lhs.kind_ == rhs.kind_ && lhs.id_ == rhs.id_;
    }
    friend bool operator!=(const ExternalObjectReference& lhs, const ExternalObjectReference& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend bool operator<(const ExternalObjectReference& lhs, const ExternalObjectReference& rhs) noexcept {
        return lhs.kind_ != rhs.kind_ ? lhs.kind_ < rhs.kind_ : lhs.id_ < rhs.id_;
    }

private:
    std::string kind_;
    std::string id_;
};

/// Evidence state of a stored reference.
///
/// The distinction matters operationally: an Unverified reference is a claim
/// carried in from an import that this registry has not confirmed against the
/// owning repository, and consumers must not treat it as authorisation.
enum class ReferenceEvidence : std::uint8_t {
    /// Recorded from an import or an unconfirmed external claim.
    Unverified = 0,
    /// The owning repository confirmed the target exists.
    Verified = 1,
    /// A previous verification is no longer known to be current.
    Stale = 2,
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(ReferenceEvidence value) noexcept;
[[nodiscard]] ASSET_REGISTRY_API std::optional<ReferenceEvidence> reference_evidence_from_token(
    std::string_view token) noexcept;

/// True when the evidence level is sufficient to act on the reference.
[[nodiscard]] ASSET_REGISTRY_API bool is_actionable(ReferenceEvidence value) noexcept;

/// Typed reference attached to an asset record. Exactly one target field is
/// active, selected by `kind`.
class ASSET_REGISTRY_API Reference {
public:
    enum class Kind : std::uint8_t {
        Capability = 0,
        Location = 1,
        Rack = 2,
        ExternalObject = 3,
    };

    Reference() = default;

    [[nodiscard]] static Reference capability(class CapabilityReference value, ReferenceEvidence evidence);
    [[nodiscard]] static Reference location(class LocationId value, ReferenceEvidence evidence);
    [[nodiscard]] static Reference rack(class RackId value, ReferenceEvidence evidence);
    [[nodiscard]] static Reference external_object(class ExternalObjectReference value, ReferenceEvidence evidence);

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] ReferenceEvidence evidence() const noexcept { return evidence_; }

    /// The capability target. Only valid when kind() == Kind::Capability.
    [[nodiscard]] const class CapabilityReference& capability() const noexcept { return capability_; }
    [[nodiscard]] const class LocationId& location() const noexcept { return location_; }
    [[nodiscard]] const class RackId& rack() const noexcept { return rack_; }
    [[nodiscard]] const class ExternalObjectReference& external_object() const noexcept { return external_; }

    /// Canonical ordering key: "<kind>:<target>". Used for deterministic
    /// ordering of attached references and for duplicate detection.
    [[nodiscard]] const std::string& canonical() const noexcept { return canonical_; }

    /// Lowercase token naming the reference kind ("capability", "location",
    /// "rack", "external_object").
    [[nodiscard]] std::string_view kind_token() const noexcept;

    friend bool operator==(const Reference& lhs, const Reference& rhs) noexcept {
        return lhs.canonical_ == rhs.canonical_;
    }
    friend bool operator!=(const Reference& lhs, const Reference& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const Reference& lhs, const Reference& rhs) noexcept {
        return lhs.canonical_ < rhs.canonical_;
    }

private:
    Kind kind_ = Kind::Capability;
    ReferenceEvidence evidence_ = ReferenceEvidence::Unverified;
    class CapabilityReference capability_;
    class LocationId location_;
    class RackId rack_;
    class ExternalObjectReference external_;
    std::string canonical_;
};

[[nodiscard]] ASSET_REGISTRY_API std::string_view to_string(Reference::Kind value) noexcept;

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_REFERENCE_HPP
