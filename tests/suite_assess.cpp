// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "test_framework.hpp"
#include "test_support.hpp"

#include <algorithm>

using namespace congestion;

namespace {

ClassificationRequest make_request(const EvidenceSubject& subject, std::int64_t now_offset,
                                   const GenerationVector& generation) {
  ClassificationRequest request;
  request.subject = subject;
  request.window_start = cotest::at(now_offset - 60);
  request.window_end = cotest::at(now_offset);
  request.evaluated_at = cotest::at(now_offset);
  request.generation = generation;
  request.max_citations = 64;
  return request;
}

bool has_citation(const CongestionAssessment& assessment, const EvidenceId& id) {
  return std::find(assessment.citations.begin(), assessment.citations.end(), id) !=
         assessment.citations.end();
}

}  // namespace

CO_TEST(assess, utilization_alone_is_never_congestion) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();

  std::vector<EvidenceRecord> records;
  records.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.97, cotest::at(0)));
  records.push_back(source.record(EvidenceKind::kCapacityAdvertisement, link, 100000000000.0,
                                  ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                                  cotest::at(0)));
  records.push_back(source.record(EvidenceKind::kOfferedDemand, link, 40000000000.0,
                                  ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                                  cotest::at(0)));
  records.push_back(source.ratio(EvidenceKind::kQueueOccupancy, link, 0.10, cotest::at(0)));

  auto assessment = classify_subject(make_request(link, 1, generation), cotest::pointers_to(records),
                                     ClassificationPolicy{});
  CO_EXPECT_OK(assessment);
  CO_EXPECT_EQ(assessment.value().verdict, Verdict::kUtilizedHealthy);
  CO_EXPECT(!assessment.value().congestion_asserted);
  CO_EXPECT(!is_congestion_assertion(assessment.value().verdict));
  CO_EXPECT(assessment.value().has_blocker(blockers::kUtilizationAlone));
  CO_EXPECT(assessment.value().features.utilization_present);
  CO_EXPECT(assessment.value().features.utilization_exceeds_high);
  CO_EXPECT(!assessment.value().features.impairment_present);
  CO_EXPECT(assessment.value().confidence > 0);
  CO_EXPECT(!assessment.value().citations.empty());
  CO_EXPECT_EQ(assessment.value().mechanisms, MechanismSet{0});
  CO_EXPECT(assessment.value().severity <= Severity::kInformational);
}

CO_TEST(assess, stale_pressure_cannot_prove_current_congestion) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();

  std::vector<EvidenceRecord> only_stale;
  only_stale.push_back(source.ratio(EvidenceKind::kQueueOccupancy, link, 0.95, cotest::at(-600)));
  auto stale_only = classify_subject(make_request(link, 0, generation),
                                     cotest::pointers_to(only_stale), ClassificationPolicy{});
  CO_EXPECT_OK(stale_only);
  CO_EXPECT_EQ(stale_only.value().verdict, Verdict::kIndeterminate);
  CO_EXPECT(!stale_only.value().congestion_asserted);
  CO_EXPECT(stale_only.value().has_blocker(blockers::kStalePressureCannotProveCurrent));
  CO_EXPECT(stale_only.value().features.pressure_seen_not_fresh);
  CO_EXPECT(!stale_only.value().features.pressure_present);

  // With fresh utilization but only stale pressure, the verdict must stay non-congestion and
  // must say why.
  std::vector<EvidenceRecord> mixed;
  mixed.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.95, cotest::at(0)));
  mixed.push_back(source.ratio(EvidenceKind::kQueueOccupancy, link, 0.95, cotest::at(-600)));
  auto mixed_assessment = classify_subject(make_request(link, 1, generation),
                                           cotest::pointers_to(mixed), ClassificationPolicy{});
  CO_EXPECT_OK(mixed_assessment);
  CO_EXPECT(!mixed_assessment.value().congestion_asserted);
  CO_EXPECT_NE(mixed_assessment.value().verdict, Verdict::kPressureObserved);
  CO_EXPECT(mixed_assessment.value().has_blocker(blockers::kStalePressureCannotProveCurrent));
}

