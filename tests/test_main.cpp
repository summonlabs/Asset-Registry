// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "test_harness.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace asset_test {
namespace {

std::vector<TestCase>& cases() {
    static std::vector<TestCase> registry;
    return registry;
}

struct RunState {
    std::string current;
    std::uint64_t failures = 0;
    std::uint64_t checks = 0;
    bool quiet = false;
};

RunState& state() {
    static RunState value;
    return value;
}

void report(const std::string& line) {
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

}  // namespace

void register_case(std::string suite, std::string name, std::function<void()> body) {
    cases().push_back(TestCase{std::move(suite), std::move(name), std::move(body)});
}

void record_failure(const char* file, int line, std::string message) {
    ++state().failures;
    if (state().quiet) {
        return;
    }
    report("FAIL " + state().current + " at " + file + ":" + std::to_string(line) + ": " + message);
}

void record_note(std::string message) {
    if (state().quiet) {
        return;
    }
    report("note " + state().current + ": " + std::move(message));
}

void log_line(std::string_view text) {
    report(std::string(text));
}

QuietScope::QuietScope() {
    previous_ = state().quiet;
    state().quiet = true;
}

QuietScope::~QuietScope() {
    state().quiet = previous_;
}

std::uint64_t failure_count() noexcept {
    return state().failures;
}

int run_all(int argc, char** argv) {
    std::string filter;
    bool list_only = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--list") {
            list_only = true;
            continue;
        }
        if (argument.rfind("--filter=", 0) == 0) {
            filter = std::string(argument.substr(9));
            continue;
        }
        report(std::string("unknown argument: ") + std::string(argument));
        return 2;
    }

    if (list_only) {
        for (const TestCase& test : cases()) {
            report(test.suite + "." + test.name);
        }
        return 0;
    }

    std::uint64_t executed = 0;
    std::uint64_t skipped = 0;
    for (const TestCase& test : cases()) {
        const std::string full = test.suite + "." + test.name;
        if (!filter.empty() && full.find(filter) == std::string::npos) {
            ++skipped;
            continue;
        }
        state().current = full;
        const std::uint64_t before = state().failures;
        try {
            test.body();
        } catch (const std::exception& error) {
            record_failure(__FILE__, __LINE__,
                           std::string("the case threw an exception: ") + error.what());
        } catch (...) {
            record_failure(__FILE__, __LINE__, "the case threw an unknown exception");
        }
        ++executed;
        if (state().failures == before && !state().quiet) {
            report("ok   " + full);
        }
    }
    std::string summary = "ran " + std::to_string(executed) + " case(s)";
    if (skipped > 0) {
        summary += ", filtered out " + std::to_string(skipped);
    }
    summary += ", " + std::to_string(state().failures) + " failure(s)";
    report(summary);
    return state().failures == 0 ? 0 : 1;
}

}  // namespace asset_test

int main(int argc, char** argv) {
    return asset_test::run_all(argc, argv);
}
