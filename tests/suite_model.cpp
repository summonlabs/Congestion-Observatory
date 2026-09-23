// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace congestion;

CO_TEST(model, topology_builds_and_indexes) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  CO_EXPECT_EQ(topology.nodes().size(), static_cast<std::size_t>(4));
  CO_EXPECT_EQ(topology.ports().size(), static_cast<std::size_t>(6));
  CO_EXPECT_EQ(topology.links().size(), static_cast<std::size_t>(3));
  CO_EXPECT_EQ(topology.queues().size(), static_cast<std::size_t>(4));
  CO_EXPECT_EQ(topology.buffers().size(), static_cast<std::size_t>(2));
  CO_EXPECT_EQ(topology.paths().size(), static_cast<std::size_t>(1));
  CO_EXPECT_EQ(topology.flows().size(), static_cast<std::size_t>(1));

  CO_EXPECT(topology.link(LinkId::unchecked("leaf1-leaf2")) != nullptr);
  CO_EXPECT(topology.link(LinkId::unchecked("missing")) == nullptr);
  const Node* node = topology.node_of_port(PortId::unchecked("leaf1/2"));
  CO_EXPECT(node != nullptr);
  if (node != nullptr) {
    CO_EXPECT_EQ(node->id, NodeId::unchecked("leaf1"));
  }
  const Link* queue_link = topology.link_of_queue(QueueId::unchecked("leaf1-leaf2/q0"));
  CO_EXPECT(queue_link != nullptr);
  if (queue_link != nullptr) {
    CO_EXPECT_EQ(queue_link->id, LinkId::unchecked("leaf1-leaf2"));
  }

  const auto links = topology.links_of_node(NodeId::unchecked("leaf1"));
  CO_EXPECT_EQ(links.size(), static_cast<std::size_t>(2));
  if (links.size() == 2) {
    CO_EXPECT(links[0] < links[1]);
  }
  const auto paths = topology.paths_through_link(LinkId::unchecked("leaf1-leaf2"));
  CO_EXPECT_EQ(paths.size(), static_cast<std::size_t>(1));
  const auto flows = topology.flows_on_path(PathId::unchecked("host1-host2"));
  CO_EXPECT_EQ(flows.size(), static_cast<std::size_t>(1));
  const auto queues = topology.queues_of_link(LinkId::unchecked("leaf1-leaf2"));
  CO_EXPECT_EQ(queues.size(), static_cast<std::size_t>(2));
  const auto buffers = topology.buffers_of_port(PortId::unchecked("leaf1/2"));
  CO_EXPECT_EQ(buffers.size(), static_cast<std::size_t>(1));
  CO_EXPECT_EQ(topology.total_objects(), static_cast<std::size_t>(21));
}

CO_TEST(model, topology_contains_and_require) {
  const Topology topology = cotest::make_test_topology(cotest::test_generation());
  CO_EXPECT(topology.contains(cotest::link_subject()));
  CO_EXPECT(topology.contains(cotest::queue_subject()));
  CO_EXPECT(!topology.contains(cotest::link_subject("no-such-link")));
  CO_EXPECT(topology.require(cotest::link_subject()).ok());
  CO_EXPECT_STATUS_ERR(topology.require(cotest::link_subject("no-such-link")), ErrorCode::kNotFound);
  CO_EXPECT_STATUS_ERR(topology.require(EvidenceSubject{}), ErrorCode::kInvalidArgument);
  // Tenants, classes and sources are not topology objects but are valid subjects.
  CO_EXPECT(topology.contains(EvidenceSubject(TenantId::unchecked("tenant-a"))));
  CO_EXPECT(topology.contains(EvidenceSubject(SourceId::unchecked("collector-1"))));
}

