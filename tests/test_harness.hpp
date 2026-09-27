// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Minimal test harness.
//
// Deliberately small: a registry of cases, an assertion set that records failures
// with file and line, and a runner that reports every failure and exits non-zero
// when any case failed. There is no timeout mechanism of any kind: a test that
// hangs is a defect to be diagnosed, not a test to be killed, so the suite runs
// each case to completion and the process exits only when every case has
// returned.

#ifndef ASSET_REGISTRY_TEST_HARNESS_HPP
#define ASSET_REGISTRY_TEST_HARNESS_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace asset_test {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

/// Registers a case. Called by the REGISTER_TEST macro at namespace scope.
void register_case(std::string suite, std::string name, std::function<void()> body);

/// Records a failure for the currently running case.
void record_failure(const char* file, int line, std::string message);

/// Records a non-fatal observation that is printed but does not fail the case.
void record_note(std::string message);

/// Marks a bound assertion result as used. A case that binds a result it never
/// inspects would otherwise be rejected by a strict-warning build for an unused
/// variable, which says nothing about the behaviour under test; this consumes the
/// binding for that purpose only and asserts nothing.
template <typename T>
const T& bound_value(const T& value) noexcept {
    return value;
}

/// Prints a diagnostic line for the currently running case.
void log_line(std::string_view text);

/// Temporarily silences failure output, for adversarial cases that expect a
/// rejection and would otherwise print a wall of expected diagnostics.
class QuietScope {
public:
    QuietScope();
    ~QuietScope();
    QuietScope(const QuietScope&) = delete;
    QuietScope& operator=(const QuietScope&) = delete;

private:
    bool previous_ = false;
};

/// Runs every registered case. Returns the number of failed cases.
int run_all(int argc, char** argv);

/// Number of cases that failed so far in the current process.
[[nodiscard]] std::uint64_t failure_count() noexcept;

}  // namespace asset_test

#define AR_TEST(suite_name, case_name)                                                       \
    static void suite_name##_##case_name##_body();                                            \
    namespace {                                                                              \
    const bool suite_name##_##case_name##_registered = [] {                                   \
        ::asset_test::register_case(#suite_name, #case_name, &suite_name##_##case_name##_body); \
        return true;                                                                          \
    }();                                                                                      \
    }                                                                                         \
    static void suite_name##_##case_name##_body()

#define AR_CHECK(condition)                                                                       \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            ::asset_test::record_failure(__FILE__, __LINE__, "check failed: " #condition);        \
        }                                                                                         \
    } while (false)

#define AR_CHECK_MSG(condition, message)                                                          \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            ::asset_test::record_failure(__FILE__, __LINE__,                                      \
                                         std::string("check failed: " #condition " -- ") +        \
                                             std::string(message));                               \
        }                                                                                         \
    } while (false)

/// Checks `condition` and abandons the case when it does not hold.
///
/// A case that cannot continue meaningfully — a decode that failed, an element that is
/// not the one expected — uses this so the failure is reported once rather than once per
/// dependent assertion.
#define AR_REQUIRE_CHECK(condition)                                                               \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            ::asset_test::record_failure(__FILE__, __LINE__, "required check failed: " #condition); \
            return;                                                                               \
        }                                                                                         \
    } while (false)

/// Requires `condition` and reports `message` when it does not hold.
#define AR_REQUIRE_CHECK_MSG(condition, message)                                                  \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            ::asset_test::record_failure(__FILE__, __LINE__,                                      \
                                         std::string("required check failed: " #condition " -- ") + \
                                             std::string(message));                               \
            return;                                                                               \
        }                                                                                         \
    } while (false)

/// Requires `outcome` to hold a value and binds it to `name`.
///
/// The binding is asserted to be usable, so a case that binds a result it never
/// reads still fails a strict-warning build rather than being silently accepted.
#define AR_REQUIRE_OK(name, outcome)                                                              \
    auto name##_outcome = (outcome);                                                              \
    if (!name##_outcome) {                                                                        \
        ::asset_test::record_failure(__FILE__, __LINE__,                                          \
                                     std::string("expected success but got: ") +                  \
                                         name##_outcome.error().to_string());                     \
        return;                                                                                   \
    }                                                                                             \
    AR_CHECK(static_cast<bool>(name##_outcome));                                                  \
    auto& name = name##_outcome.value();                                                          \
    (void)::asset_test::bound_value(name)

/// Requires an optional to hold a value and binds it to `name`.
#define AR_REQUIRE_PRESENT(name, optional_value)                                                  \
    auto name##_optional = (optional_value);                                                      \
    if (!name##_optional.has_value()) {                                                           \
        ::asset_test::record_failure(__FILE__, __LINE__,                                          \
                                     "expected the optional to hold a value: " #optional_value);   \
        return;                                                                                   \
    }                                                                                             \
    const auto& name = *name##_optional

/// Requires `outcome` to fail with the given code and binds the error to `name`.
#define AR_REQUIRE_ERROR(name, outcome, expected_code)                                            \
    auto name##_outcome = (outcome);                                                              \
    if (name##_outcome) {                                                                         \
        ::asset_test::record_failure(__FILE__, __LINE__,                                          \
                                     "expected rejection " #expected_code " but the call succeeded"); \
        return;                                                                                   \
    }                                                                                             \
    const ::asset_registry::Error& name = name##_outcome.error();                                 \
    if (name.code() != (expected_code)) {                                                         \
        ::asset_test::record_failure(__FILE__, __LINE__,                                          \
                                     std::string("expected rejection " #expected_code " but got ") + \
                                         std::string(::asset_registry::error_code_name(name.code())) + \
                                         ": " + name.message());                                  \
        return;                                                                                   \
    }

#endif  // ASSET_REGISTRY_TEST_HARNESS_HPP
