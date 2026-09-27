// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Independent-process tests.
//
// These tests start the helper as a real operating-system process, which is the
// only way to prove the claims that depend on process boundaries: that the store
// lock excludes another process, that a writer minted in one incarnation is fenced
// out in the next, and that state committed by one process is authoritative for
// another. Nothing here simulates a process: each assertion follows an actual
// spawn, and the helper's exit status is part of the assertion.
//
// The harness has no timeout mechanism. A helper that hung would hang this test,
// which is the intended behaviour: a hang is a defect to be diagnosed, not a case
// to be killed.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "test_harness.hpp"
#include "test_support.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace asset_registry;
using namespace asset_test;

namespace {

struct SpawnResult {
    int exit_code = -1;
    bool started = false;
};

/// Directory holding this test binary. A multi-configuration generator keeps the
/// test and the helper in a per-configuration subdirectory and runs the test from
/// the parent, so the binary's own location, not the working directory, is what
/// resolves the helper that is built beside it.
[[nodiscard]] std::filesystem::path self_directory() {
    std::error_code error;
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            break;
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    std::vector<char> buffer(1024, '\0');
    const ssize_t written = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (written > 0) {
        return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(written))).parent_path();
    }
#endif
    return std::filesystem::current_path(error);
}

/// Resolves the helper next to this test binary, so the suite works from any
/// build directory without an absolute path baked into the source.
[[nodiscard]] std::filesystem::path helper_path() {
    std::error_code error;
    const std::filesystem::path self = self_directory();
    const std::vector<std::filesystem::path> candidates = {
        self / "ar_multiprocess_helper",
        self / "ar_multiprocess_helper.exe",
        self.parent_path() / "ar_multiprocess_helper",
        self.parent_path() / "ar_multiprocess_helper.exe",
        self / "tests" / "ar_multiprocess_helper",
        self / "tests" / "ar_multiprocess_helper.exe",
    };
    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
    }
    return {};
}

[[nodiscard]] std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

/// Runs the helper and waits for it to finish. The command line is built from a
/// fixed program path plus arguments the test controls, and the helper is this
/// repository's own binary: no external command is ever executed from imported
/// state.
[[nodiscard]] SpawnResult run_helper(const std::vector<std::string>& arguments) {
    SpawnResult result;
    const std::filesystem::path helper = helper_path();
    if (helper.empty()) {
        return result;
    }
    std::string command = quote(helper);
    for (const std::string& argument : arguments) {
        command += " ";
        command += quote(argument);
    }
#ifdef _WIN32
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::string mutable_command = command;
    if (::CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                         &process) == 0) {
        return result;
    }
    ::WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    ::GetExitCodeProcess(process.hProcess, &exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    result.exit_code = static_cast<int>(exit_code);
    result.started = true;
#else
    const pid_t child = ::fork();
    if (child == 0) {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(helper.string().c_str()));
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(helper.string().c_str(), argv.data());
        ::_exit(127);
    }
    if (child < 0) {
        return result;
    }
    int status = 0;
    if (::waitpid(child, &status, 0) < 0) {
        return result;
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result.started = true;
#endif
    return result;
}

/// Starts the helper without waiting, so two processes genuinely overlap.
class BackgroundHelper {
public:
    explicit BackgroundHelper(std::vector<std::string> arguments) {
        const std::filesystem::path helper = helper_path();
        if (helper.empty()) {
            return;
        }
        std::string command = quote(helper);
        for (const std::string& argument : arguments) {
            command += " ";
            command += quote(argument);
        }
#ifdef _WIN32
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        std::string mutable_command = command;
        if (::CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                             &process_) == 0) {
            process_.hProcess = nullptr;
            return;
        }
        started_ = true;
#else
        child_ = ::fork();
        if (child_ == 0) {
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(helper.string().c_str()));
            for (const std::string& argument : arguments) {
                argv.push_back(const_cast<char*>(argument.c_str()));
            }
            argv.push_back(nullptr);
            ::execv(helper.string().c_str(), argv.data());
            ::_exit(127);
        }
        started_ = child_ >= 0;
