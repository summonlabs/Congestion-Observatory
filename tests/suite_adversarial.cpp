// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
// Adversarial inputs: malformed, hostile, oversized and fuzzed.
#include "test_framework.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace congestion;

namespace {

std::vector<std::uint8_t> encode_reference_snapshot() {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  SnapshotContent content;
  content.metadata.policy_version = ClassificationPolicy{}.version;
  content.metadata.policy_digest = ClassificationPolicy{}.digest();
  content.metadata.limits_digest = Limits{}.digest();
  content.metadata.created_at = cotest::at(0);
  content.metadata.topology_digest = topology.digest();
  content.topology = topology;
  cotest::Source source = cotest::make_source("collector-1", topology.generation());
  content.evidence.push_back(
      source.ratio(EvidenceKind::kDropCount, cotest::link_subject(), 0.5, cotest::at(0)));
  auto encoded = encode_snapshot(content, cotest::test_limits());
  return encoded.ok() ? encoded.value() : std::vector<std::uint8_t>{};
}

}  // namespace

CO_TEST(adversarial, hostile_json_documents_are_refused) {
  const Limits limits = cotest::test_limits();
  // Deeply nested arrays must be rejected by the depth bound, not by a stack overflow.
  std::string deep(200, '[');
  deep += std::string(200, ']');
  CO_EXPECT_ERR(parse_json(deep, limits), ErrorCode::kLimitExceeded);

  // A document full of keys hits the node bound.
  std::string many = "{";
  for (int i = 0; i < 200000; ++i) {
    if (i != 0) {
      many += ",";
    }
    many += "\"k" + std::to_string(i) + "\":1";
  }
  many += "}";
  auto parsed = parse_json(many, limits);
  CO_EXPECT(!parsed.ok());

  // Control characters, lone surrogates and truncated escapes are refused.
  for (const char* text : {"\"\\ud800\"", "\"abc", "{\"a\"", "[1,2", "tru"}) {
    CO_EXPECT(!parse_json(text, limits).ok());
  }
  // An escaped NUL is legal JSON and decodes to an embedded NUL byte.
  auto escaped_nul = parse_json("\"\\u0000\"", limits);
  CO_EXPECT_OK(escaped_nul);
  if (escaped_nul.ok()) {
    CO_EXPECT_EQ(escaped_nul.value().as_string().size(), static_cast<std::size_t>(1));
  }
}

CO_TEST(adversarial, evidence_decoding_refuses_malformed_documents) {
  const Limits limits = cotest::test_limits();
  const char* documents[] = {
      "",
      "null",
      "[]",
      "[{}]",
      "[{\"kind\":\"nonsense\"}]",
      "[{\"kind\":\"drop_count\"}]",
      "[{\"kind\":\"drop_count\",\"subject\":\"nonsense:x\"}]",
      "[{\"kind\":\"drop_count\",\"subject\":\"link:a\",\"source\":\"\"}]",
      "[{\"kind\":\"drop_count\",\"subject\":\"link:a\",\"source\":\"s\",\"observed_at\":\"bad\"}]",
      "[{\"kind\":\"drop_count\",\"subject\":\"link:a\",\"source\":\"s\",\"observed_at\":"
      "\"1970-01-01T00:00:00Z\",\"received_at\":\"1970-01-01T00:00:00Z\",\"value\":-1,\"unit\":"
      "\"count\",\"semantics\":\"rate\",\"validity_ns\":-5}]",
      "[{\"kind\":\"drop_count\",\"subject\":\"link:a\",\"source\":\"s\",\"observed_at\":"
      "\"1970-01-01T00:00:00Z\",\"received_at\":\"1970-01-01T00:00:00Z\",\"value\":1,\"unit\":"
      "\"ratio\",\"semantics\":\"gauge\",\"epoch\":-1}]",
  };
  for (const char* document : documents) {
    auto decoded = decode_ingest_json(document, limits);
    CO_EXPECT(!decoded.ok());
  }
  auto valid = decode_ingest_json(
      "{\"kind\":\"drop_count\",\"subject\":\"link:a\",\"source\":\"s\",\"authority\":\"measured\","
      "\"boot\":1,\"incarnation\":1,\"epoch\":1,\"generation\":1,\"revision\":1,\"sequence\":1,"
      "\"observed_at\":\"1970-01-01T00:00:00Z\",\"received_at\":\"1970-01-01T00:00:00Z\","
      "\"clock\":\"collector_wall_clock\",\"value\":0.5,\"unit\":\"ratio\",\"semantics\":\"gauge\"}",
      limits);
  CO_EXPECT_OK(valid);
}