CO_TEST(model, topology_digest_is_content_addressed) {
  const Topology first = cotest::make_test_topology(cotest::test_generation());
  const Topology second = cotest::make_test_topology(cotest::test_generation());
  CO_EXPECT_EQ(first.digest(), second.digest());

  TopologyBuilder builder;
  builder.set_limits(cotest::test_limits());
  builder.set_generation(cotest::test_generation());
  Node node;
  node.id = NodeId::unchecked("solo");
  node.kind = NodeKind::kSwitch;
  CO_EXPECT(builder.add_node(node).ok());
  auto small = builder.build(cotest::test_limits());
  CO_EXPECT_OK(small);
  CO_EXPECT_NE(small.value().digest(), first.digest());

  const Topology other_generation =
      cotest::make_test_topology(cotest::test_generation(2, 1, 1));
  CO_EXPECT_NE(other_generation.digest(), first.digest());
}

CO_TEST(model, topology_builder_rejects_invalid_structure) {
  TopologyBuilder builder;
  builder.set_limits(cotest::test_limits());
  Node node;
  node.id = NodeId::unchecked("leaf");
  node.kind = NodeKind::kSwitch;
  CO_EXPECT(builder.add_node(node).ok());
  CO_EXPECT_STATUS_ERR(builder.add_node(node), ErrorCode::kAlreadyExists);

  Node invalid;
  CO_EXPECT_STATUS_ERR(builder.add_node(invalid), ErrorCode::kInvalidArgument);

  Port port;
  port.id = PortId::unchecked("leaf/1");
  port.node = NodeId::unchecked("unknown-node");
  CO_EXPECT_STATUS_ERR(builder.add_port(port), ErrorCode::kNotFound);
  port.node = NodeId::unchecked("leaf");
  CO_EXPECT(builder.add_port(port).ok());

  Link self_loop;
  self_loop.id = LinkId::unchecked("loop");
  self_loop.endpoint_a = PortId::unchecked("leaf/1");
  self_loop.endpoint_b = PortId::unchecked("leaf/1");
  CO_EXPECT_STATUS_ERR(builder.add_link(self_loop), ErrorCode::kInvalidArgument);

  Link dangling;
  dangling.id = LinkId::unchecked("dangling");
  dangling.endpoint_a = PortId::unchecked("leaf/1");
  dangling.endpoint_b = PortId::unchecked("nowhere/1");
  CO_EXPECT_STATUS_ERR(builder.add_link(dangling), ErrorCode::kNotFound);

  Queue queue;
  queue.id = QueueId::unchecked("q");
  queue.link = LinkId::unchecked("missing");
  CO_EXPECT_STATUS_ERR(builder.add_queue(queue), ErrorCode::kNotFound);

  Path path;
  path.id = PathId::unchecked("p");
  CO_EXPECT_STATUS_ERR(builder.add_path(path), ErrorCode::kInvalidArgument);

  Flow flow;
  flow.id = FlowId::unchecked("f");
  flow.path = PathId::unchecked("missing");
  CO_EXPECT_STATUS_ERR(builder.add_flow(flow), ErrorCode::kNotFound);
}

CO_TEST(model, topology_builder_enforces_limits) {
  Limits limits = cotest::test_limits();
  limits.max_nodes = 1;
  limits.max_links = 1;
  limits.max_ports_per_node = 1;
  TopologyBuilder builder;
  builder.set_limits(limits);
  Node first;
  first.id = NodeId::unchecked("a");
  CO_EXPECT(builder.add_node(first).ok());
  Node second;
  second.id = NodeId::unchecked("b");
  CO_EXPECT_STATUS_ERR(builder.add_node(second), ErrorCode::kLimitExceeded);

  Port port;
  port.id = PortId::unchecked("a/1");
  port.node = NodeId::unchecked("a");
  CO_EXPECT(builder.add_port(port).ok());
  Port another;
  another.id = PortId::unchecked("a/2");
  another.node = NodeId::unchecked("a");
  CO_EXPECT_STATUS_ERR(builder.add_port(another), ErrorCode::kLimitExceeded);
}

