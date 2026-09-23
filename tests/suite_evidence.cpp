// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "test_framework.hpp"
#include "test_support.hpp"

#include <limits>
#include <set>

using namespace congestion;

CO_TEST(evidence, kinds_map_to_argumentative_roles) {
  CO_EXPECT_EQ(role_of(EvidenceKind::kLinkUtilization), EvidenceRole::kUtilization);
  CO_EXPECT_EQ(role_of(EvidenceKind::kPortUtilization), EvidenceRole::kUtilization);
  CO_EXPECT_EQ(role_of(EvidenceKind::kQueueOccupancy), EvidenceRole::kPressure);
  CO_EXPECT_EQ(role_of(EvidenceKind::kBufferOccupancy), EvidenceRole::kPressure);
  CO_EXPECT_EQ(role_of(EvidenceKind::kDropCount), EvidenceRole::kImpairment);
  CO_EXPECT_EQ(role_of(EvidenceKind::kMarkCount), EvidenceRole::kImpairment);
  CO_EXPECT_EQ(role_of(EvidenceKind::kPauseCount), EvidenceRole::kImpairment);
  CO_EXPECT_EQ(role_of(EvidenceKind::kLatencySample), EvidenceRole::kImpairment);
  CO_EXPECT_EQ(role_of(EvidenceKind::kOfferedDemand), EvidenceRole::kDemand);
  CO_EXPECT_EQ(role_of(EvidenceKind::kAchievedThroughput), EvidenceRole::kDemand);
  CO_EXPECT_EQ(role_of(EvidenceKind::kActiveFlowCount), EvidenceRole::kContention);
  CO_EXPECT_EQ(role_of(EvidenceKind::kCapacityAdvertisement), EvidenceRole::kCapacity);
  CO_EXPECT_EQ(role_of(EvidenceKind::kSourceHeartbeat), EvidenceRole::kLiveness);
  CO_EXPECT_EQ(role_of(EvidenceKind::kUnknown), EvidenceRole::kUnknown);
  CO_EXPECT(is_impairment_role(EvidenceKind::kDropCount));
  CO_EXPECT(!is_impairment_role(EvidenceKind::kLinkUtilization));

  // Every kind must resolve to exactly one role and round-trip through its name.
  std::set<std::string> names;
  for (std::uint16_t raw = 0; raw < kEvidenceKindCount; ++raw) {
    const auto kind = static_cast<EvidenceKind>(raw);
    const std::string name(to_string(kind));
    CO_EXPECT(names.insert(name).second);
    auto parsed = evidence_kind_from_string(name);
    CO_EXPECT(parsed.ok());
    if (parsed.ok()) {
      CO_EXPECT_EQ(parsed.value(), kind);
    }
  }
}

CO_TEST(evidence, string_conversions_are_total_and_strict) {
  CO_EXPECT_EQ(std::string(to_string(ObservationUnit::kRatio)), std::string("ratio"));
  CO_EXPECT_EQ(std::string(to_string(ValueSemantics::kCumulativeCounter)),
               std::string("cumulative_counter"));
  CO_EXPECT_EQ(std::string(to_string(Support::kUnsupported)), std::string("unsupported"));
  CO_EXPECT_EQ(std::string(to_string(Completeness::kIncomplete)), std::string("incomplete"));
  CO_EXPECT_EQ(std::string(to_string(Freshness::kExpired)), std::string("expired"));
  CO_EXPECT_EQ(std::string(to_string(Agreement::kMinority)), std::string("minority"));
  CO_EXPECT(observation_unit_from_string("").ok() == false);
  CO_EXPECT(!observation_unit_from_string("").ok());
  CO_EXPECT(!value_semantics_from_string("nonsense").ok());
  CO_EXPECT(!support_from_string("maybe").ok());
  CO_EXPECT(!completeness_from_string("probably").ok());
}

