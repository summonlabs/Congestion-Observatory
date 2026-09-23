// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
// Seeded randomized property tests over the defining invariants.
#include "test_framework.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <random>
#include <set>
#include <vector>

using namespace congestion;

namespace {

struct RandomCase {
  std::vector<EvidenceRecord> records;
  ClassificationRequest request{};
  std::vector<EvidenceId> ids;
};

const EvidenceKind kKinds[] = {EvidenceKind::kLinkUtilization, EvidenceKind::kQueueOccupancy,
                               EvidenceKind::kDropCount,       EvidenceKind::kMarkCount,
                               EvidenceKind::kPauseCount,      EvidenceKind::kLatencySample,
                               EvidenceKind::kOfferedDemand,   EvidenceKind::kCapacityAdvertisement,
                               EvidenceKind::kActiveFlowCount};

RandomCase generate_case(std::mt19937_64& engine, std::uint64_t seed) {
  RandomCase generated;
  const GenerationVector generation = cotest::test_generation();
  const EvidenceSubject link = cotest::link_subject();
  const EvidenceSubject queue = cotest::queue_subject();

  std::uniform_int_distribution<int> count_distribution(0, 24);
  std::uniform_int_distribution<int> source_distribution(0, 3);
  std::uniform_int_distribution<int> kind_distribution(0, 8);
  std::uniform_int_distribution<int> offset_distribution(-6, 2);
  std::uniform_int_distribution<int> stale_distribution(0, 9);
  std::uniform_int_distribution<int> flag_distribution(0, 9);
  std::uniform_int_distribution<int> unit_distribution(0, 4);
  std::uniform_int_distribution<int> semantics_distribution(0, 3);
  std::uniform_real_distribution<double> value_distribution(0.0, 1.0);

  std::vector<cotest::Source> sources;
  for (int i = 0; i < 4; ++i) {
    cotest::Source source = cotest::make_source("source-" + std::to_string(i + seed % 7), generation);
    sources.push_back(source);
  }

  const int count = count_distribution(engine);
  for (int i = 0; i < count; ++i) {
    cotest::Source& source = sources[static_cast<std::size_t>(source_distribution(engine))];
    const EvidenceKind kind = kKinds[kind_distribution(engine)];
    const EvidenceSubject subject = (i % 3 == 0) ? queue : link;
    const std::int64_t offset = stale_distribution(engine) == 0 ? -600 : offset_distribution(engine);

    const auto unit = static_cast<ObservationUnit>(1 + unit_distribution(engine));
    const auto semantics = static_cast<ValueSemantics>(1 + semantics_distribution(engine));
    double value = value_distribution(engine);
    if (kind == EvidenceKind::kOfferedDemand || kind == EvidenceKind::kCapacityAdvertisement) {
      value *= 100000000000.0;
    }
    EvidenceRecord record = source.record(kind, subject, value, unit, semantics, cotest::at(offset));

    const int flag = flag_distribution(engine);
    if (flag == 0) {
      record.recovered_from_snapshot = true;
    } else if (flag == 1) {
      record.support = Support::kUnsupported;
      record.value.unit = ObservationUnit::kUnknown;
    } else if (flag == 2) {
      record.completeness = Completeness::kPartial;
    } else if (flag == 3) {
      record.fence.gen = cotest::test_generation(1, 9, 9);
    } else if (flag == 4) {
      record.retired = true;
    }
    record.id = compute_evidence_id(record);
    generated.records.push_back(record);
    generated.ids.push_back(record.id);
  }

  generated.request.subject = link;
  generated.request.window_start = cotest::at(-120);
  generated.request.window_end = cotest::at(3);
  generated.request.evaluated_at = cotest::at(3);
  generated.request.generation = generation;
  generated.request.max_citations = 64;
  return generated;
}

bool has_fresh_supported_impairment(const RandomCase& test_case) {
  const ClassificationPolicy policy;
  for (const EvidenceRecord& record : test_case.records) {
    if (record.subject != test_case.request.subject) {
      continue;
    }
    if (record.support == Support::kUnsupported) {
      continue;
    }
    if (record.fence.gen != test_case.request.generation) {
      continue;
    }
    if (role_of(record.kind) != EvidenceRole::kImpairment) {
      continue;
    }
    const auto freshness = assess_freshness(record, test_case.request.evaluated_at, policy.freshness);
    if (usable_as_deciding_evidence(freshness.freshness)) {
      return true;
    }
  }
  return false;
}

}  // namespace