CO_TEST(model, generation_ordering_is_explicit) {
  const GenerationVector base = cotest::test_generation(1, 1, 1);
  CO_EXPECT_EQ(compare_generation(base, base), GenerationOrder::kEqual);

  GenerationVector newer_revision = base;
  newer_revision.revision = Revision(2);
  CO_EXPECT_EQ(compare_generation(newer_revision, base), GenerationOrder::kNewer);
  CO_EXPECT_EQ(compare_generation(base, newer_revision), GenerationOrder::kOlder);

  GenerationVector newer_generation = base;
  newer_generation.generation = Generation(2);
  newer_generation.revision = Revision(0);
  CO_EXPECT_EQ(compare_generation(newer_generation, base), GenerationOrder::kNewer);

  GenerationVector newer_epoch = base;
  newer_epoch.epoch = Epoch(2);
  newer_epoch.generation = Generation(1);
  newer_epoch.revision = Revision(0);
  CO_EXPECT_EQ(compare_generation(newer_epoch, base), GenerationOrder::kNewer);

  // A higher epoch with a lower generation is not orderable: it must be rejected, not guessed.
  GenerationVector inconsistent = base;
  inconsistent.epoch = Epoch(2);
  inconsistent.generation = Generation(0);
  CO_EXPECT_EQ(compare_generation(inconsistent, newer_generation), GenerationOrder::kIncomparable);

  GenerationVector older_epoch = base;
  older_epoch.epoch = Epoch(0);
  CO_EXPECT_EQ(compare_generation(older_epoch, base), GenerationOrder::kOlder);
  CO_EXPECT_EQ(base.str(), std::string("1/1/1"));
}

CO_TEST(model, fence_accepts_first_observation_and_in_order_sequences) {
  SourceFenceState state;
  const FenceVector incoming = []() {
    FenceVector vector;
    vector.source = SourceId::unchecked("s1");
    vector.boot = BootId(1);
    vector.incarnation = Incarnation(1);
    vector.gen = cotest::test_generation();
    vector.sequence = Sequence(1);
    return vector;
  }();

  FenceOutcome outcome = evaluate_fence(incoming, AuthorityLevel::kMeasured,
                                        AuthorityLevel::kUnknown, state);
  CO_EXPECT_EQ(outcome.decision, FenceDecision::kAcceptedFirstObservation);
  CO_EXPECT(outcome.accepted());
  CO_EXPECT(!outcome.liveness_reset);

  state.has_state = true;
  state.last = incoming;
  FenceVector next = incoming;
  next.sequence = Sequence(2);
  outcome = evaluate_fence(next, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state);
  CO_EXPECT_EQ(outcome.decision, FenceDecision::kAcceptedInOrder);

  FenceVector gap = incoming;
  gap.sequence = Sequence(9);
  outcome = evaluate_fence(gap, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state);
  CO_EXPECT_EQ(outcome.decision, FenceDecision::kAcceptedGapDetected);
  CO_EXPECT(outcome.sequence_gap);
  CO_EXPECT_EQ(outcome.gap_size, static_cast<std::uint64_t>(7));
}

