// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "congestion/transport/socket.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace congestion {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

std::once_flag g_network_once;
std::atomic<int> g_network_result{-1};

void initialise_network_once() {
  WSADATA data;
  const int result = WSAStartup(MAKEWORD(2, 2), &data);
  g_network_result.store(result, std::memory_order_release);
}

int last_error() { return WSAGetLastError(); }

#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;

std::once_flag g_network_once;
std::atomic<int> g_network_result{0};

void initialise_network_once() { g_network_result.store(0, std::memory_order_release); }

int last_error() { return errno; }
#endif

#if defined(_WIN32)
int close_socket(NativeSocket socket) { return ::closesocket(socket); }
#else
int close_socket(NativeSocket socket) { return ::close(socket); }
#endif

NativeSocket from_handle(std::uintptr_t handle) { return static_cast<NativeSocket>(handle); }
std::uintptr_t to_handle(NativeSocket socket) { return static_cast<std::uintptr_t>(socket); }

bool wait_readable(NativeSocket socket, Duration deadline) {
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(socket, &read_set);
  timeval timeout;
  const std::int64_t nanos = deadline.nanos() < 0 ? 0 : deadline.nanos();
  timeout.tv_sec = static_cast<long>(nanos / 1000000000LL);
  timeout.tv_usec = static_cast<long>((nanos % 1000000000LL) / 1000LL);
#if defined(_WIN32)
  const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
#else
  const int ready = select(socket + 1, &read_set, nullptr, nullptr, &timeout);
#endif
  return ready > 0;
}

}  // namespace

Status ensure_network_initialised() {
  std::call_once(g_network_once, initialise_network_once);
  if (g_network_result.load(std::memory_order_acquire) != 0) {
    return Status(make_error(ErrorCode::kIoError, "network subsystem initialisation failed",
                             std::to_string(g_network_result.load(std::memory_order_acquire))));
  }
  return Status{};
}

void shutdown_network() noexcept {
#if defined(_WIN32)
  WSACleanup();
#endif
}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept
    : handle_(other.handle_), peer_port_(other.peer_port_), options_(other.options_) {
  other.handle_ = ~static_cast<std::uintptr_t>(0);
  other.peer_port_ = 0;
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    peer_port_ = other.peer_port_;
    options_ = other.options_;
    other.handle_ = ~static_cast<std::uintptr_t>(0);
    other.peer_port_ = 0;
  }
  return *this;
}

Result<Socket> Socket::connect_tcp(const std::string& host, std::uint16_t port,
                                   const SocketOptions& options) {
  const Status network = ensure_network_initialised();
  if (!network.ok()) {
    return network.error();
  }
  addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  const int resolved = getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
  if (resolved != 0 || results == nullptr) {
    return make_error(ErrorCode::kIoError, "endpoint could not be resolved", host);
  }
  NativeSocket socket = kInvalidSocket;
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (socket == kInvalidSocket) {
      continue;
    }
    if (::connect(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      break;
    }
    close_socket(socket);
    socket = kInvalidSocket;
  }
  freeaddrinfo(results);
  if (socket == kInvalidSocket) {
    return make_error(ErrorCode::kIoError, "connection failed",
                      host + ":" + service + " error=" + std::to_string(last_error()));
  }
  Socket result(to_handle(socket), port);
  result.options_ = options;
  if (options.tcp_no_delay) {
    const int one = 1;
    ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                 static_cast<int>(sizeof(one)));
  }
  return result;
}

Status Socket::send_all(const void* data, std::size_t size) {
  if (!valid()) {
    return Status(make_error(ErrorCode::kIoError, "socket is not connected"));
  }
  const char* bytes = static_cast<const char*>(data);
  std::size_t sent = 0;
  while (sent < size) {
    const int chunk = ::send(from_handle(handle_), bytes + sent,
                             static_cast<int>(size - sent), 0);
    if (chunk <= 0) {
      return Status(make_error(ErrorCode::kIoError, "send failed",
                               "error=" + std::to_string(last_error())));
    }
    sent += static_cast<std::size_t>(chunk);
  }
  return Status{};
}

