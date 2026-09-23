// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/core/cancel.hpp"

#include <mutex>

namespace congestion {

// Definition of the state type declared in the header. Cancellation is a one way latch.
struct CancelState {
  std::atomic<bool> cancelled{false};
  mutable std::mutex mutex{};
  std::string reason{};
};

bool CancellationToken::cancelled() const noexcept {
  return state_ != nullptr && state_->cancelled.load(std::memory_order_acquire);
}

std::string CancellationToken::reason() const {
  if (state_ == nullptr) {
    return {};
  }
  const std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->reason;
}

CancellationSource::CancellationSource() : state_(std::make_shared<CancelState>()) {}

void CancellationSource::cancel(std::string reason) {
  if (state_ == nullptr) {
    return;
  }
  {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->reason.empty()) {
      state_->reason = std::move(reason);
    }
  }
  state_->cancelled.store(true, std::memory_order_release);
}

bool CancellationSource::cancelled() const noexcept {
  return state_ != nullptr && state_->cancelled.load(std::memory_order_acquire);
}

}  // namespace congestion
