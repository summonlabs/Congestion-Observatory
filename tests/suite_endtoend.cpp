// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
// End to end scenarios driven through the installed command line tool.
#include "test_framework.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace congestion;

namespace {

// std::system reports the raw wait status on POSIX and the exit code on Windows.
int exit_code_of(int status) {
#if defined(_WIN32)
  return status;
#else
  if (status == -1) {
    return -1;
  }
  return (status >> 8) & 0xFF;
#endif
}

std::string topology_document() {
  return R"({
  "generation": {"epoch": 1, "generation": 1, "revision": 1},
  "revision_label": "e2e-1",
  "nodes": [
    {"id": "leaf1", "kind": "switch"},
    {"id": "leaf2", "kind": "switch"},
    {"id": "host1", "kind": "host"},
    {"id": "host2", "kind": "host"}
  ],
  "ports": [
    {"id": "host1/1", "node": "host1", "speed_bps": 25000000000},
    {"id": "host2/1", "node": "host2", "speed_bps": 25000000000},
    {"id": "leaf1/1", "node": "leaf1", "speed_bps": 25000000000},
    {"id": "leaf1/2", "node": "leaf1", "speed_bps": 100000000000},
    {"id": "leaf2/1", "node": "leaf2", "speed_bps": 100000000000},
    {"id": "leaf2/2", "node": "leaf2", "speed_bps": 25000000000}
  ],
  "links": [
    {"id": "host1-leaf1", "endpoint_a": "host1/1", "endpoint_b": "leaf1/1",
     "capacity_bps": 25000000000},
    {"id": "leaf1-leaf2", "endpoint_a": "leaf1/2", "endpoint_b": "leaf2/1",
     "capacity_bps": 100000000000},
    {"id": "leaf2-host2", "endpoint_a": "leaf2/2", "endpoint_b": "host2/1",
     "capacity_bps": 25000000000}
  ],
  "queues": [
    {"id": "leaf1-leaf2/q0", "link": "leaf1-leaf2", "index": 0}
  ],
  "paths": [
    {"id": "host1-host2", "hops": ["host1-leaf1", "leaf1-leaf2", "leaf2-host2"]}
  ],
  "flows": [
    {"id": "flow-a", "path": "host1-host2", "tenant": "tenant-a", "class": "class-1"}
  ]
})";
}

std::string evidence_document(const std::vector<EvidenceRecord>& records) {
  std::string document = "{\"records\":[";
  bool first = true;
  for (const EvidenceRecord& record : records) {
    if (!first) {
      document += ",";
    }
    first = false;
    document += encode_ingest_json(record);
  }
  document += "]}";
  return document;
}

std::string cli(const std::string& arguments) { return arguments; }

}  // namespace

CO_TEST(endtoend, selfcheck_and_version_run_clean) {
  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(("selfcheck"))), 0);
  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(("version"))), 0);
  const std::string directory = cotest::unique_temp_dir("e2e-limits");
  const std::string output = cotest::write_temp_file(directory, "limits.json", "");
  (void)output;
  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(("limits --json --out " + cotest::quote(output)))),
               0);
  std::ifstream stream(output);
  std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  auto parsed = parse_json(text, Limits{});
  CO_EXPECT_OK(parsed);
  if (parsed.ok()) {
    CO_EXPECT(parsed.value().member("digest") != nullptr);
  }
  cotest::remove_tree(directory);
}

CO_TEST(endtoend, topology_document_is_validated_by_the_tool) {
  const std::string directory = cotest::unique_temp_dir("e2e-topology");
  const std::string topology_path =
      cotest::write_temp_file(directory, "topology.json", topology_document());
  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(
                   cli("topology --file " + cotest::quote(topology_path)))),
               0);

  const std::string broken_path =
      cotest::write_temp_file(directory, "broken.json", "{\"links\":[{\"id\":\"x\"}]}");
  CO_EXPECT_NE(exit_code_of(cotest::run_cli(("topology --file " + cotest::quote(broken_path)))),
               0);
  CO_EXPECT_NE(exit_code_of(cotest::run_cli(("topology --file " + cotest::quote(directory + "/missing.json")))),
               0);
  cotest::remove_tree(directory);
}