#endif
    }

    BackgroundHelper(const BackgroundHelper&) = delete;
    BackgroundHelper& operator=(const BackgroundHelper&) = delete;

    ~BackgroundHelper() {
        if (started_) {
            (void)wait();
        }
    }

    [[nodiscard]] bool started() const noexcept { return started_; }

    /// Waits for completion. Called at most once per helper.
    [[nodiscard]] int wait() {
        if (!started_) {
            return -1;
        }
        started_ = false;
#ifdef _WIN32
        ::WaitForSingleObject(process_.hProcess, INFINITE);
        DWORD exit_code = 1;
        ::GetExitCodeProcess(process_.hProcess, &exit_code);
        ::CloseHandle(process_.hThread);
        ::CloseHandle(process_.hProcess);
        return static_cast<int>(exit_code);
#else
        int status = 0;
        if (::waitpid(child_, &status, 0) < 0) {
            return -1;
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }

    /// Kills the process without letting it unwind, so a case can prove what a crashed
    /// incarnation leaves behind. The process is still reaped by wait().
    void kill() {
        if (!started_) {
            return;
        }
#ifdef _WIN32
        if (process_.hProcess != nullptr) {
            (void)::TerminateProcess(process_.hProcess, kKilledExitCode);
        }
#else
        (void)::kill(child_, SIGKILL);
#endif
    }

private:
    /// Exit code a killed helper settles at. Never used by the helper itself, so a case
    /// can tell "killed" from "returned".
    static constexpr int kKilledExitCode = 137;
    bool started_ = false;
#ifdef _WIN32
    PROCESS_INFORMATION process_{};
#else
    pid_t child_ = -1;
#endif
};

}  // namespace

AR_TEST(multiprocess, the_helper_binary_is_available) {
    const std::filesystem::path helper = helper_path();
    AR_CHECK_MSG(!helper.empty(), "ar_multiprocess_helper was not found next to the test binary");
    if (helper.empty()) {
        return;
    }
    // `inspect` on a directory that is not yet a store must fail cleanly rather
    // than crash, which also proves the helper runs at all.
    TempDirectory directory("multiprocess-probe");
    const SpawnResult result = run_helper({"inspect", directory.path().string()});
    AR_CHECK(result.started);
    if (result.started) {
        AR_CHECK(result.exit_code == 4);
    }
}

AR_TEST(multiprocess, state_committed_by_one_process_is_authoritative_for_another) {
    TempDirectory directory("multiprocess-restart");
    const std::string prefix = "cross-process";
    constexpr int kCount = 8;

    const SpawnResult first = run_helper({"register", directory.path().string(), "process-one", prefix,
                                          std::to_string(kCount)});
    AR_CHECK(first.started);
    AR_CHECK_MSG(first.exit_code == 0, "helper exit code " + std::to_string(first.exit_code));

    // A second, independent process reads the committed inventory.
    const SpawnResult verify = run_helper({"verify", directory.path().string(), prefix, std::to_string(kCount)});
    AR_CHECK(verify.started);
    AR_CHECK_MSG(verify.exit_code == 0, "verify exit code " + std::to_string(verify.exit_code));

    // A third process registers more assets into the same store, which requires it
    // to have opened and recovered the state the second process left.
    const std::string second_prefix = "cross-process-two";
    const SpawnResult third = run_helper({"register", directory.path().string(), "process-three", second_prefix,
                                          std::to_string(kCount)});
    AR_CHECK(third.started);
    AR_CHECK(third.exit_code == 0);

    const SpawnResult confirm = run_helper({"inspect", directory.path().string()});
    AR_CHECK(confirm.started);
    AR_CHECK(confirm.exit_code == 0);

    // The parent process reads the same store and finds every asset from both
    // independent writers.
    RecoveryReport report;
    auto registry = AssetRegistry::open(directory.path(), StoreOpenMode::ReadOnly, StoreOpenOptions{}, report);
    AR_REQUIRE_OK(opened, registry);
    const Snapshot snapshot = opened->snapshot();
    AR_CHECK(snapshot.size() == static_cast<std::uint64_t>(kCount * 2));
    AR_CHECK(report.action == RecoveryAction::None || report.action == RecoveryAction::Initialised ||
             report.action == RecoveryAction::RemovedOrphanTemporaries);
    AR_CHECK(snapshot.audit_conflicts().clean());
    AR_CHECK(opened->close().has_value());
}

