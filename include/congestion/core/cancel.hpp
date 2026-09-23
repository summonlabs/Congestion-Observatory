// Congestion Observatory - real cancellation and shutdown signalling.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_CORE_CANCEL_HPP
#define CONGESTION_CORE_CANCEL_HPP

#include <atomic>
#include <memory>
#include <string>

namespace congestion {

// Cancellation is cooperative and observable. Long running operations poll the token at bounded
// intervals and return ErrorCode::kCancelled; they never rely on a timeout to make progress.
class CancellationToken {
 public:
  CancellationToken() = default;
  explicit CancellationToken(std::shared_ptr<struct CancelState> state) : state_(std::move(state)) {}

  [[nodiscard]] bool cancelled() const noexcept;
  [[nodiscard]] std::string reason() const;

 private:
  std::shared_ptr<CancelState> state_{};
};

// Owner side of the token. Cancellation is idempotent and monotonic: once cancelled, a token
// never returns to the running state.
class CancellationSource {
 public:
  CancellationSource();

  void cancel(std::string reason);
  [[nodiscard]] bool cancelled() const noexcept;
  [[nodiscard]] CancellationToken token() const noexcept { return CancellationToken(state_); }

 private:
  std::shared_ptr<CancelState> state_;
};

}  // namespace congestion

#endif  // CONGESTION_CORE_CANCEL_HPP