CO_TEST(assess, fresh_pressure_is_reported_as_pressure_not_congestion) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject queue = cotest::queue_subject();

  std::vector<EvidenceRecord> records;
  records.push_back(source.ratio(EvidenceKind::kQueueOccupancy, queue, 0.85, cotest::at(0)));
  records.push_back(source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.80,
                                 cotest::at(0)));
  auto assessment = classify_subject(make_request(queue, 1, generation),
                                     cotest::pointers_to(records), ClassificationPolicy{});
  CO_EXPECT_OK(assessment);
  CO_EXPECT_EQ(assessment.value().verdict, Verdict::kPressureObserved);
  CO_EXPECT(!assessment.value().congestion_asserted);
  CO_EXPECT(assessment.value().has_blocker(blockers::kUtilizationAlone) == false);
  CO_EXPECT(assessment.value().features.pressure_exceeds_ratio);
}

CO_TEST(assess, fresh_impairment_confirms_congestion) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();

  std::vector<EvidenceRecord> records;
  records.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.99, cotest::at(0)));
  records.push_back(source.ratio(EvidenceKind::kDropCount, link, 0.001, cotest::at(0)));
  records.push_back(source.record(EvidenceKind::kCapacityAdvertisement, link, 100000000000.0,
                                  ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                                  cotest::at(0)));
  records.push_back(source.record(EvidenceKind::kOfferedDemand, link, 99000000000.0,
                                  ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                                  cotest::at(0)));
  auto assessment = classify_subject(make_request(link, 1, generation), cotest::pointers_to(records),
                                     ClassificationPolicy{});
  CO_EXPECT_OK(assessment);
  CO_EXPECT_EQ(assessment.value().verdict, Verdict::kCongestionConfirmed);
  CO_EXPECT(assessment.value().congestion_asserted);
  CO_EXPECT(has_mechanism(assessment.value().mechanisms, Mechanism::kPacketDrop));
  CO_EXPECT_EQ(primary_mechanism(assessment.value().mechanisms), Mechanism::kPacketDrop);
  CO_EXPECT(!assessment.value().citations.empty());
  CO_EXPECT(assessment.value().features.drop_rate > 0.0);
  CO_EXPECT(!assessment.value().has_blocker(blockers::kUtilizationAlone));
}

CO_TEST(assess, marginal_and_unusable_impairment_do_not_confirm) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();

  std::vector<EvidenceRecord> below_threshold;
  below_threshold.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.99, cotest::at(0)));
  below_threshold.push_back(source.ratio(EvidenceKind::kDropCount, link, 1e-9, cotest::at(0)));
  auto marginal = classify_subject(make_request(link, 1, generation),
                                   cotest::pointers_to(below_threshold), ClassificationPolicy{});
  CO_EXPECT_OK(marginal);
  CO_EXPECT(!marginal.value().congestion_asserted);
  CO_EXPECT_EQ(marginal.value().verdict, Verdict::kUtilizedHealthy);
  CO_EXPECT(marginal.value().has_blocker(blockers::kImpairmentBelowThreshold));

  // A cumulative counter is a lifetime total: it cannot prove present harm.
  std::vector<EvidenceRecord> counter_only;
  counter_only.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.99, cotest::at(0)));
  counter_only.push_back(source.counter(EvidenceKind::kDropCount, link, 5000.0,
                                        ValueSemantics::kCumulativeCounter, cotest::at(0)));
  auto counter = classify_subject(make_request(link, 1, generation),
                                  cotest::pointers_to(counter_only), ClassificationPolicy{});
  CO_EXPECT_OK(counter);
  CO_EXPECT(!counter.value().congestion_asserted);
  CO_EXPECT(counter.value().has_blocker(blockers::kCounterWithoutDelta));

  // A delta counter above zero is harm actually observed in this window.
  std::vector<EvidenceRecord> delta_only;
  delta_only.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.99, cotest::at(0)));
  delta_only.push_back(source.counter(EvidenceKind::kDropCount, link, 12.0,
                                      ValueSemantics::kDeltaCounter, cotest::at(0)));
  auto delta = classify_subject(make_request(link, 1, generation), cotest::pointers_to(delta_only),
                                ClassificationPolicy{});
  CO_EXPECT_OK(delta);
  CO_EXPECT(delta.value().congestion_asserted);
  CO_EXPECT(delta.value().features.impairment_unquantified);
}