CO_TEST(evidence, identity_is_deterministic_and_content_addressed) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceRecord first = source.ratio(EvidenceKind::kLinkUtilization,
                                            cotest::link_subject(), 0.5, cotest::at(0));
  const EvidenceId id = compute_evidence_id(first);
  CO_EXPECT_EQ(id, compute_evidence_id(first));

  // Identity is the address of the observation (source, fence, kind, subject, time, unit and
  // semantics), not the measured value: a corrected value for the same address keeps its id, and
  // the fence guarantees at most one accepted value per address.
  EvidenceRecord changed = first;
  changed.value.scalar = 0.6;
  CO_EXPECT_EQ(compute_evidence_id(changed), id);

  changed = first;
  changed.fence.sequence = Sequence(99);
  CO_EXPECT_NE(compute_evidence_id(changed), id);

  changed = first;
  changed.kind = EvidenceKind::kQueueOccupancy;
  CO_EXPECT_NE(compute_evidence_id(changed), id);

  changed = first;
  changed.subject = cotest::queue_subject();
  CO_EXPECT_NE(compute_evidence_id(changed), id);

  changed = first;
  changed.observed_at = cotest::at(1);
  CO_EXPECT_NE(compute_evidence_id(changed), id);

  // A copy with a different insertion index or note keeps the same identity: identity is about
  // the observation, not about bookkeeping.
  changed = first;
  changed.insertion_index = 42;
  changed.note = "annotated";
  CO_EXPECT_EQ(compute_evidence_id(changed), id);
}

CO_TEST(evidence, validation_rejects_incoherent_records) {
  const Limits limits = cotest::test_limits();
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  EvidenceRecord record = source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.5,
                                       cotest::at(0));
  CO_EXPECT(validate_evidence(record, limits).ok());

  EvidenceRecord broken = record;
  broken.kind = EvidenceKind::kUnknown;
  CO_EXPECT_STATUS_ERR(validate_evidence(broken, limits), ErrorCode::kInvalidArgument);

  broken = record;
  broken.subject = EvidenceSubject{};
  CO_EXPECT_STATUS_ERR(validate_evidence(broken, limits), ErrorCode::kInvalidArgument);

  broken = record;
  broken.provenance.source = SourceId{};
  CO_EXPECT_STATUS_ERR(validate_evidence(broken, limits), ErrorCode::kInvalidArgument);

  broken = record;
  broken.fence.source = SourceId::unchecked("someone-else");
  CO_EXPECT_STATUS_ERR(validate_evidence(broken, limits), ErrorCode::kPreconditionFailed);

  broken = record;
  broken.value.unit = ObservationUnit::kUnknown;
  CO_EXPECT_STATUS_ERR(validate_evidence(broken, limits), ErrorCode::kInvalidArgument);

  broken = record;
  broken.value.scalar = std::numeric_limits<double>::quiet_NaN();
  CO_EXPECT_STATUS_ERR(validate_evidence(broken, limits), ErrorCode::kInvalidArgument);

  broken = record;
  broken.validity = Duration::from_seconds(-1);
  CO_EXPECT_STATUS_ERR(validate_evidence(broken, limits), ErrorCode::kInvalidArgument);

  broken = record;
  broken.metadata.emplace_back(std::string(limits.max_metadata_key_bytes + 1, 'k'), "v");
  CO_EXPECT_STATUS_ERR(validate_evidence(broken, limits), ErrorCode::kLimitExceeded);

  broken = record;
  broken.note = std::string(limits.max_note_bytes + 1, 'n');
  CO_EXPECT_STATUS_ERR(validate_evidence(broken, limits), ErrorCode::kLimitExceeded);

  // Unsupported evidence is allowed to omit a unit: it asserts that the value cannot be observed.
  broken = record;
  broken.support = Support::kUnsupported;
  broken.value.unit = ObservationUnit::kUnknown;
  CO_EXPECT(validate_evidence(broken, limits).ok());
}

