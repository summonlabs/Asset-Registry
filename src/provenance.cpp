// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_registry/provenance.hpp"

#include <array>
#include <chrono>
#include <cstdio>

namespace asset_registry {
namespace {

struct OriginEntry {
    ProvenanceOrigin value;
    std::string_view token;
};

constexpr std::array<OriginEntry, 4> kOrigins = {{
    {ProvenanceOrigin::ApiMutation, "api_mutation"},
    {ProvenanceOrigin::Import, "import"},
    {ProvenanceOrigin::Recovery, "recovery"},
    {ProvenanceOrigin::IdentityReuse, "identity_reuse"},
}};

struct ActionEntry {
    ProvenanceAction value;
    std::string_view token;
};

constexpr std::array<ActionEntry, 15> kActions = {{
    {ProvenanceAction::Registered, "registered"},
    {ProvenanceAction::MetadataUpdated, "metadata_updated"},
    {ProvenanceAction::LifecycleTransitioned, "lifecycle_transitioned"},
    {ProvenanceAction::InstallationTransitioned, "installation_transitioned"},
    {ProvenanceAction::ReferenceAttached, "reference_attached"},
    {ProvenanceAction::ReferenceDetached, "reference_detached"},
    {ProvenanceAction::ReferenceEvidenceChanged, "reference_evidence_changed"},
    {ProvenanceAction::OwnerChanged, "owner_changed"},
    {ProvenanceAction::PlacementChanged, "placement_changed"},
    {ProvenanceAction::ReplacementLinked, "replacement_linked"},
    {ProvenanceAction::IdentityReused, "identity_reused"},
    {ProvenanceAction::SerialIdentityUpdated, "serial_identity_updated"},
    {ProvenanceAction::ClassReclassified, "class_reclassified"},
    {ProvenanceAction::StateDeclared, "state_declared"},
    {ProvenanceAction::RecoveredFromStore, "recovered_from_store"},
}};

/// Days from 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
/// civil-from-days algorithm). Valid for the whole range the Timestamp type
/// accepts, including negative (pre-1970) values.
[[nodiscard]] std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
    const unsigned day_of_year = (153U * adjusted_month + 2U) / 5U + day - 1U;
    const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

struct CivilDate {
    std::int64_t year = 1970;
    unsigned month = 1;
    unsigned day = 1;
};

[[nodiscard]] CivilDate civil_from_days(std::int64_t days) noexcept {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
    std::int64_t year = static_cast<std::int64_t>(year_of_era) + era * 400;
    const unsigned day_of_year = day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    const unsigned month_prime = (5U * day_of_year + 2U) / 153U;
    const unsigned day = day_of_year - (153U * month_prime + 2U) / 5U + 1U;
    const unsigned month = month_prime < 10U ? month_prime + 3U : month_prime - 9U;
    year += month <= 2U ? 1 : 0;
    return CivilDate{year, month, day};
}

[[nodiscard]] bool is_leap_year(std::int64_t year) noexcept {
    const bool divisible_by_four = year % 4 == 0;
    const bool divisible_by_hundred = year % 100 == 0;
    const bool divisible_by_four_hundred = year % 400 == 0;
    return divisible_by_four && (!divisible_by_hundred || divisible_by_four_hundred);
}

[[nodiscard]] unsigned days_in_month(std::int64_t year, unsigned month) noexcept {
    static constexpr std::array<unsigned, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return kDays[month - 1];
}

[[nodiscard]] std::optional<std::uint64_t> parse_digits(std::string_view text, std::size_t count) noexcept {
    if (text.size() != count) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        value = value * 10U + static_cast<std::uint64_t>(character - '0');
    }
    return value;
}

}  // namespace

std::optional<Timestamp> Timestamp::create(std::int64_t unix_nanos) noexcept {
    if (unix_nanos < kMinUnixNanos || unix_nanos > kMaxUnixNanos) {
        return std::nullopt;
    }
    return Timestamp(unix_nanos);
}