CO_TEST(assess, latency_inflation_needs_a_baseline) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();

  std::vector<EvidenceRecord> single_sample;
  single_sample.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.95, cotest::at(0)));
  single_sample.push_back(source.record(EvidenceKind::kLatencySample, link, 500000.0,
                                        ObservationUnit::kNanoseconds, ValueSemantics::kGauge,
                                        cotest::at(0)));
  auto single = classify_subject(make_request(link, 1, generation),
                                 cotest::pointers_to(single_sample), ClassificationPolicy{});
  CO_EXPECT_OK(single);
  CO_EXPECT(!single.value().congestion_asserted);
  CO_EXPECT_EQ(single.value().features.latency_factor, 0.0);

  std::vector<EvidenceRecord> baseline_and_spike;
  baseline_and_spike.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.95, cotest::at(0)));
  baseline_and_spike.push_back(source.record(EvidenceKind::kLatencySample, link, 100000.0,
                                             ObservationUnit::kNanoseconds, ValueSemantics::kGauge,
                                             cotest::at(0)));
  baseline_and_spike.push_back(source.record(EvidenceKind::kLatencySample, link, 400000.0,
                                             ObservationUnit::kNanoseconds, ValueSemantics::kGauge,
                                             cotest::at(0)));
  auto inflated = classify_subject(make_request(link, 1, generation),
                                   cotest::pointers_to(baseline_and_spike), ClassificationPolicy{});
  CO_EXPECT_OK(inflated);
  CO_EXPECT(inflated.value().congestion_asserted);
  CO_EXPECT(has_mechanism(inflated.value().mechanisms, Mechanism::kLatencyInflation));
  CO_EXPECT(inflated.value().features.latency_factor >= 4.0);
}

CO_TEST(assess, saturation_is_not_congestion) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();

  std::vector<EvidenceRecord> records;
  records.push_back(source.record(EvidenceKind::kCapacityAdvertisement, link, 100000000000.0,
                                  ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                                  cotest::at(0)));
  records.push_back(source.record(EvidenceKind::kOfferedDemand, link, 130000000000.0,
                                  ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                                  cotest::at(0)));
  records.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.99, cotest::at(0)));
  auto assessment = classify_subject(make_request(link, 1, generation), cotest::pointers_to(records),
                                     ClassificationPolicy{});
  CO_EXPECT_OK(assessment);
  CO_EXPECT_EQ(assessment.value().verdict, Verdict::kSaturated);
  CO_EXPECT(!assessment.value().congestion_asserted);
  CO_EXPECT(assessment.value().features.demand_exceeds_capacity);
  CO_EXPECT(assessment.value().features.demand_capacity_ratio > 1.0);
}

CO_TEST(assess, contention_requires_load) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();

  std::vector<EvidenceRecord> loaded;
  loaded.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.85, cotest::at(0)));
  loaded.push_back(source.record(EvidenceKind::kActiveFlowCount, link, 12.0,
                                 ObservationUnit::kCount, ValueSemantics::kGauge, cotest::at(0)));
  auto with_load = classify_subject(make_request(link, 1, generation), cotest::pointers_to(loaded),
                                    ClassificationPolicy{});
  CO_EXPECT_OK(with_load);
  CO_EXPECT_EQ(with_load.value().verdict, Verdict::kContentionObserved);
  CO_EXPECT(!with_load.value().congestion_asserted);

  std::vector<EvidenceRecord> idle_contention;
  idle_contention.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.10, cotest::at(0)));
  idle_contention.push_back(source.record(EvidenceKind::kActiveFlowCount, link, 12.0,
                                          ObservationUnit::kCount, ValueSemantics::kGauge,
                                          cotest::at(0)));
  auto idle = classify_subject(make_request(link, 1, generation),
                               cotest::pointers_to(idle_contention), ClassificationPolicy{});
  CO_EXPECT_OK(idle);
  CO_EXPECT_EQ(idle.value().verdict, Verdict::kIdle);
}