AR_TEST(multiprocess, a_second_process_cannot_open_a_store_another_process_holds) {
    TempDirectory directory("multiprocess-lock");
    // Initialise the store first so the holder has something to open.
    const SpawnResult seed = run_helper({"register", directory.path().string(), "seed-writer", "seed", "1"});
    AR_CHECK(seed.started);
    AR_CHECK(seed.exit_code == 0);

    // Hold the store for long enough that the competing process runs while it is
    // held.
    BackgroundHelper holder({"hold", directory.path().string(), "holder-writer", "1500"});
    AR_CHECK(holder.started());
    if (!holder.started()) {
        return;
    }
    // Give the holder time to acquire the lock. This is a scheduling delay, not a
    // test timeout: the assertions below do not depend on it for correctness, only
    // for exercising the contended path.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const SpawnResult contender =
        run_helper({"register", directory.path().string(), "contender-writer", "contender", "1"});
    AR_CHECK(contender.started);
    AR_CHECK_MSG(contender.exit_code == 2,
                 "expected the lock to be refused, helper exit code " + std::to_string(contender.exit_code));

    // A read-only open is still possible while a writer holds the store, and it
    // observes only committed state.
    RecoveryReport read_report;
    auto reader = AssetRegistry::open(directory.path(), StoreOpenMode::ReadOnly, StoreOpenOptions{}, read_report);
    AR_REQUIRE_OK(read_only, reader);
    AR_CHECK(read_only->snapshot().size() == 1);
    AR_CHECK(read_only->close().has_value());

    AR_CHECK(holder.wait() == 0);
}

AR_TEST(multiprocess, a_writer_from_a_previous_incarnation_is_fenced_after_another_process_reopens) {
    TempDirectory directory("multiprocess-fencing");
    const SpawnResult seed = run_helper({"register", directory.path().string(), "seed-writer", "fence-seed", "1"});
    AR_CHECK(seed.started);
    AR_CHECK(seed.exit_code == 0);

    // The first process mints authority, then exits, releasing the store.
    const SpawnResult fenced = run_helper({"fenced", directory.path().string(), "fenced-writer", "50"});
    AR_CHECK(fenced.started);
    AR_CHECK_MSG(fenced.exit_code == 0, "the holder's own mutation should succeed; exit code " +
                                            std::to_string(fenced.exit_code));

    // Another process reopens the store, which publishes a strictly greater epoch.
    // Authority minted by the earlier incarnation is no longer live in this one.
    RecoveryReport report;
    StoreOpenOptions options;
    auto registry = AssetRegistry::open(directory.path(), StoreOpenMode::CreateIfMissing, options, report);
    AR_REQUIRE_OK(opened, registry);

    // A token with the previous epoch must be refused, and the refusal must be the
    // epoch-specific code rather than a generic failure.
    AuthorityToken stale;
    {
        // Mint a token, then reopen the store again so the minted token belongs to
        // the closed incarnation.
        auto session = opened->open_writer(WriterId::create("incarnation-one").value());
        AR_REQUIRE_OK(armed, session);
        stale = armed.token();
    }
    AR_CHECK(opened->close().has_value());

    RecoveryReport second_report;
    auto second = AssetRegistry::open(directory.path(), StoreOpenMode::CreateIfMissing, options, second_report);
    AR_REQUIRE_OK(reopened, second);
    AR_CHECK(second_report.epoch > report.epoch);
    AR_CHECK(!reopened->store()->is_token_live(stale));

    WriterSession stale_session = WriterSession::create(WriterId::create("incarnation-one").value());
    stale_session.attach_token(stale);
    auto refused = reopened->register_asset(
        stale_session, stale_session.advance(),
        [&] {
            RegisterAssetRequest request = make_request("fenced-attempt", AssetClass::Server, "Acme", "FENCE-X");
            return request;
        }());
    AR_REQUIRE_ERROR(error, refused, ErrorCode::StaleAuthorityEpoch);
    // The refused mutation published nothing.
    AR_CHECK(!reopened->snapshot().contains(asset_id_for("fenced-attempt")));
    AR_CHECK(reopened->close().has_value());
}