std::optional<Timestamp> Timestamp::parse_rfc3339(std::string_view text) noexcept {
    // Strict profile: "YYYY-MM-DDTHH:MM:SSZ" or "YYYY-MM-DDTHH:MM:SS.fffffffffZ".
    // A UTC offset other than Z is rejected so the stored value is unambiguous.
    if (text.size() != 20 && text.size() != 30) {
        return std::nullopt;
    }
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    if (text.back() != 'Z') {
        return std::nullopt;
    }
    if (text.size() == 30 && text[19] != '.') {
        return std::nullopt;
    }

    const auto year = parse_digits(text.substr(0, 4), 4);
    const auto month = parse_digits(text.substr(5, 2), 2);
    const auto day = parse_digits(text.substr(8, 2), 2);
    const auto hour = parse_digits(text.substr(11, 2), 2);
    const auto minute = parse_digits(text.substr(14, 2), 2);
    const auto second = parse_digits(text.substr(17, 2), 2);
    if (!year.has_value() || !month.has_value() || !day.has_value() || !hour.has_value() ||
        !minute.has_value() || !second.has_value()) {
        return std::nullopt;
    }
    if (*month < 1 || *month > 12 || *day < 1 || *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    if (*day > days_in_month(static_cast<std::int64_t>(*year), static_cast<unsigned>(*month))) {
        return std::nullopt;
    }

    std::int64_t nanos = 0;
    if (text.size() == 30) {
        const auto fraction = parse_digits(text.substr(20, 9), 9);
        if (!fraction.has_value()) {
            return std::nullopt;
        }
        nanos = static_cast<std::int64_t>(*fraction);
    }

    const std::int64_t days =
        days_from_civil(static_cast<std::int64_t>(*year), static_cast<unsigned>(*month), static_cast<unsigned>(*day));
    const std::int64_t seconds = days * 86400 + static_cast<std::int64_t>(*hour) * 3600 +
                                 static_cast<std::int64_t>(*minute) * 60 + static_cast<std::int64_t>(*second);
    return create(seconds * 1000000000 + nanos);
}

std::string Timestamp::to_rfc3339() const {
    std::int64_t seconds = unix_nanos_ / 1000000000;
    std::int64_t nanos = unix_nanos_ % 1000000000;
    if (nanos < 0) {
        nanos += 1000000000;
        seconds -= 1;
    }
    // Split toward negative infinity so that the civil date and the time of day are
    // both correct for instants before the epoch, where truncating division would
    // put the remainder on the wrong side of midnight.
    std::int64_t days = seconds / 86400;
    std::int64_t second_of_day = seconds % 86400;
    if (second_of_day < 0) {
        second_of_day += 86400;
        days -= 1;
    }
    const CivilDate date = civil_from_days(days);
    const auto hour = static_cast<unsigned>(second_of_day / 3600);
    const auto minute = static_cast<unsigned>((second_of_day % 3600) / 60);
    const auto second = static_cast<unsigned>(second_of_day % 60);

    char buffer[40];
    const int written = std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02u:%02u:%02u",  // NOLINT
                                      static_cast<long long>(date.year), date.month, date.day, hour, minute, second);
    std::string result(buffer, written > 0 ? static_cast<std::size_t>(written) : 0);
    if (nanos != 0) {
        char fraction[16];
        const int digits = std::snprintf(fraction, sizeof(fraction), ".%09lld",  // NOLINT
                                         static_cast<long long>(nanos));
        if (digits > 0) {
            result.append(fraction, static_cast<std::size_t>(digits));
        }
    }
    result.push_back('Z');
    return result;
}

Clock::~Clock() = default;

Timestamp SystemClock::now() const {
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
    if (nanos < Timestamp::kMinUnixNanos) {
        return Timestamp(Timestamp::kMinUnixNanos);
    }
    if (nanos > Timestamp::kMaxUnixNanos) {
        return Timestamp(Timestamp::kMaxUnixNanos);
    }
    return Timestamp(nanos);
}

std::string_view to_string(ProvenanceOrigin value) noexcept {
    for (const OriginEntry& entry : kOrigins) {
        if (entry.value == value) {
            return entry.token;
        }
    }
    return "api_mutation";
}

std::optional<ProvenanceOrigin> provenance_origin_from_token(std::string_view token) noexcept {
    for (const OriginEntry& entry : kOrigins) {
        if (entry.token == token) {
            return entry.value;
        }
    }
    return std::nullopt;
}

std::string_view to_string(ProvenanceAction value) noexcept {
    for (const ActionEntry& entry : kActions) {
        if (entry.value == value) {
            return entry.token;
        }
    }
    return "registered";
}

std::optional<ProvenanceAction> provenance_action_from_token(std::string_view token) noexcept {
    for (const ActionEntry& entry : kActions) {
        if (entry.token == token) {
            return entry.value;
        }
    }
    return std::nullopt;
}

ActorRef ActorRef::writer(WriterId id) {
    ActorRef actor;
    actor.writer_ = std::move(id);
    return actor;
}

ActorRef ActorRef::system() {
    return ActorRef{};
}

std::string ActorRef::to_string() const {
    return writer_.empty() ? std::string("system") : writer_.text();
}

}  // namespace asset_registry