CO_TEST(property, classification_never_asserts_congestion_without_fresh_impairment) {
  std::mt19937_64 engine(20260101);
  std::size_t asserted = 0;
  std::size_t indeterminate = 0;
  for (std::uint64_t seed = 0; seed < 300; ++seed) {
    const RandomCase test_case = generate_case(engine, seed);
    auto assessment = classify_subject(test_case.request, cotest::pointers_to(test_case.records),
                                       ClassificationPolicy{});
    CO_EXPECT_OK(assessment);
    if (!assessment.ok()) {
      continue;
    }
    if (assessment.value().congestion_asserted) {
      ++asserted;
      CO_EXPECT(has_fresh_supported_impairment(test_case));
      CO_EXPECT(assessment.value().features.impairment_present);
      CO_EXPECT(assessment.value().mechanisms != 0);
      CO_EXPECT(!assessment.value().citations.empty());
    } else if (assessment.value().verdict == Verdict::kIndeterminate) {
      ++indeterminate;
      CO_EXPECT(!assessment.value().blockers.empty());
    }
    // Citations always come from the input.
    const std::set<EvidenceId> known(test_case.ids.begin(), test_case.ids.end());
    for (const EvidenceId& citation : assessment.value().citations) {
      CO_EXPECT(known.find(citation) != known.end());
    }
  }
  CO_EXPECT(asserted > 0);
  CO_EXPECT(indeterminate > 0);
}

CO_TEST(property, classification_is_reproducible_for_every_seed) {
  std::mt19937_64 engine(777001);
  for (std::uint64_t seed = 0; seed < 120; ++seed) {
    const RandomCase test_case = generate_case(engine, seed);
    const auto first = classify_subject(test_case.request, cotest::pointers_to(test_case.records),
                                        ClassificationPolicy{});
    const auto second = classify_subject(test_case.request, cotest::pointers_to(test_case.records),
                                         ClassificationPolicy{});
    CO_EXPECT_OK(first);
    CO_EXPECT_OK(second);
    if (!first.ok() || !second.ok()) {
      continue;
    }
    CO_EXPECT_EQ(first.value().verdict, second.value().verdict);
    CO_EXPECT_EQ(first.value().confidence, second.value().confidence);
    CO_EXPECT_EQ(first.value().severity, second.value().severity);
    CO_EXPECT_EQ(first.value().mechanisms, second.value().mechanisms);
    CO_EXPECT_EQ(first.value().citations, second.value().citations);
    CO_EXPECT_EQ(first.value().features.feature_digest, second.value().features.feature_digest);
    CO_EXPECT_EQ(first.value().explanation.render(), second.value().explanation.render());

    // The feature digest is stable when the record order changes: features are order independent.
    std::vector<EvidenceRecord> reversed(test_case.records.rbegin(), test_case.records.rend());
    const auto permuted = classify_subject(test_case.request, cotest::pointers_to(reversed),
                                           ClassificationPolicy{});
    CO_EXPECT_OK(permuted);
    if (permuted.ok()) {
      CO_EXPECT_EQ(permuted.value().verdict, first.value().verdict);
      CO_EXPECT_EQ(permuted.value().features.feature_digest, first.value().features.feature_digest);
      CO_EXPECT_EQ(permuted.value().confidence, first.value().confidence);
    }
  }
}