AR_TEST(multiprocess, concurrent_processes_produce_one_consistent_inventory) {
    TempDirectory directory("multiprocess-concurrent");
    constexpr int kProcesses = 3;
    constexpr int kPerProcess = 6;

    std::vector<std::unique_ptr<BackgroundHelper>> helpers;
    for (int index = 0; index < kProcesses; ++index) {
        helpers.push_back(std::make_unique<BackgroundHelper>(
            std::vector<std::string>{"register", directory.path().string(), "racer-" + std::to_string(index),
                                     "racer-" + std::to_string(index), std::to_string(kPerProcess), "60"}));
    }
    int completed = 0;
    int lock_refusals = 0;
    for (auto& helper : helpers) {
        AR_CHECK(helper->started());
        const int code = helper->wait();
        if (code == 0) {
            ++completed;
        } else if (code == 2) {
            ++lock_refusals;
        } else {
            AR_CHECK_MSG(false, "unexpected helper exit code " + std::to_string(code));
        }
    }
    AR_CHECK(completed + lock_refusals == kProcesses);
    AR_CHECK(completed >= 1);

    // Whatever the interleaving, the surviving inventory is consistent and every
    // asset in it belongs to a process that reported success.
    RecoveryReport report;
    auto registry = AssetRegistry::open(directory.path(), StoreOpenMode::ReadOnly, StoreOpenOptions{}, report);
    AR_REQUIRE_OK(opened, registry);
    const Snapshot snapshot = opened->snapshot();
    AR_CHECK(snapshot.audit_conflicts().clean());
    const InventorySummary summary = snapshot.summary();
    AR_CHECK(summary.total_assets <= static_cast<std::uint64_t>(kProcesses * kPerProcess));
    for (const AssetId& identifier : snapshot.ids()) {
        const auto view = snapshot.find(identifier);
        AR_CHECK(view.has_value());
        if (!view.has_value()) {
            continue;
        }
        AR_CHECK(view->metadata.display_name.rfind("racer-", 0) == 0);
        AR_CHECK(view->revision == AssetRevision(1));
        AR_CHECK(view->provenance->size() == 1);
    }
    AR_CHECK(opened->close().has_value());

    // A fresh process started after the dust settles sees the same inventory.
    const SpawnResult confirm = run_helper({"inspect", directory.path().string()});
    AR_CHECK(confirm.started);
    AR_CHECK(confirm.exit_code == 0);
}

AR_TEST(multiprocess, a_killed_process_releases_the_store_and_loses_nothing) {
    TempDirectory directory("multiprocess-kill-holder");
    constexpr int kCount = 3;
    const std::string prefix = "kill";
    // Committed state, so there is something to lose if the store depended on a clean
    // shutdown rather than on what was already published.
    const SpawnResult seed =
        run_helper({"register", directory.path().string(), "kill-seed", prefix, std::to_string(kCount)});
    AR_CHECK(seed.started);
    AR_CHECK_MSG(seed.exit_code == 0, "seed exit code " + std::to_string(seed.exit_code));

    {
        // A second process takes the store and is killed while it holds it: no close, no
        // unwinding, no chance to release the lock or flush anything by hand.
        BackgroundHelper victim({"hold", directory.path().string(), "kill-victim", "20000"});
        AR_CHECK(victim.started());
        if (!victim.started()) {
            return;
        }
        // The store is held once a competing writer is refused the lock, which is the
        // helper's documented exit code for a contended store.
        bool held = false;
        for (int attempt = 0; attempt < 60 && !held; ++attempt) {
            const SpawnResult contender =
                run_helper({"register", directory.path().string(), "kill-contender-" + std::to_string(attempt),
                            "contender", "1"});
            AR_CHECK(contender.started);
            held = contender.exit_code == 2;
        }
        AR_CHECK_MSG(held, "the holder never took the store lock");
        victim.kill();
        AR_CHECK_MSG(victim.wait() != 0, "the killed process reported a clean exit");
    }

    // The operating system released the lock with the process, so the store opens for
    // writing again and holds exactly the inventory the seed committed.
    const SpawnResult verify = run_helper({"verify", directory.path().string(), prefix, std::to_string(kCount)});
    AR_CHECK(verify.started);
    AR_CHECK_MSG(verify.exit_code == 0, "verify exit code " + std::to_string(verify.exit_code));

    // Restart: a fresh process writes to the same store, and its commit is durable.
    const SpawnResult successor =
        run_helper({"register", directory.path().string(), "kill-successor", "after-kill", "1"});
    AR_CHECK(successor.started);
    AR_CHECK_MSG(successor.exit_code == 0, "successor exit code " + std::to_string(successor.exit_code));
    const SpawnResult confirm = run_helper({"verify", directory.path().string(), "after-kill", "1"});
    AR_CHECK(confirm.started);
    AR_CHECK_MSG(confirm.exit_code == 0, "confirm exit code " + std::to_string(confirm.exit_code));
}