CO_TEST(assess, conflicting_sources_stay_visible) {
  const auto generation = cotest::test_generation();
  cotest::Source high = cotest::make_source("collector-high", generation);
  cotest::Source low = cotest::make_source("collector-low", generation);
  const EvidenceSubject link = cotest::link_subject();

  std::vector<EvidenceRecord> records;
  EvidenceRecord high_record = high.ratio(EvidenceKind::kLinkUtilization, link, 0.98, cotest::at(0));
  EvidenceRecord low_record = low.ratio(EvidenceKind::kLinkUtilization, link, 0.05, cotest::at(0));
  records.push_back(high_record);
  records.push_back(low_record);
  auto assessment = classify_subject(make_request(link, 1, generation), cotest::pointers_to(records),
                                     ClassificationPolicy{});
  CO_EXPECT_OK(assessment);
  CO_EXPECT(assessment.value().has_blocker(blockers::kConflictingSources));
  CO_EXPECT_EQ(assessment.value().features.utilization_agreement, Agreement::kConflicting);
  CO_EXPECT(has_citation(assessment.value(), high_record.id));
  CO_EXPECT(has_citation(assessment.value(), low_record.id));
  CO_EXPECT(!assessment.value().congestion_asserted);
  CO_EXPECT(assessment.value().basis.conflicting_features >= 1);
}

CO_TEST(assess, unsupported_and_incomplete_evidence_are_named) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();

  EvidenceRecord unsupported = source.ratio(EvidenceKind::kDropCount, link, 0.0, cotest::at(0));
  unsupported.support = Support::kUnsupported;
  unsupported.id = compute_evidence_id(unsupported);
  EvidenceRecord utilization = source.ratio(EvidenceKind::kLinkUtilization, link, 0.95, cotest::at(0));
  std::vector<EvidenceRecord> records{unsupported, utilization};
  auto assessment = classify_subject(make_request(link, 1, generation), cotest::pointers_to(records),
                                     ClassificationPolicy{});
  CO_EXPECT_OK(assessment);
  CO_EXPECT(assessment.value().has_blocker(blockers::kUnsupportedEvidence));
  CO_EXPECT_EQ(assessment.value().basis.unsupported_records, static_cast<std::size_t>(1));
  CO_EXPECT(!assessment.value().congestion_asserted);

  EvidenceRecord incomplete = source.ratio(EvidenceKind::kLinkUtilization, link, 0.95, cotest::at(0));
  incomplete.completeness = Completeness::kIncomplete;
  incomplete.id = compute_evidence_id(incomplete);
  std::vector<EvidenceRecord> incomplete_records{incomplete};
  auto incomplete_assessment =
      classify_subject(make_request(link, 1, generation), cotest::pointers_to(incomplete_records),
                       ClassificationPolicy{});
  CO_EXPECT_OK(incomplete_assessment);
  CO_EXPECT(incomplete_assessment.value().has_blocker(blockers::kIncompleteEvidence));

  // Only unsupported evidence: the verdict must be indeterminate, never a positive finding.
  std::vector<EvidenceRecord> only_unsupported{unsupported};
  auto only = classify_subject(make_request(link, 1, generation),
                               cotest::pointers_to(only_unsupported), ClassificationPolicy{});
  CO_EXPECT_OK(only);
  CO_EXPECT_EQ(only.value().verdict, Verdict::kIndeterminate);
}

CO_TEST(assess, generation_mismatch_is_reported_and_blocks) {
  const auto requested = cotest::test_generation(2, 2, 2);
  const auto other = cotest::test_generation(1, 1, 1);
  cotest::Source source = cotest::make_source("collector-1", other);
  const EvidenceSubject link = cotest::link_subject();

  std::vector<EvidenceRecord> stale_records;
  stale_records.push_back(source.ratio(EvidenceKind::kDropCount, link, 0.5, cotest::at(0)));
  auto assessment = classify_subject(make_request(link, 1, requested),
                                     cotest::pointers_to(stale_records), ClassificationPolicy{});
  CO_EXPECT_OK(assessment);
  CO_EXPECT_EQ(assessment.value().verdict, Verdict::kIndeterminate);
  CO_EXPECT(assessment.value().has_blocker(blockers::kGenerationMismatch));
  CO_EXPECT_EQ(assessment.value().features.records_generation_mismatch, static_cast<std::size_t>(1));
  CO_EXPECT(!assessment.value().congestion_asserted);

  // An unconstrained request generation accepts every record.
  ClassificationRequest unconstrained = make_request(link, 1, GenerationVector{});
  auto accepted = classify_subject(unconstrained, cotest::pointers_to(stale_records),
                                   ClassificationPolicy{});
  CO_EXPECT_OK(accepted);
  CO_EXPECT(accepted.value().congestion_asserted);
}