CO_TEST(evidence, freshness_distinguishes_every_state) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  // The record declares a long validity so that the policy windows alone decide the state.
  const EvidenceRecord record = source.record(
      EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.5, ObservationUnit::kRatio,
      ValueSemantics::kGauge, cotest::at(0), Duration::from_minutes(60));
  const FreshnessPolicy policy;

  CO_EXPECT_EQ(assess_freshness(record, cotest::at(1), policy).freshness, Freshness::kFresh);
  CO_EXPECT_EQ(assess_freshness(record, cotest::at(5), policy).freshness, Freshness::kAging);
  CO_EXPECT_EQ(assess_freshness(record, cotest::at(30), policy).freshness, Freshness::kStale);
  CO_EXPECT_EQ(assess_freshness(record, cotest::at(600), policy).freshness, Freshness::kExpired);

  // A record whose own validity is shorter expires earlier: the tighter bound wins.
  const EvidenceRecord short_lived = source.record(
      EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.5, ObservationUnit::kRatio,
      ValueSemantics::kGauge, cotest::at(0), Duration::from_seconds(5));
  CO_EXPECT_EQ(assess_freshness(short_lived, cotest::at(30), policy).freshness,
               Freshness::kExpired);

  EvidenceRecord no_clock = record;
  no_clock.clock = ClockDomain::kUnknown;
  const auto unknown = assess_freshness(no_clock, cotest::at(1), policy);
  CO_EXPECT_EQ(unknown.freshness, Freshness::kUnknown);
  CO_EXPECT_EQ(unknown.reason_code, std::string("clock_domain_unknown"));

  EvidenceRecord no_timestamps = record;
  no_timestamps.observed_at = Timestamp{};
  CO_EXPECT_EQ(assess_freshness(no_timestamps, cotest::at(1), policy).freshness, Freshness::kUnknown);

  EvidenceRecord future = record;
  future.observed_at = cotest::at(100);
  future.received_at = cotest::at(100);
  CO_EXPECT_EQ(assess_freshness(future, cotest::at(1), policy).freshness, Freshness::kUnknown);

  EvidenceRecord recovered = record;
  recovered.recovered_from_snapshot = true;
  const auto recovered_result = assess_freshness(recovered, cotest::at(0), policy);
  CO_EXPECT_EQ(recovered_result.freshness, Freshness::kUnknown);
  CO_EXPECT_EQ(recovered_result.reason_code, std::string("persisted_dynamic_evidence_not_live"));

  EvidenceRecord retired = record;
  retired.retired = true;
  CO_EXPECT_EQ(assess_freshness(retired, cotest::at(0), policy).freshness, Freshness::kUnknown);

  CO_EXPECT(usable_as_deciding_evidence(Freshness::kFresh));
  CO_EXPECT(!usable_as_deciding_evidence(Freshness::kAging));
  CO_EXPECT(usable_as_corroboration(Freshness::kAging));
  CO_EXPECT(!usable_as_corroboration(Freshness::kStale));
  CO_EXPECT(!usable_as_corroboration(Freshness::kUnknown));
}

CO_TEST(evidence, agreement_keeps_minorities_visible) {
  const auto generation = cotest::test_generation();
  std::vector<EvidenceRecord> records;
  cotest::Source a = cotest::make_source("source-a", generation);
  cotest::Source b = cotest::make_source("source-b", generation);
  cotest::Source c = cotest::make_source("source-c", generation);
  records.push_back(a.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.90, cotest::at(0)));
  records.push_back(b.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.91, cotest::at(0)));
  records.push_back(c.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.20, cotest::at(0)));
  auto pointers = cotest::pointers_to(records);

  // Two of three sources agree: a dissenting minority is reported, never hidden.
  const AgreementAssessment minority = assess_agreement(pointers, 0.10, 64);
  CO_EXPECT_EQ(minority.agreement, Agreement::kMinority);
  CO_EXPECT_EQ(minority.distinct_sources, static_cast<std::size_t>(3));
  CO_EXPECT_EQ(minority.citations.size(), static_cast<std::size_t>(3));

  std::vector<EvidenceRecord> split;
  cotest::Source d = cotest::make_source("source-d", generation);
  split.push_back(a.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.90, cotest::at(0)));
  split.push_back(c.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.20, cotest::at(0)));
  const AgreementAssessment conflicting = assess_agreement(cotest::pointers_to(split), 0.10, 64);
  CO_EXPECT_EQ(conflicting.agreement, Agreement::kConflicting);

  std::vector<EvidenceRecord> single;
  single.push_back(a.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.90, cotest::at(0)));
  CO_EXPECT_EQ(assess_agreement(cotest::pointers_to(single), 0.10, 64).agreement,
               Agreement::kSingleSource);

  std::vector<EvidenceRecord> unanimous;
  unanimous.push_back(a.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.90, cotest::at(0)));
  unanimous.push_back(b.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.91, cotest::at(0)));
  CO_EXPECT_EQ(assess_agreement(cotest::pointers_to(unanimous), 0.10, 64).agreement,
               Agreement::kUnanimous);

  std::vector<EvidenceRecord> mixed_units;
  mixed_units.push_back(a.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.90, cotest::at(0)));
  mixed_units.push_back(
      b.record(EvidenceKind::kLinkUtilization, cotest::link_subject(), 1000.0,
               ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge, cotest::at(0)));
  CO_EXPECT_EQ(assess_agreement(cotest::pointers_to(mixed_units), 0.10, 64).agreement,
               Agreement::kIncomparable);

  std::vector<EvidenceRecord> counters;
  counters.push_back(a.counter(EvidenceKind::kDropCount, cotest::link_subject(), 100.0,
                               ValueSemantics::kCumulativeCounter, cotest::at(0)));
  counters.push_back(b.counter(EvidenceKind::kDropCount, cotest::link_subject(), 200.0,
                               ValueSemantics::kCumulativeCounter, cotest::at(0)));
  CO_EXPECT_EQ(assess_agreement(cotest::pointers_to(counters), 0.10, 64).agreement,
               Agreement::kIncomparable);

  CO_EXPECT_EQ(assess_agreement({}, 0.10, 64).agreement, Agreement::kUnknown);
}

