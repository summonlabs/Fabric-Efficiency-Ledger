// Fabric Efficiency Ledger - real independent process locking proof.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This test drives the fel command line tool as a real, separate operating
// system process. It proves that the exclusive store lock is honoured across
// process boundaries, that a second writer is refused while the lock is held,
// and that a waiter proceeds once the holder releases. There are no timeouts:
// every step is synchronised through a pipe read or a process exit.
#ifdef _WIN32

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
  if (condition) {
    std::printf("  ok   %s\n", what.c_str());
  } else {
    std::printf("  FAIL %s\n", what.c_str());
    ++g_failures;
  }
  std::fflush(stdout);
}

struct Child {
  PROCESS_INFORMATION process{};
  HANDLE stdin_write = nullptr;
  HANDLE stderr_read = nullptr;
  bool started = false;
};

[[nodiscard]] std::string quote(const std::string& value) {
  return std::string("\"") + value + "\"";
}

// Starts a child whose stdin is a pipe controlled by this test and whose stderr
// is captured for synchronisation.
[[nodiscard]] Child start_captured(const std::string& command_line,
                                   bool capture_stdin, bool capture_stderr) {
  Child child;
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE child_stdin_read = nullptr;
  HANDLE parent_stdin_write = nullptr;
  HANDLE child_stderr_write = nullptr;
  HANDLE parent_stderr_read = nullptr;
  if (capture_stdin) {
    CreatePipe(&child_stdin_read, &parent_stdin_write, &attributes, 0);
    SetHandleInformation(parent_stdin_write, HANDLE_FLAG_INHERIT, 0);
  }
  if (capture_stderr) {
    CreatePipe(&parent_stderr_read, &child_stderr_write, &attributes, 0);
    SetHandleInformation(parent_stderr_read, HANDLE_FLAG_INHERIT, 0);
  }

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = capture_stdin ? child_stdin_read : GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = capture_stderr ? child_stderr_write : GetStdHandle(STD_ERROR_HANDLE);

  std::string mutable_command = command_line;
  const BOOL ok = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child.process);
  if (child_stdin_read != nullptr) CloseHandle(child_stdin_read);
  if (child_stderr_write != nullptr) CloseHandle(child_stderr_write);
  child.started = ok != FALSE;
  child.stdin_write = parent_stdin_write;
  child.stderr_read = parent_stderr_read;
  return child;
}

// Reads from the child's stderr until a line containing the marker is seen. This
// is a blocking read: it is synchronisation, not a timeout.
[[nodiscard]] bool read_until(HANDLE pipe, const std::string& marker, std::string* transcript) {
  std::string buffer;
  char chunk[256];
  DWORD read = 0;
  while (true) {
    if (!ReadFile(pipe, chunk, sizeof(chunk), &read, nullptr) || read == 0) {
      return transcript->find(marker) != std::string::npos;
    }
    buffer.append(chunk, read);
    transcript->append(chunk, read);
    if (buffer.find(marker) != std::string::npos) {
      return true;
    }
  }
}

[[nodiscard]] DWORD wait_for(Child& child) {
  WaitForSingleObject(child.process.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(child.process.hProcess, &code);
  return code;
}

void reap(Child& child) {
  if (child.stdin_write != nullptr) CloseHandle(child.stdin_write);
  if (child.stderr_read != nullptr) CloseHandle(child.stderr_read);
  if (child.started) {
    CloseHandle(child.process.hThread);
    CloseHandle(child.process.hProcess);
  }
}

[[nodiscard]] std::string write_file(const std::string& path, const std::string& content) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return {};
  }
  std::fwrite(content.data(), 1, content.size(), file);
  std::fclose(file);
  return path;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: fel_process_tests <path-to-fel-cli>\n");
    return 2;
  }
  const std::string cli = argv[1];
  const auto base = std::filesystem::temp_directory_path() / "fel-process-lock-test";
  std::error_code ec;
  std::filesystem::remove_all(base, ec);
  std::filesystem::create_directories(base, ec);
  const std::string store = (base / "store").string();
  const std::string policy_path = (base / "policy.json").string();
  const std::string evidence_a = (base / "a.jsonl").string();
  const std::string evidence_b = (base / "b.jsonl").string();

  // The default policy is written explicitly so the child processes open the
  // store with exactly the policy the test expects.
  static_cast<void>(write_file(policy_path, "{\"format_version\":1}"));
  static_cast<void>(write_file(evidence_a, std::string()));
  static_cast<void>(write_file(evidence_b, std::string()));

  // Step 0: create the store and a period with the CLI itself.
  {
    const std::string line = quote(cli) + " init --store " + quote(store) + " --policy " +
                             quote(policy_path) + " --period-start 2024-01-01T00:00:00Z" +
                             " --period-end 2024-01-01T01:00:00Z" + " --as-of 2024-01-01T01:00:00Z";
    Child child = start_captured(line, false, false);
    check(child.started, "cli init starts");
    if (!child.started) return 1;
    check(wait_for(child) == 0, "cli init exits successfully");
    reap(child);
  }

  // Step 1: hold the store lock in an independent process.
  Child holder = start_captured(quote(cli) + " lock-hold --store " + quote(store) +
                       " --hold-lock-until-eof --lock-hold-max-ms 30000",
                   true, true);
  check(holder.started, "lock holder starts");
  if (!holder.started) return 1;
  std::string transcript;
  check(read_until(holder.stderr_read, "lock-acquired", &transcript),
        "lock holder reports that it acquired the exclusive lock");

  // Step 2: a second process must be refused immediately.
  {
    Child contender = start_captured(quote(cli) + " verify --store " + quote(store) +
                           " --lock-timeout-ms 0",
                       false, false);
    check(contender.started, "contending process starts");
    const DWORD code = wait_for(contender);
    check(code != 0, "contending process fails while the lock is held");
    reap(contender);
  }

  // Step 3: a patient waiter must succeed once the holder releases.
  Child waiter = start_captured(quote(cli) + " verify --store " + quote(store) +
                       " --lock-timeout-ms 30000",
                   false, false);
  check(waiter.started, "waiting process starts");
  // Release the holder by closing its stdin, which is the documented contract.
  CloseHandle(holder.stdin_write);
  holder.stdin_write = nullptr;
  const DWORD holder_code = wait_for(holder);
  check(holder_code == 0, "lock holder exits cleanly after its stdin closes");
  reap(holder);
  check(wait_for(waiter) == 0, "waiting process succeeds after the lock is released");
  reap(waiter);

  std::filesystem::remove_all(base, ec);
  std::printf("\nprocess lock checks failed: %d\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}

#else

#include <cstdio>

int main(int, char**) {
  std::printf("process lock tests are implemented for Windows only on this host\n");
  return 0;
}

#endif