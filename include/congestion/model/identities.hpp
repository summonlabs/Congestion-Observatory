// Congestion Observatory - typed identities for every domain object.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_MODEL_IDENTITIES_HPP
#define CONGESTION_MODEL_IDENTITIES_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "congestion/core/hash.hpp"
#include "congestion/core/result.hpp"
#include "congestion/core/strong_id.hpp"

namespace congestion {

struct NodeTag;
struct PortTag;
struct LinkTag;
struct QueueTag;
struct BufferTag;
struct PathTag;
struct FlowTag;
struct SourceTag;
struct TenantTag;
struct ClassTag;
struct TopologyTag;
struct EvidenceTag;
struct EpisodeTag;
struct GroupTag;

using NodeId = EntityId<NodeTag>;
using PortId = EntityId<PortTag>;
using LinkId = EntityId<LinkTag>;
using QueueId = EntityId<QueueTag>;
using BufferId = EntityId<BufferTag>;
using PathId = EntityId<PathTag>;
using FlowId = EntityId<FlowTag>;
using SourceId = EntityId<SourceTag>;
using TenantId = EntityId<TenantTag>;
using ClassId = EntityId<ClassTag>;
using TopologyId = EntityId<TopologyTag>;

// DigestId is the identity of a *derived* object: its value is a deterministic function of the
// canonical encoding of the object's identity-bearing fields. Two runs, two machines and two
// builds therefore agree on the identity of the same episode or evidence record. This is an
// integrity/disambiguation aid, not a cryptographic guarantee.
template <class Tag>
class DigestId {
 public:
  using tag_type = Tag;
  static constexpr std::size_t kHexLength = 32;

  DigestId() = default;

  [[nodiscard]] static DigestId from_digest(const std::array<std::uint64_t, 2>& digest) noexcept {
    DigestId id;
    id.value_ = digest;
    return id;
  }

  [[nodiscard]] static Result<DigestId> parse(std::string_view hex) {
    if (hex.size() != kHexLength) {
      return make_error(ErrorCode::kInvalidArgument, "digest id must be 32 hex characters");
    }
    std::array<std::uint64_t, 2> value{0, 0};
    for (std::size_t i = 0; i < kHexLength; ++i) {
      const char c = hex[i];
      std::uint64_t nibble = 0;
      if (c >= '0' && c <= '9') {
        nibble = static_cast<std::uint64_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        nibble = static_cast<std::uint64_t>(c - 'a' + 10);
      } else {
        return make_error(ErrorCode::kInvalidArgument,
                          "digest id must be lower case hexadecimal");
      }
      const std::size_t word = i / 16;
      value[word] = (value[word] << 4) | nibble;
    }
    return from_digest(value);
  }

  [[nodiscard]] bool valid() const noexcept { return value_[0] != 0 || value_[1] != 0; }
  [[nodiscard]] const std::array<std::uint64_t, 2>& digest() const noexcept { return value_; }
  [[nodiscard]] std::string str() const { return to_hex(value_); }

  friend bool operator==(const DigestId& lhs, const DigestId& rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend bool operator!=(const DigestId& lhs, const DigestId& rhs) noexcept { return !(lhs == rhs); }
  friend bool operator<(const DigestId& lhs, const DigestId& rhs) noexcept {
    if (lhs.value_[0] != rhs.value_[0]) {
      return lhs.value_[0] < rhs.value_[0];
    }
    return lhs.value_[1] < rhs.value_[1];
  }
  friend bool operator>(const DigestId& lhs, const DigestId& rhs) noexcept { return rhs < lhs; }

  struct Less {
    [[nodiscard]] bool operator()(const DigestId& lhs, const DigestId& rhs) const noexcept {
      return lhs < rhs;
    }
  };

 private:
  std::array<std::uint64_t, 2> value_{0, 0};
};

using EvidenceId = DigestId<EvidenceTag>;
using EpisodeId = DigestId<EpisodeTag>;
using GroupId = DigestId<GroupTag>;

// Kind tag of an evidence subject. Present so switches and maps over subjects stay exhaustive.
enum class SubjectKind : std::uint8_t {
  kNone = 0,
  kNode = 1,
  kPort = 2,
  kLink = 3,
  kQueue = 4,
  kBuffer = 5,
  kPath = 6,
  kFlow = 7,
  kTenant = 8,
  kClass = 9,
  kSource = 10,
  kTopology = 11,
};

[[nodiscard]] std::string_view to_string(SubjectKind kind) noexcept;

// EvidenceSubject names the object an observation is about. The variant order is fixed and is
// part of the canonical encoding, so serialised subjects are stable across builds.
class EvidenceSubject {
 public:
  using Storage = std::variant<std::monostate, NodeId, PortId, LinkId, QueueId, BufferId, PathId,
                               FlowId, TenantId, ClassId, SourceId, TopologyId>;

  EvidenceSubject() = default;
  explicit EvidenceSubject(NodeId id) : storage_(std::move(id)) {}
  explicit EvidenceSubject(PortId id) : storage_(std::move(id)) {}
  explicit EvidenceSubject(LinkId id) : storage_(std::move(id)) {}
  explicit EvidenceSubject(QueueId id) : storage_(std::move(id)) {}
  explicit EvidenceSubject(BufferId id) : storage_(std::move(id)) {}
  explicit EvidenceSubject(PathId id) : storage_(std::move(id)) {}
  explicit EvidenceSubject(FlowId id) : storage_(std::move(id)) {}
  explicit EvidenceSubject(TenantId id) : storage_(std::move(id)) {}
  explicit EvidenceSubject(ClassId id) : storage_(std::move(id)) {}
  explicit EvidenceSubject(SourceId id) : storage_(std::move(id)) {}
  explicit EvidenceSubject(TopologyId id) : storage_(std::move(id)) {}

  [[nodiscard]] SubjectKind kind() const noexcept;
  [[nodiscard]] bool valid() const noexcept { return kind() != SubjectKind::kNone; }

  // Canonical "kind:name" rendering, e.g. "link:leaf1-uplink".
  [[nodiscard]] std::string str() const;

  [[nodiscard]] const Storage& storage() const noexcept { return storage_; }

  template <class T>
  [[nodiscard]] const T* get_if() const noexcept {
    return std::get_if<T>(&storage_);
  }

  // Total order used to make every subject-ordered output deterministic.
  friend bool operator<(const EvidenceSubject& lhs, const EvidenceSubject& rhs) noexcept {
    const auto lk = static_cast<std::uint8_t>(lhs.kind());
    const auto rk = static_cast<std::uint8_t>(rhs.kind());
    if (lk != rk) {
      return lk < rk;
    }
    return lhs.str() < rhs.str();
  }
  friend bool operator==(const EvidenceSubject& lhs, const EvidenceSubject& rhs) noexcept {
    return !(lhs < rhs) && !(rhs < lhs);
  }
  friend bool operator!=(const EvidenceSubject& lhs, const EvidenceSubject& rhs) noexcept {
    return !(lhs == rhs);
  }

  struct Less {
    [[nodiscard]] bool operator()(const EvidenceSubject& lhs, const EvidenceSubject& rhs) const noexcept {
      return lhs < rhs;
    }
  };

 private:
  Storage storage_{};
};

// Parses "kind:name" into a subject. Unknown kinds are rejected rather than defaulted.
[[nodiscard]] Result<EvidenceSubject> parse_subject(std::string_view text);

}  // namespace congestion

#endif  // CONGESTION_MODEL_IDENTITIES_HPP