CO_TEST(assess, recovered_evidence_is_history_not_liveness) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();
  EvidenceRecord record = source.ratio(EvidenceKind::kDropCount, link, 0.5, cotest::at(0));
  record.recovered_from_snapshot = true;

  std::vector<EvidenceRecord> records{record};
  auto assessment = classify_subject(make_request(link, 1, generation), cotest::pointers_to(records),
                                     ClassificationPolicy{});
  CO_EXPECT_OK(assessment);
  CO_EXPECT(!assessment.value().congestion_asserted);
  CO_EXPECT_EQ(assessment.value().verdict, Verdict::kIndeterminate);
  CO_EXPECT(assessment.value().has_blocker(blockers::kPersistedNotLive));
  CO_EXPECT_EQ(assessment.value().features.records_recovered, static_cast<std::size_t>(1));
}

CO_TEST(assess, no_evidence_is_explicit) {
  const auto generation = cotest::test_generation();
  auto assessment = classify_subject(make_request(cotest::link_subject(), 1, generation), {},
                                     ClassificationPolicy{});
  CO_EXPECT_OK(assessment);
  CO_EXPECT_EQ(assessment.value().verdict, Verdict::kNoEvidence);
  CO_EXPECT_EQ(assessment.value().confidence, 0u);
  CO_EXPECT(assessment.value().has_blocker(blockers::kAbsenceOfEvidence));
  CO_EXPECT(!assessment.value().congestion_asserted);
  CO_EXPECT(assessment.value().citations.empty());
}

CO_TEST(assess, classification_is_deterministic) {
  const auto generation = cotest::test_generation();
  cotest::Source a = cotest::make_source("collector-a", generation);
  cotest::Source b = cotest::make_source("collector-b", generation);
  const EvidenceSubject link = cotest::link_subject();
  std::vector<EvidenceRecord> records;
  records.push_back(a.ratio(EvidenceKind::kLinkUtilization, link, 0.91, cotest::at(0)));
  records.push_back(b.ratio(EvidenceKind::kLinkUtilization, link, 0.905, cotest::at(0)));
  records.push_back(a.ratio(EvidenceKind::kDropCount, link, 0.002, cotest::at(0)));
  records.push_back(b.ratio(EvidenceKind::kDropCount, link, 0.0021, cotest::at(0)));
  records.push_back(a.record(EvidenceKind::kOfferedDemand, link, 91000000000.0,
                             ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                             cotest::at(0)));
  records.push_back(b.record(EvidenceKind::kCapacityAdvertisement, link, 100000000000.0,
                             ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                             cotest::at(0)));

  const auto first = classify_subject(make_request(link, 1, generation),
                                      cotest::pointers_to(records), ClassificationPolicy{});
  const auto second = classify_subject(make_request(link, 1, generation),
                                       cotest::pointers_to(records), ClassificationPolicy{});
  CO_EXPECT_OK(first);
  CO_EXPECT_OK(second);
  CO_EXPECT_EQ(first.value().verdict, second.value().verdict);
  CO_EXPECT_EQ(first.value().confidence, second.value().confidence);
  CO_EXPECT_EQ(first.value().features.feature_digest, second.value().features.feature_digest);
  CO_EXPECT_EQ(first.value().mechanisms, second.value().mechanisms);
  CO_EXPECT_EQ(first.value().severity, second.value().severity);
  CO_EXPECT_EQ(first.value().citations, second.value().citations);
  CO_EXPECT_EQ(first.value().explanation.steps.size(), second.value().explanation.steps.size());
  CO_EXPECT_EQ(first.value().explanation.render(), second.value().explanation.render());

  // A policy change changes the digest but not the identity of the input.
  ClassificationPolicy changed;
  changed.version = "co-policy-2";
  const auto other_policy = classify_subject(make_request(link, 1, generation),
                                             cotest::pointers_to(records), changed);
  CO_EXPECT_OK(other_policy);
  CO_EXPECT_NE(other_policy.value().explanation.policy_digest,
               first.value().explanation.policy_digest);
}