CO_TEST(evidence, store_fences_replays_and_tracks_liveness) {
  const Limits limits = cotest::test_limits();
  EvidenceStore store(limits);
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  IngestPolicy policy;

  EvidenceRecord first = source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.5,
                                      cotest::at(0));
  auto outcome = store.ingest(first, cotest::at(0), policy);
  CO_EXPECT_OK(outcome);
  CO_EXPECT(outcome.value().stored);
  CO_EXPECT_EQ(outcome.value().fence, FenceDecision::kAcceptedFirstObservation);
  CO_EXPECT(store.source_live(SourceId::unchecked("collector-1")));

  // A replay of the same sequence number is fenced and reported, not stored.
  auto replayed = store.ingest(first, cotest::at(0), policy);
  CO_EXPECT_OK(replayed);
  CO_EXPECT(!replayed.value().stored);
  CO_EXPECT_EQ(replayed.value().fence, FenceDecision::kRejectedReplayedSequence);
  CO_EXPECT_EQ(store.size(), static_cast<std::size_t>(1));

  EvidenceRecord second = source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.6,
                                       cotest::at(1));
  CO_EXPECT(store.ingest(second, cotest::at(1), policy).value().stored);

  // A stale generation is refused and does not disturb what is retained.
  cotest::Source stale = cotest::make_source("collector-1", cotest::test_generation(0, 0, 0));
  stale.incarnation = source.incarnation;
  stale.boot = source.boot;
  stale.sequence = source.sequence;
  EvidenceRecord stale_record = stale.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(),
                                            0.99, cotest::at(2));
  auto stale_outcome = store.ingest(stale_record, cotest::at(2), policy);
  CO_EXPECT_OK(stale_outcome);
  CO_EXPECT(!stale_outcome.value().stored);
  CO_EXPECT_EQ(store.size(), static_cast<std::size_t>(2));

  const StoreStats stats = store.stats();
  CO_EXPECT_EQ(stats.accepted, static_cast<std::uint64_t>(2));
  CO_EXPECT_EQ(stats.rejected_fence, static_cast<std::uint64_t>(2));
  CO_EXPECT_EQ(stats.live_sources, static_cast<std::size_t>(1));

  store.reset_liveness();
  CO_EXPECT(!store.source_live(SourceId::unchecked("collector-1")));
  CO_EXPECT_EQ(store.stats().live_sources, static_cast<std::size_t>(0));
  CO_EXPECT_EQ(store.size(), static_cast<std::size_t>(2));
}

