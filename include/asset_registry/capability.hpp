// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Capability references. A capability reference names a behaviour or resource
// that some other DCCP repository owns (for example the ASI accelerator
// scheduling service). The Asset Registry stores the reference and nothing
// else: it never authors, validates, or overrides the semantics of the target.

#ifndef ASSET_REGISTRY_CAPABILITY_HPP
#define ASSET_REGISTRY_CAPABILITY_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "asset_registry/error.hpp"

namespace asset_registry {

namespace limits {
inline constexpr std::size_t kMaxCapabilityNamespaceBytes = 32;
inline constexpr std::size_t kMaxCapabilityPathBytes = 192;
inline constexpr std::size_t kMaxCapabilityReferenceBytes = kMaxCapabilityNamespaceBytes + 1 + kMaxCapabilityPathBytes;
}  // namespace limits

/// Lowercase-hyphen identity namespace, e.g. "asi", "dfi", "vendor-acme".
class ASSET_REGISTRY_API IdentityNamespace {
public:
    IdentityNamespace() = default;

    [[nodiscard]] static std::optional<IdentityNamespace> create(std::string_view text);

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

    friend bool operator==(const IdentityNamespace& lhs, const IdentityNamespace& rhs) noexcept {
        return lhs.text_ == rhs.text_;
    }
    friend bool operator!=(const IdentityNamespace& lhs, const IdentityNamespace& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend bool operator<(const IdentityNamespace& lhs, const IdentityNamespace& rhs) noexcept {
        return lhs.text_ < rhs.text_;
    }

private:
    std::string text_;
};

/// A typed, opaque capability reference of the form "namespace:path".
///
/// Ownership: the namespace identifies the repository that owns the referenced
/// behaviour. The Asset Registry records the reference, orders it deterministically,
/// and reports unknown references as unknown. Attaching a reference never
/// transfers authority over the target.
class ASSET_REGISTRY_API CapabilityReference {
public:
    CapabilityReference() = default;

    [[nodiscard]] static std::optional<CapabilityReference> create(std::string_view text);

    [[nodiscard]] static std::optional<CapabilityReference> create(const IdentityNamespace& name_space,
                                                                   std::string_view path);

    [[nodiscard]] const IdentityNamespace& name_space() const noexcept { return name_space_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    /// Canonical single-string spelling "namespace:path". This is the value used
    /// for ordering, export, and comparison.
    [[nodiscard]] const std::string& canonical() const noexcept { return canonical_; }
    [[nodiscard]] bool empty() const noexcept { return canonical_.empty(); }

    friend bool operator==(const CapabilityReference& lhs, const CapabilityReference& rhs) noexcept {
        return lhs.canonical_ == rhs.canonical_;
    }
    friend bool operator!=(const CapabilityReference& lhs, const CapabilityReference& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend bool operator<(const CapabilityReference& lhs, const CapabilityReference& rhs) noexcept {
        return lhs.canonical_ < rhs.canonical_;
    }

private:
    IdentityNamespace name_space_;
    std::string path_;
    std::string canonical_;
};

}  // namespace asset_registry

#endif  // ASSET_REGISTRY_CAPABILITY_HPP