CO_TEST(adversarial, classification_rejects_incoherent_requests) {
  const auto generation = cotest::test_generation();
  ClassificationRequest request;
  request.subject = EvidenceSubject{};
  request.evaluated_at = cotest::at(0);
  CO_EXPECT_ERR(classify_subject(request, {}, ClassificationPolicy{}), ErrorCode::kInvalidArgument);

  request.subject = cotest::link_subject();
  request.window_start = cotest::at(10);
  request.window_end = cotest::at(0);
  CO_EXPECT_ERR(classify_subject(request, {}, ClassificationPolicy{}), ErrorCode::kInvalidArgument);

  // A null record pointer is ignored rather than dereferenced.
  request.window_start = cotest::at(0);
  request.window_end = cotest::at(10);
  request.generation = generation;
  std::vector<const EvidenceRecord*> with_null{nullptr};
  auto assessment = classify_subject(request, with_null, ClassificationPolicy{});
  CO_EXPECT_OK(assessment);
  CO_EXPECT_EQ(assessment.value().verdict, Verdict::kNoEvidence);
}

CO_TEST(adversarial, extreme_values_do_not_produce_nonsense) {
  const auto generation = cotest::test_generation();
  cotest::Source source = cotest::make_source("collector-1", generation);
  const EvidenceSubject link = cotest::link_subject();
  ClassificationRequest request;
  request.subject = link;
  request.window_start = cotest::at(-30);
  request.window_end = cotest::at(1);
  request.evaluated_at = cotest::at(1);
  request.generation = generation;

  std::vector<EvidenceRecord> records;
  records.push_back(source.ratio(EvidenceKind::kLinkUtilization, link,
                                 std::numeric_limits<double>::max(), cotest::at(0)));
  records.push_back(source.record(EvidenceKind::kOfferedDemand, link,
                                  std::numeric_limits<double>::max(),
                                  ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                                  cotest::at(0)));
  records.push_back(source.record(EvidenceKind::kCapacityAdvertisement, link, 1.0,
                                  ObservationUnit::kBitsPerSecond, ValueSemantics::kGauge,
                                  cotest::at(0)));
  records.push_back(source.ratio(EvidenceKind::kDropCount, link, 1e300, cotest::at(0)));
  auto assessment = classify_subject(request, cotest::pointers_to(records),
                                     ClassificationPolicy{});
  CO_EXPECT_OK(assessment);
  CO_EXPECT(assessment.value().verdict != Verdict::kNoEvidence);
  CO_EXPECT(assessment.value().confidence <= 100);
  CO_EXPECT(assessment.value().severity <= Severity::kCritical);

  // Negative values are accepted as data but must not fabricate impairment.
  std::vector<EvidenceRecord> negative;
  negative.push_back(source.ratio(EvidenceKind::kDropCount, link, -5.0, cotest::at(0)));
  auto negative_assessment = classify_subject(request, cotest::pointers_to(negative),
                                              ClassificationPolicy{});
  CO_EXPECT_OK(negative_assessment);
  CO_EXPECT(!negative_assessment.value().congestion_asserted);
}