CO_TEST(evidence, store_rejects_unknown_subjects_and_low_authority) {
  const Limits limits = cotest::test_limits();
  EvidenceStore store(limits);
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);

  IngestPolicy policy;
  policy.require_known_subject = true;
  EvidenceRecord unknown = source.ratio(EvidenceKind::kLinkUtilization,
                                        cotest::link_subject("missing-link"), 0.5, cotest::at(0));
  CO_EXPECT_ERR(store.ingest(unknown, cotest::at(0), policy, &topology), ErrorCode::kNotFound);

  IngestPolicy strict = policy;
  strict.minimum_authority = AuthorityLevel::kMeasured;
  cotest::Source unverified = cotest::make_source("collector-2", generation);
  unverified.authority = AuthorityLevel::kUnverified;
  EvidenceRecord weak = unverified.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.5,
                                         cotest::at(0));
  auto rejected = store.ingest(weak, cotest::at(0), strict, &topology);
  CO_EXPECT_OK(rejected);
  CO_EXPECT(!rejected.value().stored);
  CO_EXPECT_EQ(rejected.value().fence, FenceDecision::kRejectedAuthorityTooLow);

  IngestPolicy incarnation_required;
  EvidenceRecord no_incarnation = source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(),
                                               0.5, cotest::at(0));
  no_incarnation.fence.incarnation = Incarnation(0);
  no_incarnation.id = compute_evidence_id(no_incarnation);
  CO_EXPECT_ERR(store.ingest(no_incarnation, cotest::at(0), incarnation_required, &topology),
                ErrorCode::kInvalidArgument);
}

CO_TEST(evidence, store_retires_previous_incarnation_on_reboot) {
  const Limits limits = cotest::test_limits();
  EvidenceStore store(limits);
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  IngestPolicy policy;

  EvidenceRecord before = source.ratio(EvidenceKind::kQueueOccupancy, cotest::queue_subject(), 0.9,
                                       cotest::at(0));
  CO_EXPECT(store.ingest(before, cotest::at(0), policy).value().stored);

  source.boot = BootId(2);
  source.incarnation = Incarnation(1);
  EvidenceRecord after = source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.1,
                                      cotest::at(1));
  auto outcome = store.ingest(after, cotest::at(1), policy);
  CO_EXPECT_OK(outcome);
  CO_EXPECT(outcome.value().stored);
  CO_EXPECT(outcome.value().liveness_reset);

  EvidenceQuery query;
  query.subject = nullptr;
  query.window_start = cotest::at(-10);
  query.window_end = cotest::at(10);
  query.max_records = 64;
  const auto records = store.query(query);
  CO_EXPECT_EQ(records.size(), static_cast<std::size_t>(2));
  std::size_t retired = 0;
  for (const EvidenceRecord& record : records) {
    if (record.retired) {
      ++retired;
      CO_EXPECT_EQ(record.kind, EvidenceKind::kQueueOccupancy);
      CO_EXPECT_EQ(assess_freshness(record, cotest::at(1), FreshnessPolicy{}).freshness,
                   Freshness::kUnknown);
    }
  }
  CO_EXPECT_EQ(retired, static_cast<std::size_t>(1));
}

CO_TEST(evidence, store_bounds_growth_by_eviction) {
  Limits limits = cotest::test_limits();
  limits.max_retained_evidence = 8;
  limits.max_evidence_per_subject = 3;
  limits.max_window_records = 8;
  limits.max_snapshot_evidence = 8;
  CO_EXPECT(limits.validate().ok());
  EvidenceStore store(limits);
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  IngestPolicy policy;

  for (int i = 0; i < 20; ++i) {
    EvidenceRecord record = source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(),
                                         0.1 * static_cast<double>(i), cotest::at(i));
    CO_EXPECT(store.ingest(record, cotest::at(i), policy).value().stored);
  }
  CO_EXPECT(store.size() <= limits.max_evidence_per_subject);
  CO_EXPECT(store.stats().evicted > 0);

  EvidenceQuery query;
  query.max_records = 64;
  const auto records = store.query(query);
  CO_EXPECT(records.size() <= limits.max_evidence_per_subject);
  // Retention keeps the newest observations: the oldest were evicted first.
  if (!records.empty()) {
    CO_EXPECT(records.back().observed_at >= records.front().observed_at);
  }
}

