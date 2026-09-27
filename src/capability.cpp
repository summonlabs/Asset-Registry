// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_registry/capability.hpp"

#include "asset_registry/text.hpp"

namespace asset_registry {

std::optional<IdentityNamespace> IdentityNamespace::create(std::string_view text) {
    if (!is_identity_namespace(text)) {
        return std::nullopt;
    }
    IdentityNamespace name_space;
    name_space.text_ = std::string(text);
    return name_space;
}

std::optional<CapabilityReference> CapabilityReference::create(std::string_view text) {
    if (text.empty() || text.size() > limits::kMaxCapabilityReferenceBytes) {
        return std::nullopt;
    }
    const std::size_t separator = text.find(':');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= text.size()) {
        return std::nullopt;
    }
    // Exactly one separator: a second colon would make the reference ambiguous
    // between two spellings of the same target.
    if (text.find(':', separator + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto name_space = IdentityNamespace::create(text.substr(0, separator));
    if (!name_space.has_value()) {
        return std::nullopt;
    }
    return create(*name_space, text.substr(separator + 1));
}

std::optional<CapabilityReference> CapabilityReference::create(const IdentityNamespace& name_space,
                                                               std::string_view path) {
    if (name_space.empty()) {
        return std::nullopt;
    }
    if (path.empty() || path.size() > limits::kMaxCapabilityPathBytes) {
        return std::nullopt;
    }
    if (!is_dotted_identity_path(path)) {
        return std::nullopt;
    }
    CapabilityReference reference;
    reference.name_space_ = name_space;
    reference.path_ = std::string(path);
    reference.canonical_.reserve(name_space.text().size() + 1 + reference.path_.size());
    reference.canonical_ = name_space.text();
    reference.canonical_ += ':';
    reference.canonical_ += reference.path_;
    return reference;
}

}  // namespace asset_registry