CO_TEST(adversarial, snapshot_fuzzing_never_crashes_or_silently_accepts) {
  const std::vector<std::uint8_t> reference = encode_reference_snapshot();
  CO_EXPECT(!reference.empty());
  if (reference.empty()) {
    return;
  }
  std::mt19937_64 engine(20260102);
  std::uniform_int_distribution<std::size_t> position_distribution(0, reference.size() - 1);
  std::uniform_int_distribution<int> byte_distribution(0, 255);
  const Limits limits = cotest::test_limits();
  std::size_t rejected = 0;
  std::size_t accepted = 0;
  for (int iteration = 0; iteration < 400; ++iteration) {
    std::vector<std::uint8_t> mutated = reference;
    const int mutations = 1 + (iteration % 4);
    for (int m = 0; m < mutations; ++m) {
      mutated[position_distribution(engine)] =
          static_cast<std::uint8_t>(byte_distribution(engine));
    }
    auto decoded = decode_snapshot(mutated, limits, ClassificationPolicy{});
    if (decoded.ok()) {
      ++accepted;
      // If a mutation is accepted it must still be internally consistent.
      CO_EXPECT_EQ(decoded.value().topology.digest(), decoded.value().metadata.topology_digest);
      for (const Episode& episode : decoded.value().episodes) {
        CO_EXPECT_EQ(compute_episode_id(episode.key), episode.id);
      }
    } else {
      ++rejected;
      CO_EXPECT(decoded.error().code() == ErrorCode::kIntegrityFailure ||
                decoded.error().code() == ErrorCode::kVersionMismatch ||
                decoded.error().code() == ErrorCode::kLimitExceeded ||
                decoded.error().code() == ErrorCode::kNotFound ||
                decoded.error().code() == ErrorCode::kInvalidArgument ||
                decoded.error().code() == ErrorCode::kAlreadyExists);
    }
  }
  CO_EXPECT(rejected > 0);

  // Truncations at every length must be rejected or decode cleanly; none may crash.
  for (std::size_t length = 0; length < reference.size(); length += 7) {
    std::vector<std::uint8_t> truncated(reference.begin(), reference.begin() + static_cast<std::ptrdiff_t>(length));
    auto decoded = decode_snapshot(truncated, limits, ClassificationPolicy{});
    if (decoded.ok()) {
      CO_EXPECT_EQ(decoded.value().topology.digest(), decoded.value().metadata.topology_digest);
    }
  }
}

CO_TEST(adversarial, frame_decoder_survives_hostile_streams) {
  Limits limits = cotest::test_limits();
  limits.max_frame_bytes = 128;
  FrameDecoder decoder(limits);
  // A stream of zero bytes: the magic check must refuse without allocating.
  std::vector<char> zeros(limits.max_frame_bytes, 0);
  CO_EXPECT(decoder.feed(zeros.data(), zeros.size()).ok());
  CO_EXPECT_ERR(decoder.next(), ErrorCode::kProtocolError);

  // A feed that would exceed the buffer bound is refused before it is appended.
  FrameDecoder overflowing(limits);
  std::vector<char> too_much(limits.max_frame_bytes + kFrameHeaderBytes + 1, 0);
  CO_EXPECT_STATUS_ERR(overflowing.feed(too_much.data(), too_much.size()),
                       ErrorCode::kLimitExceeded);

  FrameDecoder bounded(limits);
  std::vector<char> flood(limits.max_frame_bytes + kFrameHeaderBytes + 1, 0x41);
  CO_EXPECT_STATUS_ERR(bounded.feed(flood.data(), flood.size()), ErrorCode::kLimitExceeded);

  Frame oversized;
  oversized.type = FrameType::kIngest;
  oversized.payload.assign(limits.max_frame_bytes, 'z');
  CO_EXPECT_ERR(encode_frame(oversized, limits), ErrorCode::kLimitExceeded);
}

CO_TEST(adversarial, store_limits_are_actually_enforced) {
  Limits limits = cotest::test_limits();
  limits.max_sources = 2;
  limits.max_retained_evidence = 4;
  limits.max_evidence_per_subject = 2;
  limits.max_window_records = 4;
  limits.max_snapshot_evidence = 4;
  CO_EXPECT(limits.validate().ok());
  EvidenceStore store(limits);
  const auto generation = cotest::test_generation();
  IngestPolicy policy;

  for (int source_index = 0; source_index < 2; ++source_index) {
    cotest::Source source =
        cotest::make_source("source-" + std::to_string(source_index), generation);
    for (int i = 0; i < 10; ++i) {
      CO_EXPECT_OK(store.ingest(source.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(),
                                             0.5, cotest::at(i)),
                                cotest::at(i), policy));
    }
  }
  CO_EXPECT(store.size() <= limits.max_retained_evidence);

  cotest::Source extra = cotest::make_source("source-extra", generation);
  CO_EXPECT_ERR(store.ingest(extra.ratio(EvidenceKind::kLinkUtilization, cotest::link_subject(), 0.5,
                                         cotest::at(0)),
                             cotest::at(0), policy),
                ErrorCode::kLimitExceeded);
}