CO_TEST(property, recovered_and_retired_evidence_never_asserts) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();

  ClassificationRequest request;
  request.subject = link;
  request.window_start = cotest::at(-10);
  request.window_end = cotest::at(1);
  request.evaluated_at = cotest::at(1);
  request.generation = generation;

  std::vector<EvidenceRecord> records;
  for (int i = 0; i < 6; ++i) {
    EvidenceRecord record = source.ratio(EvidenceKind::kDropCount, link, 0.9, cotest::at(0));
    record.recovered_from_snapshot = true;
    record.id = compute_evidence_id(record);
    records.push_back(record);
  }
  auto recovered = classify_subject(request, cotest::pointers_to(records), ClassificationPolicy{});
  CO_EXPECT_OK(recovered);
  CO_EXPECT(!recovered.value().congestion_asserted);
  CO_EXPECT(recovered.value().has_blocker(blockers::kPersistedNotLive));

  for (EvidenceRecord& record : records) {
    record.recovered_from_snapshot = false;
    record.retired = true;
    record.id = compute_evidence_id(record);
  }
  auto retired = classify_subject(request, cotest::pointers_to(records), ClassificationPolicy{});
  CO_EXPECT_OK(retired);
  CO_EXPECT(!retired.value().congestion_asserted);
  CO_EXPECT(retired.value().has_blocker(blockers::kRetiredIncarnation));
}

CO_TEST(property, episode_history_is_reproducible_from_the_same_observation_sequence) {
  std::mt19937_64 engine(4242);
  for (std::uint64_t seed = 0; seed < 20; ++seed) {
    const auto generation = cotest::test_generation();
    const EvidenceSubject link = cotest::link_subject();
    EpisodeRegistry left(cotest::test_limits());
    EpisodeRegistry right(cotest::test_limits());
    EpisodePolicy policy;

    std::uniform_int_distribution<int> value_distribution(0, 3);
    std::uniform_int_distribution<int> source_distribution(0, 2);
    for (int step = 0; step < 40; ++step) {
      cotest::Source source = cotest::make_source(
          "source-" + std::to_string(source_distribution(engine)), generation);
      ClassificationRequest request;
      request.subject = link;
      request.window_start = cotest::at(step - 10);
      request.window_end = cotest::at(step);
      request.evaluated_at = cotest::at(step);
      request.generation = generation;
      std::vector<EvidenceRecord> records;
      const int severity = value_distribution(engine);
      switch (severity) {
        case 0:
          records.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.95, cotest::at(step)));
          break;
        case 1:
          records.push_back(source.ratio(EvidenceKind::kDropCount, link, 0.001, cotest::at(step)));
          break;
        case 2:
          records.push_back(source.ratio(EvidenceKind::kDropCount, link, 0.5, cotest::at(step)));
          records.push_back(source.ratio(EvidenceKind::kMarkCount, link, 0.5, cotest::at(step)));
          break;
        default:
          records.push_back(source.ratio(EvidenceKind::kQueueOccupancy, link, 0.8, cotest::at(step)));
          break;
      }
      auto assessment = classify_subject(request, cotest::pointers_to(records),
                                         ClassificationPolicy{});
      CO_EXPECT_OK(assessment);
      if (!assessment.ok()) {
        continue;
      }
      auto left_update = left.observe(assessment.value(), policy, cotest::at(step));
      auto right_update = right.observe(assessment.value(), policy, cotest::at(step));
      CO_EXPECT_OK(left_update);
      CO_EXPECT_OK(right_update);
      if (left_update.ok() && right_update.ok()) {
        CO_EXPECT_EQ(left_update.value().id, right_update.value().id);
        CO_EXPECT_EQ(left_update.value().kind, right_update.value().kind);
        CO_EXPECT_EQ(left_update.value().revision.value(), right_update.value().revision.value());
      }
    }
    const auto left_episodes = left.all();
    const auto right_episodes = right.all();
    CO_EXPECT_EQ(left_episodes.size(), right_episodes.size());
    for (std::size_t i = 0; i < left_episodes.size() && i < right_episodes.size(); ++i) {
      CO_EXPECT_EQ(left_episodes[i].id, right_episodes[i].id);
      CO_EXPECT_EQ(left_episodes[i].revision.value(), right_episodes[i].revision.value());
      CO_EXPECT_EQ(left_episodes[i].state, right_episodes[i].state);
      CO_EXPECT_EQ(left_episodes[i].transitions.size(), right_episodes[i].transitions.size());
      CO_EXPECT_EQ(left_episodes[i].observations, right_episodes[i].observations);
    }

    // The same episodes survive a snapshot round trip unchanged.
    SnapshotContent content;
    content.metadata.policy_version = ClassificationPolicy{}.version;
    content.metadata.policy_digest = ClassificationPolicy{}.digest();
    content.metadata.limits_digest = Limits{}.digest();
    content.metadata.created_at = cotest::at(0);
    content.topology = cotest::make_test_topology(generation);
    content.metadata.topology_digest = content.topology.digest();
    content.episodes = left_episodes;
    auto encoded = encode_snapshot(content, cotest::test_limits());
    CO_EXPECT_OK(encoded);
    if (encoded.ok()) {
      auto decoded = decode_snapshot(encoded.value(), cotest::test_limits(), ClassificationPolicy{});
      CO_EXPECT_OK(decoded);
      if (decoded.ok()) {
        CO_EXPECT_EQ(decoded.value().episodes.size(), left_episodes.size());
        for (std::size_t i = 0; i < decoded.value().episodes.size(); ++i) {
          CO_EXPECT_EQ(decoded.value().episodes[i].id, left_episodes[i].id);
          CO_EXPECT_EQ(decoded.value().episodes[i].revision.value(),
                       left_episodes[i].revision.value());
        }
      }
    }
  }
}