Result<std::size_t> Socket::recv_some(char* buffer, std::size_t capacity) {
  if (!valid()) {
    return make_error(ErrorCode::kIoError, "socket is not connected");
  }
  if (!wait_readable(from_handle(handle_), options_.io_deadline)) {
    return make_error(ErrorCode::kIoError, "read deadline expired",
                      options_.io_deadline.to_string());
  }
  const int received = ::recv(from_handle(handle_), buffer, static_cast<int>(capacity), 0);
  if (received < 0) {
    return make_error(ErrorCode::kIoError, "receive failed",
                      "error=" + std::to_string(last_error()));
  }
  return static_cast<std::size_t>(received);
}

void Socket::close() noexcept {
  if (handle_ != ~static_cast<std::uintptr_t>(0)) {
    close_socket(from_handle(handle_));
    handle_ = ~static_cast<std::uintptr_t>(0);
  }
}

bool Socket::valid() const noexcept { return handle_ != ~static_cast<std::uintptr_t>(0); }

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_), port_(other.port_), options_(other.options_) {
  other.handle_ = ~static_cast<std::uintptr_t>(0);
  other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    port_ = other.port_;
    options_ = other.options_;
    other.handle_ = ~static_cast<std::uintptr_t>(0);
    other.port_ = 0;
  }
  return *this;
}

Result<TcpListener> TcpListener::bind_loopback(std::uint16_t port, const SocketOptions& options) {
  const Status network = ensure_network_initialised();
  if (!network.ok()) {
    return network.error();
  }
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    return make_error(ErrorCode::kIoError, "listener socket could not be created");
  }
  const int one = 1;
  ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
               static_cast<int>(sizeof(one)));

  sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::bind(socket, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
    close_socket(socket);
    return make_error(ErrorCode::kIoError, "listener could not bind",
                      "port=" + std::to_string(port) + " error=" + std::to_string(last_error()));
  }
  if (::listen(socket, 8) != 0) {
    close_socket(socket);
    return make_error(ErrorCode::kIoError, "listener could not listen");
  }

  sockaddr_in bound;
  std::memset(&bound, 0, sizeof(bound));
#if defined(_WIN32)
  int bound_length = static_cast<int>(sizeof(bound));
#else
  socklen_t bound_length = static_cast<socklen_t>(sizeof(bound));
#endif
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
    close_socket(socket);
    return make_error(ErrorCode::kIoError, "listener address could not be read");
  }

  TcpListener listener;
  listener.handle_ = to_handle(socket);
  listener.port_ = ntohs(bound.sin_port);
  listener.options_ = options;
  return listener;
}

Result<Socket> TcpListener::accept() {
  if (!valid()) {
    return make_error(ErrorCode::kIoError, "listener is not bound");
  }
  if (!wait_readable(from_handle(handle_), options_.io_deadline)) {
    return make_error(ErrorCode::kIoError, "accept deadline expired", options_.io_deadline.to_string());
  }
  sockaddr_in peer;
  std::memset(&peer, 0, sizeof(peer));
#if defined(_WIN32)
  int peer_length = static_cast<int>(sizeof(peer));
#else
  socklen_t peer_length = static_cast<socklen_t>(sizeof(peer));
#endif
  const NativeSocket socket =
      ::accept(from_handle(handle_), reinterpret_cast<sockaddr*>(&peer), &peer_length);
  if (socket == kInvalidSocket) {
    return make_error(ErrorCode::kIoError, "accept failed",
                      "error=" + std::to_string(last_error()));
  }
  Socket result(to_handle(socket), ntohs(peer.sin_port));
  result.options_ = options_;
  if (options_.tcp_no_delay) {
    const int one = 1;
    ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                 static_cast<int>(sizeof(one)));
  }
  return result;
}

void TcpListener::close() noexcept {
  if (handle_ != ~static_cast<std::uintptr_t>(0)) {
    close_socket(from_handle(handle_));
    handle_ = ~static_cast<std::uintptr_t>(0);
  }
}

bool TcpListener::valid() const noexcept { return handle_ != ~static_cast<std::uintptr_t>(0); }

}  // namespace congestion
