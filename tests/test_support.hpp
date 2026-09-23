// Congestion Observatory - shared fixtures for the test suites.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_TESTS_TEST_SUPPORT_HPP
#define CONGESTION_TESTS_TEST_SUPPORT_HPP

#include <string>
#include <vector>

#include "congestion/congestion.hpp"

namespace cotest {

// Fixed evaluation instants: no suite depends on the wall clock.
inline constexpr std::int64_t kBaseSeconds = 1000000;

[[nodiscard]] congestion::Timestamp at(std::int64_t seconds_offset);

[[nodiscard]] congestion::Limits test_limits();

// A four node fabric: host1 -> leaf1 -> leaf2 -> host2, with queues, buffers, one path and one
// flow. Deterministic and small enough to reason about by hand.
[[nodiscard]] congestion::GenerationVector test_generation(std::uint64_t epoch = 1,
                                                          std::uint64_t generation = 1,
                                                          std::uint64_t revision = 1);
[[nodiscard]] congestion::Topology make_test_topology(
    const congestion::GenerationVector& generation);

// A source that observes the fabric. The sequence number advances on every produced record, and
// the caller controls boot id, incarnation and generation so fencing can be exercised directly.
struct Source {
  congestion::SourceId id{};
  congestion::BootId boot{1};
  congestion::Incarnation incarnation{1};
  congestion::GenerationVector gen{};
  congestion::Sequence sequence{0};
  congestion::AuthorityLevel authority{congestion::AuthorityLevel::kMeasured};
  congestion::ClockDomain clock{congestion::ClockDomain::kCollectorWallClock};
  std::string transport{"test"};
  std::string collector{"test-collector"};

  [[nodiscard]] congestion::EvidenceRecord record(congestion::EvidenceKind kind,
                                                 const congestion::EvidenceSubject& subject,
                                                 double value,
                                                 congestion::ObservationUnit unit,
                                                 congestion::ValueSemantics semantics,
                                                 congestion::Timestamp observed_at,
                                                 congestion::Duration validity =
                                                     congestion::Duration::from_seconds(5));
  [[nodiscard]] congestion::EvidenceRecord ratio(congestion::EvidenceKind kind,
                                                const congestion::EvidenceSubject& subject,
                                                double value,
                                                congestion::Timestamp observed_at);
  [[nodiscard]] congestion::EvidenceRecord counter(congestion::EvidenceKind kind,
                                                  const congestion::EvidenceSubject& subject,
                                                  double value,
                                                  congestion::ValueSemantics semantics,
                                                  congestion::Timestamp observed_at);
};

[[nodiscard]] Source make_source(const std::string& name,
                                 const congestion::GenerationVector& generation);

// Subjects used throughout the suites.
[[nodiscard]] congestion::EvidenceSubject link_subject(const char* name = "leaf1-leaf2");
[[nodiscard]] congestion::EvidenceSubject queue_subject(const char* name = "leaf1-leaf2/q0");

// Standard runtime configuration bound to the test topology.
[[nodiscard]] congestion::RuntimeConfig make_runtime_config(
    const congestion::Topology& topology, const congestion::Limits& limits);

[[nodiscard]] std::unique_ptr<congestion::Runtime> make_runtime(const congestion::Topology& topology,
                                                              const congestion::Limits& limits);

// Copies a record into the vector of pointers used by the pure classification entry points.
[[nodiscard]] std::vector<const congestion::EvidenceRecord*> pointers_to(
    const std::vector<congestion::EvidenceRecord>& records);

// ---- filesystem and process helpers ----------------------------------------

// Creates a unique temporary directory. Never writes outside the system temporary directory.
[[nodiscard]] std::string unique_temp_dir(const std::string& tag);

// Writes a file inside the directory, returning the full path.
[[nodiscard]] std::string write_temp_file(const std::string& directory, const std::string& name,
                                          const std::string& content);

// A real operating system child process. The independent process transport tests use it both to
// run a client against an in-process server and to run a server for an in-process client.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  [[nodiscard]] bool start(const std::string& command_line);
  [[nodiscard]] bool running() const;
  // Waits for the process to exit and returns its exit code. Returns -1 if it was never started.
  int wait();
  // Terminates the process if it is still running. Returns true when the process is gone.
  bool terminate();

 private:
  void* handle_{nullptr};
  std::uint64_t pid_{0};
};

// Attempts a TCP connection until it succeeds or the attempt budget is exhausted. This is a
// bounded start up synchronisation helper for process level tests, not a correctness timeout.
[[nodiscard]] bool wait_for_port(const std::string& host, std::uint16_t port,
                                 std::uint32_t max_attempts);

// Path of the congestion-observatory executable, injected by the build.
[[nodiscard]] std::string cli_path();

// Quotes a path for the platform shell.
[[nodiscard]] std::string quote(const std::string& text);

// Runs the tool as a real child process with the given arguments and returns its exit code.
// The process is started directly, never through a shell, so no quoting layer can corrupt it.
[[nodiscard]] int run_cli(const std::string& arguments);

// Removes a directory tree; failures are ignored because temporary state is not under test.
void remove_tree(const std::string& path);

}  // namespace cotest

#endif  // CONGESTION_TESTS_TEST_SUPPORT_HPP
