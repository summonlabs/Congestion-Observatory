// Congestion Observatory - strongly typed identities.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_CORE_STRONG_ID_HPP
#define CONGESTION_CORE_STRONG_ID_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "congestion/core/name.hpp"
#include "congestion/core/result.hpp"

namespace congestion {

// EntityId<Tag> gives every domain object class its own identity type. Two different kinds of
// object can therefore never be silently interchanged, compared, or used as a map key by accident.
template <class Tag>
class EntityId {
 public:
  using tag_type = Tag;

  EntityId() = default;
  explicit EntityId(Name name) : name_(std::move(name)) {}

  [[nodiscard]] static Result<EntityId> parse(std::string_view text) {
    auto parsed = Name::parse(text);
    if (!parsed.ok()) {
      return parsed.error();
    }
    return EntityId(parsed.value());
  }

  [[nodiscard]] static EntityId unchecked(std::string_view text) {
    return EntityId(Name::unchecked(text));
  }

  [[nodiscard]] bool valid() const noexcept { return name_.valid(); }
  [[nodiscard]] const Name& name() const noexcept { return name_; }
  [[nodiscard]] std::string str() const { return name_.str(); }
  [[nodiscard]] std::string_view view() const noexcept { return name_.view(); }

  friend bool operator==(const EntityId& lhs, const EntityId& rhs) noexcept {
    return lhs.name_ == rhs.name_;
  }
  friend bool operator!=(const EntityId& lhs, const EntityId& rhs) noexcept { return !(lhs == rhs); }
  friend bool operator<(const EntityId& lhs, const EntityId& rhs) noexcept {
    return lhs.name_ < rhs.name_;
  }
  friend bool operator>(const EntityId& lhs, const EntityId& rhs) noexcept { return rhs < lhs; }
  friend bool operator<=(const EntityId& lhs, const EntityId& rhs) noexcept { return !(rhs < lhs); }
  friend bool operator>=(const EntityId& lhs, const EntityId& rhs) noexcept { return !(lhs < rhs); }

  // Deterministic ordering functor for ordered containers that must expose stable iteration.
  struct Less {
    [[nodiscard]] bool operator()(const EntityId& lhs, const EntityId& rhs) const noexcept {
      return lhs.name_ < rhs.name_;
    }
  };

 private:
  Name name_{};
};

// Monotonic numeric identities: epoch, generation, revision, incarnation, sequence.
template <class Tag, class Rep = std::uint64_t>
class StrongNumber {
 public:
  using tag_type = Tag;
  using rep_type = Rep;

  constexpr StrongNumber() = default;
  constexpr explicit StrongNumber(Rep value) : value_(value) {}

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] std::string str() const { return std::to_string(value_); }

  constexpr StrongNumber& operator++() noexcept {
    ++value_;
    return *this;
  }
  constexpr StrongNumber operator++(int) noexcept {
    StrongNumber copy(*this);
    ++value_;
    return copy;
  }
  friend constexpr bool operator==(StrongNumber lhs, StrongNumber rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr bool operator!=(StrongNumber lhs, StrongNumber rhs) noexcept {
    return !(lhs == rhs);
  }
  friend constexpr bool operator<(StrongNumber lhs, StrongNumber rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }
  friend constexpr bool operator>(StrongNumber lhs, StrongNumber rhs) noexcept { return rhs < lhs; }
  friend constexpr bool operator<=(StrongNumber lhs, StrongNumber rhs) noexcept {
    return !(rhs < lhs);
  }
  friend constexpr bool operator>=(StrongNumber lhs, StrongNumber rhs) noexcept {
    return !(lhs < rhs);
  }

 private:
  Rep value_{0};
};

struct EpochTag;
struct GenerationTag;
struct RevisionTag;
struct IncarnationTag;
struct SequenceTag;
struct BootTag;
struct PolicyVersionTag;

using Epoch = StrongNumber<EpochTag>;
using Generation = StrongNumber<GenerationTag>;
using Revision = StrongNumber<RevisionTag>;
using Incarnation = StrongNumber<IncarnationTag>;
using Sequence = StrongNumber<SequenceTag>;
using BootId = StrongNumber<BootTag>;

}  // namespace congestion

#endif  // CONGESTION_CORE_STRONG_ID_HPP