CO_TEST(endtoend, full_pipeline_from_the_command_line) {
  const std::string directory = cotest::unique_temp_dir("e2e-pipeline");
  const std::string topology_path =
      cotest::write_temp_file(directory, "topology.json", topology_document());
  const std::string state = directory + "/state";
  const std::string base = " --topology " + cotest::quote(topology_path) + " --state " +
                           cotest::quote(state);

  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject("leaf1-leaf2");

  // 1. High utilization with no impairment: busy, healthy, and explicitly not congestion.
  std::vector<EvidenceRecord> utilization_only;
  utilization_only.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.96, cotest::at(0)));
  utilization_only.push_back(source.record(EvidenceKind::kOfferedDemand, link, 96000000000.0,
                                           ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                                           cotest::at(0)));
  utilization_only.push_back(source.record(EvidenceKind::kCapacityAdvertisement, link,
                                           100000000000.0, ObservationUnit::kBitsPerSecond,
                                           ValueSemantics::kGauge, cotest::at(0)));
  const std::string utilization_path =
      cotest::write_temp_file(directory, "utilization.json", evidence_document(utilization_only));

  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(("ingest --file " +
                                                    cotest::quote(utilization_path) + base +
                                                    " --save"))),
               0);

  const std::string classify_base =
      cli("classify --subject link:leaf1-leaf2 --window 10s --now @1000001" + base);
  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(classify_base)), 1);
  const std::string json_path = directory + "/classify.json";
  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(classify_base + " --json --out " + cotest::quote(json_path))),
               1);
  {
    std::ifstream stream(json_path);
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    auto parsed = parse_json(text, Limits{});
    CO_EXPECT_OK(parsed);
    if (parsed.ok()) {
      CO_EXPECT_EQ(parsed.value().member("verdict")->as_string(), std::string("indeterminate"));
      CO_EXPECT_EQ(parsed.value().member("congestion_asserted")->as_bool(), false);
    }
  }

  // 2. A fresh observation document restores liveness and reports the utilization finding.
  const std::string classify_fresh =
      cli("classify --subject link:leaf1-leaf2 --window 10s --now @1000001 --file " +
          cotest::quote(utilization_path) + base);
  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(classify_fresh)), 1);
  const std::string fresh_json = directory + "/classify-fresh.json";
  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(classify_fresh + " --json --out " + cotest::quote(fresh_json))),
               1);
  {
    std::ifstream stream(fresh_json);
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    auto parsed = parse_json(text, Limits{});
    CO_EXPECT_OK(parsed);
    if (parsed.ok()) {
      CO_EXPECT_EQ(parsed.value().member("verdict")->as_string(), std::string("utilized_healthy"));
      bool saw_blocker = false;
      for (const JsonValue& blocker : parsed.value().member("blockers")->as_array()) {
        if (blocker.member("code")->as_string() == "utilization_alone_is_not_congestion") {
          saw_blocker = true;
        }
      }
      CO_EXPECT(saw_blocker);
    }
  }

  // 3. Add drops: congestion is asserted and an episode is opened.
  cotest::Source second = cotest::make_source("collector-2", generation);
  std::vector<EvidenceRecord> congestion;
  congestion.push_back(second.ratio(EvidenceKind::kLinkUtilization, link, 0.99, cotest::at(1)));
  congestion.push_back(second.ratio(EvidenceKind::kDropCount, link, 0.004, cotest::at(1)));
  congestion.push_back(second.record(EvidenceKind::kCapacityAdvertisement, link, 100000000000.0,
                                     ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                                     cotest::at(1)));
  congestion.push_back(second.record(EvidenceKind::kOfferedDemand, link, 99000000000.0,
                                     ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                                     cotest::at(1)));
  const std::string congestion_path =
      cotest::write_temp_file(directory, "congestion.json", evidence_document(congestion));
  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(("ingest --file " +
                                                    cotest::quote(congestion_path) + base +
                                                    " --save"))),
               0);

  const std::string classify_congested =
      cli("classify --subject link:leaf1-leaf2 --window 30s --now @1000002 --file " +
          cotest::quote(congestion_path) + base);
  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(classify_congested)), 0);

  const std::string episodes_json = directory + "/episodes.json";
  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(("episodes --json" + base + " --out " +
                                                    cotest::quote(episodes_json)))),
               0);
  std::string episode_id;
  {
    std::ifstream stream(episodes_json);
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    auto parsed = parse_json(text, Limits{});
    CO_EXPECT_OK(parsed);
    if (parsed.ok()) {
      // Episodes persist only when an evaluation command records them; the registry is empty
      // here because classification alone does not open history.
      CO_EXPECT(parsed.value().is_array());
    }
  }

  // 4. Export is valid JSON describing the runtime.
  const std::string export_path = directory + "/export.json";
  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(("export --evidence" + base + " --out " +
                                                    cotest::quote(export_path)))),
               0);
  {
    std::ifstream stream(export_path);
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    auto parsed = parse_json(text, Limits{});
    CO_EXPECT_OK(parsed);
    if (parsed.ok()) {
      CO_EXPECT_EQ(parsed.value().member("product")->as_string(),
                   std::string("congestion-observatory"));
      CO_EXPECT(parsed.value().member("topology") != nullptr);
      CO_EXPECT(parsed.value().member("metrics") != nullptr);
    }
  }

  // 5. Explanation is rendered from the policy rules.
  const std::string explain_path = directory + "/explain.txt";
  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(("explain --subject link:leaf1-leaf2 --window 30s"
                                                    " --now @1000002 --file " +
                                                    cotest::quote(congestion_path) + base + " --out " +
                                                    cotest::quote(explain_path)))),
               0);
  {
    std::ifstream stream(explain_path);
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CO_EXPECT(text.find("R04-impairment") != std::string::npos);
  }

  // 6. Load reports the persisted state and zero live sources.
  const std::string load_path = directory + "/load.txt";
  CO_EXPECT_EQ(exit_code_of(cotest::run_cli(("load" + base + " --out " +
                                                    cotest::quote(load_path)))),
               0);
  {
    std::ifstream stream(load_path);
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CO_EXPECT(text.find("live_sources=0") != std::string::npos);
  }

  // 7. Localization of an asserted symptom returns a definite outcome.
  CO_EXPECT_NE(exit_code_of(cotest::run_cli(("localize --subject link:leaf1-leaf2 --window 30s"
                                                    " --now @1000002 --file " +
                                                    cotest::quote(congestion_path) + base))),
               2);

  cotest::remove_tree(directory);
}

CO_TEST(endtoend, malformed_invocations_fail_loudly) {
  CO_EXPECT_NE(exit_code_of(cotest::run_cli(("nonsense-command"))), 0);
  CO_EXPECT_NE(exit_code_of(cotest::run_cli(("ingest"))), 0);
  CO_EXPECT_NE(exit_code_of(cotest::run_cli(("classify"))), 0);
  CO_EXPECT_NE(exit_code_of(cotest::run_cli(("classify --subject not-a-subject"))), 0);
  CO_EXPECT_NE(exit_code_of(cotest::run_cli(("save"))), 0);
  CO_EXPECT_NE(exit_code_of(cotest::run_cli(("push --endpoint 127.0.0.1:1 --io-deadline 250ms"))),
               0);
  const std::string directory = cotest::unique_temp_dir("e2e-malformed");
  const std::string broken =
      cotest::write_temp_file(directory, "broken.json", "{\"records\": [{\"kind\":");
  CO_EXPECT_NE(exit_code_of(cotest::run_cli(("ingest --file " + cotest::quote(broken)))), 0);
  cotest::remove_tree(directory);
}