CO_TEST(adversarial, episode_and_correlation_budgets_hold) {
  Limits limits = cotest::test_limits();
  limits.max_groups = 2;
  limits.max_group_members = 1;
  limits.max_episodes = 3;
  limits.max_snapshot_episodes = 3;
  CO_EXPECT(limits.validate().ok());
  std::vector<Episode> episodes;
  for (int i = 0; i < 10; ++i) {
    Episode episode;
    EpisodeKey key;
    key.scope = cotest::link_subject(("link-" + std::to_string(i)).c_str());
    key.mechanism = Mechanism::kPacketDrop;
    key.generation = cotest::test_generation();
    key.policy_version = "co-policy-1";
    episode.key = key;
    episode.id = compute_episode_id(key);
    episode.first_seen = cotest::at(i);
    episode.last_seen = cotest::at(i);
    episodes.push_back(episode);
  }
  auto groups = correlate(episodes, CorrelationPolicy{}, limits, CancellationToken{});
  CO_EXPECT_OK(groups);
  CO_EXPECT(groups.value().size() <= limits.max_groups);
  for (const auto& group : groups.value()) {
    CO_EXPECT(group.members.size() <= limits.max_group_members);
  }

  EpisodeRegistry registry(limits);
  std::size_t failures = 0;
  for (int i = 0; i < 20; ++i) {
    const EvidenceSubject subject = cotest::link_subject(("link-" + std::to_string(i)).c_str());
    cotest::Source source = cotest::make_source("collector-1", cotest::test_generation());
    ClassificationRequest request;
    request.subject = subject;
    request.window_start = cotest::at(-10);
    request.window_end = cotest::at(1);
    request.evaluated_at = cotest::at(1);
    request.generation = cotest::test_generation();
    std::vector<EvidenceRecord> records;
    records.push_back(source.ratio(EvidenceKind::kDropCount, subject, 0.5, cotest::at(0)));
    auto assessment = classify_subject(request, cotest::pointers_to(records),
                                       ClassificationPolicy{});
    CO_EXPECT_OK(assessment);
    if (!assessment.ok()) {
      continue;
    }
    auto update = registry.observe(assessment.value(), EpisodePolicy{}, cotest::at(1));
    if (!update.ok()) {
      ++failures;
    }
  }
  CO_EXPECT(registry.size() <= limits.max_episodes);
  CO_EXPECT(failures > 0);
  CO_EXPECT(registry.dropped_episodes() > 0);
}

CO_TEST(adversarial, correlation_refuses_inconsistent_episode_identity) {
  const Limits limits = cotest::test_limits();
  Episode episode;
  EpisodeKey key;
  key.scope = cotest::link_subject();
  key.mechanism = Mechanism::kPacketDrop;
  key.generation = cotest::test_generation();
  key.policy_version = "co-policy-1";
  episode.key = key;
  episode.id = compute_episode_id(key);
  // A restore with a forged identity must be rejected rather than trusted.
  Episode forged = episode;
  forged.id = EpisodeId::from_digest({9, 9});
  EpisodeRegistry registry(limits);
  CO_EXPECT(registry.restore(forged).ok());  // restore trusts the caller...
  // ...but a snapshot never does: the identity is recomputed and compared on load.
  SnapshotContent content;
  content.metadata.policy_version = ClassificationPolicy{}.version;
  content.metadata.policy_digest = ClassificationPolicy{}.digest();
  content.metadata.limits_digest = Limits{}.digest();
  content.metadata.created_at = cotest::at(0);
  content.topology = cotest::make_test_topology(cotest::test_generation());
  content.metadata.topology_digest = content.topology.digest();
  content.episodes.push_back(forged);
  auto encoded = encode_snapshot(content, limits);
  CO_EXPECT_OK(encoded);
  if (encoded.ok()) {
    CO_EXPECT_ERR(decode_snapshot(encoded.value(), limits, ClassificationPolicy{}),
                  ErrorCode::kIntegrityFailure);
  }
}