CO_TEST(model, fence_rejects_every_replay_class) {
  const FenceVector base = []() {
    FenceVector vector;
    vector.source = SourceId::unchecked("s1");
    vector.boot = BootId(2);
    vector.incarnation = Incarnation(3);
    vector.gen = cotest::test_generation(2, 2, 2);
    vector.sequence = Sequence(10);
    return vector;
  }();
  SourceFenceState state;
  state.has_state = true;
  state.last = base;

  FenceVector replayed = base;
  replayed.sequence = Sequence(10);
  CO_EXPECT_EQ(evaluate_fence(replayed, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state).decision,
               FenceDecision::kRejectedReplayedSequence);
  replayed.sequence = Sequence(9);
  CO_EXPECT_EQ(evaluate_fence(replayed, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state).decision,
               FenceDecision::kRejectedReplayedSequence);

  FenceVector stale_revision = base;
  stale_revision.sequence = Sequence(11);
  stale_revision.gen.revision = Revision(1);
  CO_EXPECT_EQ(
      evaluate_fence(stale_revision, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state).decision,
      FenceDecision::kRejectedStaleRevision);

  FenceVector stale_generation = base;
  stale_generation.sequence = Sequence(11);
  stale_generation.gen.generation = Generation(1);
  CO_EXPECT_EQ(
      evaluate_fence(stale_generation, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state).decision,
      FenceDecision::kRejectedStaleGeneration);

  FenceVector stale_epoch = base;
  stale_epoch.sequence = Sequence(11);
  stale_epoch.gen.epoch = Epoch(1);
  CO_EXPECT_EQ(
      evaluate_fence(stale_epoch, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state).decision,
      FenceDecision::kRejectedStaleEpoch);

  FenceVector stale_incarnation = base;
  stale_incarnation.sequence = Sequence(11);
  stale_incarnation.incarnation = Incarnation(2);
  CO_EXPECT_EQ(
      evaluate_fence(stale_incarnation, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state).decision,
      FenceDecision::kRejectedStaleIncarnation);

  FenceVector inconsistent_boot = base;
  inconsistent_boot.sequence = Sequence(11);
  inconsistent_boot.incarnation = Incarnation(4);
  CO_EXPECT_EQ(
      evaluate_fence(inconsistent_boot, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state).decision,
      FenceDecision::kRejectedInconsistentBoot);

  FenceVector old_boot = base;
  old_boot.sequence = Sequence(11);
  old_boot.boot = BootId(1);
  CO_EXPECT_EQ(evaluate_fence(old_boot, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state).decision,
               FenceDecision::kRejectedStaleIncarnation);

  FenceVector forward = base;
  forward.sequence = Sequence(11);
  forward.gen.revision = Revision(3);
  CO_EXPECT_EQ(evaluate_fence(forward, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state).decision,
               FenceDecision::kAcceptedInOrder);

  FenceVector new_epoch = base;
  new_epoch.sequence = Sequence(1);
  new_epoch.gen.epoch = Epoch(3);
  CO_EXPECT_EQ(evaluate_fence(new_epoch, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state).decision,
               FenceDecision::kAcceptedEpochAdvance);

  FenceVector new_generation = base;
  new_generation.sequence = Sequence(1);
  new_generation.gen.generation = Generation(3);
  CO_EXPECT_EQ(
      evaluate_fence(new_generation, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state).decision,
      FenceDecision::kAcceptedGenerationAdvance);

  FenceVector reboot = base;
  reboot.sequence = Sequence(1);
  reboot.boot = BootId(3);
  reboot.incarnation = Incarnation(1);
  const FenceOutcome reboot_outcome =
      evaluate_fence(reboot, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state);
  CO_EXPECT_EQ(reboot_outcome.decision, FenceDecision::kAcceptedReboot);
  CO_EXPECT(reboot_outcome.liveness_reset);

  FenceVector low_authority = base;
  low_authority.sequence = Sequence(11);
  CO_EXPECT_EQ(evaluate_fence(low_authority, AuthorityLevel::kUnverified,
                              AuthorityLevel::kMeasured, state)
                   .decision,
               FenceDecision::kRejectedAuthorityTooLow);

  FenceVector no_source = base;
  no_source.source = SourceId{};
  CO_EXPECT_EQ(evaluate_fence(no_source, AuthorityLevel::kMeasured, AuthorityLevel::kUnknown, state).decision,
               FenceDecision::kRejectedUnknownSource);
}

CO_TEST(model, generation_vectors_order_by_epoch_then_generation_then_revision) {
  const GenerationVector a = cotest::test_generation(1, 5, 5);
  const GenerationVector b = cotest::test_generation(2, 1, 0);
  CO_EXPECT(a < b);
  CO_EXPECT_EQ(a, cotest::test_generation(1, 5, 5));
  CO_EXPECT_NE(a, cotest::test_generation(1, 5, 6));
}