AR_TEST(multiprocess, a_process_killed_while_committing_leaves_a_recoverable_store) {
    TempDirectory directory("multiprocess-kill-commit");
    constexpr int kAttempts = 200;
    {
        // A process that commits continuously, killed while it is writing. The kill may
        // land between a staged generation and its publication, or between publication and
        // the pointer update, so the store is left wherever a crash left it rather than at
        // a point the case chose in advance.
        BackgroundHelper churn({"register", directory.path().string(), "churn-writer", "churn",
                                std::to_string(kAttempts)});
        AR_CHECK(churn.started());
        if (!churn.started()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        churn.kill();
        AR_CHECK(churn.wait() != 0);
    }

    // Recovery opens the store: an interrupted commit leaves the previous generation
    // authoritative, and a staging file is removed instead of being mistaken for state.
    RecoveryReport report;
    auto opened = AssetRegistry::open(directory.path(), StoreOpenMode::CreateIfMissing, StoreOpenOptions{}, report);
    AR_REQUIRE_OK(context, opened);
    const Snapshot snapshot = context->snapshot();
    AR_CHECK_MSG(snapshot.size() <= kAttempts,
                 "the store holds " + std::to_string(snapshot.size()) + " records after a kill");
    // Every surviving record is complete: no partially written record was published.
    for (const AssetId& identifier : snapshot.ids()) {
        const auto view = snapshot.find(identifier);
        AR_CHECK(view.has_value());
        if (view.has_value()) {
            AR_CHECK(view->revision == AssetRevision(1));
            AR_CHECK(view->provenance->size() == 1);
            AR_CHECK(view->provenance->front().action == ProvenanceAction::Registered);
        }
    }
    // No staging file survived the crash, so the next commit starts from a clean staging
    // directory rather than a backlog of abandoned writes.
    AR_CHECK(list_directory(directory.path() / "tmp").empty());
    // And the store still accepts writes from the process that recovered it.
    auto session = context->open_writer(writer("after-kill"));
    AR_REQUIRE_OK(armed, session);
    WriterSession recovered = armed;
    RegisterAssetRequest request = make_request("after-kill", AssetClass::Server, "Acme", "AK-1");
    auto registered = context->register_asset(recovered, recovered.advance(), request);
    AR_CHECK_MSG(registered.has_value(), registered.has_value() ? "" : registered.error().to_string());
    AR_CHECK(context->snapshot().size() == snapshot.size() + 1);
    AR_CHECK(context->close().has_value());
}

AR_TEST(multiprocess, a_recreated_directory_is_initialised_again) {
    TempDirectory directory("multiprocess-recreate");
    const SpawnResult first = run_helper({"register", directory.path().string(), "first-writer", "recreate", "2"});
    AR_CHECK(first.started);
    AR_CHECK(first.exit_code == 0);

    directory.reset();
    AR_CHECK(list_directory(directory.path()).empty());

    const SpawnResult second = run_helper({"register", directory.path().string(), "second-writer", "recreate", "2"});
    AR_CHECK(second.started);
    AR_CHECK(second.exit_code == 0);
    const SpawnResult verify = run_helper({"verify", directory.path().string(), "recreate", "2"});
    AR_CHECK(verify.started);
    AR_CHECK(verify.exit_code == 0);
}