CO_TEST(assess, citations_are_sorted_deduplicated_and_bounded) {
  const auto generation = cotest::test_generation();
  const EvidenceSubject link = cotest::link_subject();
  std::vector<cotest::Source> sources;
  std::vector<EvidenceRecord> records;
  for (int i = 0; i < 6; ++i) {
    cotest::Source source = cotest::make_source("collector-" + std::to_string(i), generation);
    records.push_back(source.ratio(EvidenceKind::kDropCount, link, 0.01, cotest::at(0)));
    records.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.99, cotest::at(0)));
  }
  ClassificationRequest request = make_request(link, 1, generation);
  request.max_citations = 4;
  auto assessment =
      classify_subject(request, cotest::pointers_to(records), ClassificationPolicy{});
  CO_EXPECT_OK(assessment);
  CO_EXPECT(assessment.value().citations.size() <= 4);
  CO_EXPECT(std::is_sorted(assessment.value().citations.begin(), assessment.value().citations.end()));
  CO_EXPECT_EQ(std::adjacent_find(assessment.value().citations.begin(),
                                  assessment.value().citations.end()),
               assessment.value().citations.end());
}

CO_TEST(assess, severity_scales_with_impairment_rate) {
  const auto generation = cotest::test_generation();
  const EvidenceSubject link = cotest::link_subject();

  ClassificationPolicy policy;
  cotest::Source low = cotest::make_source("collector-low", generation);
  std::vector<EvidenceRecord> low_records;
  low_records.push_back(low.ratio(EvidenceKind::kLinkUtilization, link, 0.95, cotest::at(0)));
  low_records.push_back(low.ratio(EvidenceKind::kDropCount, link, 0.000002, cotest::at(0)));
  auto low_assessment = classify_subject(make_request(link, 1, generation),
                                         cotest::pointers_to(low_records), policy);
  CO_EXPECT_OK(low_assessment);
  CO_EXPECT_EQ(low_assessment.value().severity, Severity::kModerate);

  cotest::Source critical = cotest::make_source("collector-critical", generation);
  std::vector<EvidenceRecord> critical_records;
  critical_records.push_back(
      critical.ratio(EvidenceKind::kLinkUtilization, link, 0.99, cotest::at(0)));
  critical_records.push_back(critical.ratio(EvidenceKind::kDropCount, link, 0.5, cotest::at(0)));
  critical_records.push_back(critical.ratio(EvidenceKind::kMarkCount, link, 0.4, cotest::at(0)));
  auto critical_assessment = classify_subject(make_request(link, 1, generation),
                                              cotest::pointers_to(critical_records), policy);
  CO_EXPECT_OK(critical_assessment);
  CO_EXPECT_EQ(critical_assessment.value().severity, Severity::kCritical);
  CO_EXPECT(critical_assessment.value().confidence >
            low_assessment.value().confidence - 1u);
}

CO_TEST(assess, feature_extraction_is_exported_for_inspection) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();
  std::vector<EvidenceRecord> records;
  records.push_back(source.ratio(EvidenceKind::kLinkUtilization, link, 0.86, cotest::at(0)));
  records.push_back(source.ratio(EvidenceKind::kQueueOccupancy, link, 0.75, cotest::at(0)));
  records.push_back(source.ratio(EvidenceKind::kQueueOccupancy, link, 0.90, cotest::at(0)));

  const FeatureSnapshot features = extract_features(make_request(link, 1, generation),
                                                    cotest::pointers_to(records),
                                                    ClassificationPolicy{});
  CO_EXPECT(features.utilization_present);
  CO_EXPECT_EQ(features.utilization, 0.86);
  CO_EXPECT(features.pressure_present);
  CO_EXPECT(features.pressure_ratio_available);
  CO_EXPECT_EQ(features.pressure_ratio, 0.90);
  CO_EXPECT(features.pressure_exceeds_ratio);
  CO_EXPECT_EQ(features.records_fresh, static_cast<std::size_t>(3));
  CO_EXPECT_NE(features.feature_digest, static_cast<std::uint64_t>(0));
}