CO_TEST(property, evidence_store_never_exceeds_its_bounds) {
  std::mt19937_64 engine(31337);
  Limits limits = cotest::test_limits();
  limits.max_retained_evidence = 32;
  limits.max_evidence_per_subject = 8;
  limits.max_window_records = 32;
  limits.max_snapshot_evidence = 32;
  CO_EXPECT(limits.validate().ok());
  EvidenceStore store(limits);
  const auto generation = cotest::test_generation();
  IngestPolicy policy;
  std::uint64_t attempts = 0;
  std::uint64_t accepted = 0;

  for (std::uint64_t seed = 0; seed < 200; ++seed) {
    cotest::Source source = cotest::make_source("source-" + std::to_string(seed % 5), generation);
    const int count = static_cast<int>(seed % 7) + 1;
    for (int i = 0; i < count; ++i) {
      const EvidenceSubject subject = (i % 2 == 0) ? cotest::link_subject() : cotest::queue_subject();
      EvidenceRecord record = source.ratio(EvidenceKind::kLinkUtilization, subject, 0.5,
                                           cotest::at(static_cast<std::int64_t>(seed % 3)));
      ++attempts;
      auto outcome = store.ingest(record, cotest::at(static_cast<std::int64_t>(seed % 3)), policy);
      CO_EXPECT_OK(outcome);
      if (outcome.ok() && outcome.value().stored) {
        ++accepted;
      }
    }
    CO_EXPECT(store.size() <= limits.max_retained_evidence);
    CO_EXPECT(store.stats().retained <= limits.max_retained_evidence);
  }
  CO_EXPECT_EQ(store.stats().accepted, accepted);
  CO_EXPECT_EQ(store.stats().accepted + store.stats().rejected_fence, attempts);
}

CO_TEST(property, agreement_is_symmetric_and_order_independent) {
  std::mt19937_64 engine(9091);
  const auto generation = cotest::test_generation();
  const EvidenceSubject link = cotest::link_subject();
  for (std::uint64_t seed = 0; seed < 60; ++seed) {
    std::vector<EvidenceRecord> records;
    const int count = 1 + static_cast<int>(seed % 5);
    for (int i = 0; i < count; ++i) {
      cotest::Source source = cotest::make_source("source-" + std::to_string(i), generation);
      const double value = (i % 2 == 0) ? 0.9 : 0.9 - 0.5 * ((seed % 3) == 0 ? 1.0 : 0.0);
      records.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, value, cotest::at(0)));
    }
    const auto forward = assess_agreement(cotest::pointers_to(records), 0.10, 64);
    std::vector<EvidenceRecord> reversed(records.rbegin(), records.rend());
    const auto backward = assess_agreement(cotest::pointers_to(reversed), 0.10, 64);
    CO_EXPECT_EQ(forward.agreement, backward.agreement);
    CO_EXPECT_EQ(forward.distinct_sources, backward.distinct_sources);
    CO_EXPECT_EQ(forward.citations, backward.citations);
    CO_EXPECT_EQ(forward.relative_spread, backward.relative_spread);
  }
}