CO_TEST(evidence, store_sweep_is_bounded_and_cancellable) {
  Limits limits = cotest::test_limits();
  EvidenceStore store(limits);
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  IngestPolicy policy;
  for (int i = 0; i < 10; ++i) {
    EvidenceRecord record = source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.5,
                                         cotest::at(i));
    CO_EXPECT(store.ingest(record, cotest::at(i), policy).value().stored);
  }
  CancellationSource cancellation;
  auto swept = store.sweep(cotest::at(5), cotest::at(10), 3, cancellation.token());
  CO_EXPECT_OK(swept);
  CO_EXPECT_EQ(swept.value(), static_cast<std::size_t>(3));
  CO_EXPECT_EQ(store.size(), static_cast<std::size_t>(7));

  swept = store.sweep(cotest::at(100), cotest::at(200), 100, cancellation.token());
  CO_EXPECT_OK(swept);
  CO_EXPECT_EQ(swept.value(), static_cast<std::size_t>(7));
  CO_EXPECT_EQ(store.size(), static_cast<std::size_t>(0));

  CancellationSource cancelled;
  cancelled.cancel("test");
  CO_EXPECT_ERR(store.sweep(cotest::at(0), cotest::at(1), 10, cancelled.token()),
                ErrorCode::kCancelled);
}

CO_TEST(evidence, store_query_filters_and_orders_deterministically) {
  EvidenceStore store(cotest::test_limits());
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  IngestPolicy policy;
  for (int i = 0; i < 5; ++i) {
    EvidenceRecord record = source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.5,
                                         cotest::at(i));
    CO_EXPECT(store.ingest(record, cotest::at(i), policy).value().stored);
  }
  EvidenceRecord queue_record = source.ratio(EvidenceKind::kQueueOccupancy, cotest::queue_subject(),
                                             0.5, cotest::at(9));
  CO_EXPECT(store.ingest(queue_record, cotest::at(9), policy).value().stored);

  EvidenceQuery query;
  const EvidenceSubject subject = cotest::link_subject();
  query.subject = &subject;
  query.window_start = cotest::at(0);
  query.window_end = cotest::at(10);
  query.max_records = 64;
  const auto link_records = store.query(query);
  CO_EXPECT_EQ(link_records.size(), static_cast<std::size_t>(5));
  for (const EvidenceRecord& record : link_records) {
    CO_EXPECT_EQ(record.subject, subject);
  }
  for (std::size_t i = 1; i < link_records.size(); ++i) {
    CO_EXPECT(link_records[i - 1].observed_at <= link_records[i].observed_at);
  }

  query.newest_first = true;
  const auto newest_first = store.query(query);
  CO_EXPECT_EQ(newest_first.size(), link_records.size());
  if (!newest_first.empty()) {
    CO_EXPECT(newest_first.front().observed_at >= newest_first.back().observed_at);
  }

  const EvidenceKind kind = EvidenceKind::kQueueOccupancy;
  EvidenceQuery by_kind;
  by_kind.kind = &kind;
  by_kind.max_records = 64;
  CO_EXPECT_EQ(store.query(by_kind).size(), static_cast<std::size_t>(1));

  EvidenceQuery limited;
  limited.subject = &subject;
  limited.max_records = 2;
  CO_EXPECT_EQ(store.query(limited).size(), static_cast<std::size_t>(2));
}

CO_TEST(evidence, recovered_records_are_never_live) {
  EvidenceStore store(cotest::test_limits());
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  EvidenceRecord record = source.ratio(EvidenceKind::kDropCount, cotest::link_subject(), 0.5,
                                       cotest::at(0));
  auto outcome = store.ingest_recovered(record);
  CO_EXPECT_OK(outcome);
  CO_EXPECT(outcome.value().stored);
  CO_EXPECT_EQ(outcome.value().freshness, Freshness::kUnknown);
  CO_EXPECT(!store.source_live(SourceId::unchecked("collector-1")));
  CO_EXPECT_EQ(store.stats().live_sources, static_cast<std::size_t>(0));

  EvidenceQuery query;
  query.max_records = 8;
  const auto records = store.query(query);
  CO_EXPECT_EQ(records.size(), static_cast<std::size_t>(1));
  if (!records.empty()) {
    CO_EXPECT(records.front().recovered_from_snapshot);
  }

  // A live observation from the same source is accepted afterwards: recovery does not fence the
  // source out, it simply does not make it live.
  IngestPolicy policy;
  EvidenceRecord live = source.ratio(EvidenceKind::kDropCount, cotest::link_subject(), 0.5,
                                     cotest::at(1));
  auto live_outcome = store.ingest(live, cotest::at(1), policy);
  CO_EXPECT_OK(live_outcome);
  CO_EXPECT(live_outcome.value().stored);
  CO_EXPECT(store.source_live(SourceId::unchecked("collector-1")));
}
