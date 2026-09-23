// Congestion Observatory - thin, testable TCP transport layer.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_TRANSPORT_SOCKET_HPP
#define CONGESTION_TRANSPORT_SOCKET_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "congestion/core/result.hpp"
#include "congestion/core/time.hpp"

namespace congestion {

// Operation deadlines are a property of the transport, not of the tests: a collector must not
// block forever on a peer that stopped talking. Tests configure long deadlines so that a defect
// surfaces as a reported protocol error rather than as a hang.
struct SocketOptions {
  Duration io_deadline{Duration::from_seconds(5)};
  bool tcp_no_delay{true};
};

class Socket {
 public:
  Socket() = default;
  ~Socket();

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  [[nodiscard]] static Result<Socket> connect_tcp(const std::string& host, std::uint16_t port,
                                                  const SocketOptions& options);

  // Sends everything or reports the failure. Partial sends are handled internally.
  [[nodiscard]] Status send_all(const void* data, std::size_t size);
  [[nodiscard]] Status send_all(const std::string& text) { return send_all(text.data(), text.size()); }

  // Receives at least one byte. Returns 0 on orderly shutdown by the peer.
  [[nodiscard]] Result<std::size_t> recv_some(char* buffer, std::size_t capacity);

  void close() noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint16_t peer_port() const noexcept { return peer_port_; }

 private:
  friend class TcpListener;
  explicit Socket(std::uintptr_t handle, std::uint16_t peer_port) noexcept
      : handle_(handle), peer_port_(peer_port) {}

  std::uintptr_t handle_{~static_cast<std::uintptr_t>(0)};
  std::uint16_t peer_port_{0};
  SocketOptions options_{};
};

class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();

  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  // Binds to the loopback interface. Port 0 selects an ephemeral port.
  [[nodiscard]] static Result<TcpListener> bind_loopback(std::uint16_t port,
                                                         const SocketOptions& options);

  [[nodiscard]] Result<Socket> accept();
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  void close() noexcept;
  [[nodiscard]] bool valid() const noexcept;

 private:
  std::uintptr_t handle_{~static_cast<std::uintptr_t>(0)};
  std::uint16_t port_{0};
  SocketOptions options_{};
};

// One-time network subsystem initialisation. Idempotent and safe to call from any thread.
[[nodiscard]] Status ensure_network_initialised();

// Releases the network subsystem. Called at process exit; tests may call it explicitly.
void shutdown_network() noexcept;

}  // namespace congestion

#endif  // CONGESTION_TRANSPORT_SOCKET_HPP
