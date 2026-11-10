// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// The daemon's single-threaded event loop (poll), listening sockets and a
/// set of connected clients with bounded, non-blocking output buffers.
#ifndef LGRTK_SRC_DAEMON_EVENT_LOOP_H_
#define LGRTK_SRC_DAEMON_EVENT_LOOP_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "internal/unique_fd.h"

namespace lgrtk::daemon {

/// poll()-based loop. Everything runs on the thread that calls Run(); only
/// Post() and Quit() may be called from other threads.
class EventLoop {
 public:
  /// Creates the wake-up eventfd. Must succeed before anything else.
  bool Init(std::string* err);
  /// Watches `fd` for `events` (POLLIN/POLLOUT); `cb` gets the revents.
  void Add(int fd, short events, std::function<void(short)> cb);
  /// Changes the events of a watched fd.
  void Modify(int fd, short events);
  /// Stops watching `fd` (does not close it). Safe inside a callback.
  void Remove(int fd);
  /// Runs `fn` on the loop thread soon. Thread-safe.
  void Post(std::function<void()> fn);
  /// Runs `fn` every `period_ms` on the loop thread.
  void Every(int64_t period_ms, std::function<void()> fn);
  /// Processes events until Quit().
  void Run();
  /// Makes Run() return. Thread-safe.
  void Quit();

 private:
  struct Watch {
    short events;
    uint64_t gen;
    std::function<void(short)> cb;
  };
  struct Timer {
    int64_t period_ms;
    int64_t next_ms;
    std::function<void()> fn;
  };
  void Wake();
  void DrainPosted();

  internal::UniqueFd wake_fd_;
  std::map<int, Watch> watches_;
  uint64_t gen_ = 0;
  std::vector<Timer> timers_;
  std::mutex mu_;
  std::vector<std::function<void()>> posted_;
  std::atomic<bool> quit_{false};
};

/// A non-blocking TCP listening socket on host:port (port 0 picks a free
/// one); an invalid fd and `err` on failure.
[[nodiscard]] internal::UniqueFd ListenTcp(const std::string& host, int port,
                                           std::string* err);
/// A non-blocking Unix stream listening socket; replaces a stale socket file.
[[nodiscard]] internal::UniqueFd ListenUnix(const std::string& path,
                                            std::string* err);
/// The local port of a bound TCP socket.
int BoundPort(int fd);

/// Output buffered per client before it is dropped (docs/decisions.md D24).
inline constexpr size_t kClientBufferCap = 256 * 1024;
/// Clients per listening socket (docs/decisions.md D24).
inline constexpr size_t kMaxClients = 64;
/// Longest input line; a client sending a longer one is dropped.
inline constexpr size_t kMaxLineBytes = 4096;
/// Pending connections per listening socket.
inline constexpr int kListenBacklog = 16;

/// Limits and input handling of a ClientSet.
struct ClientSetOptions {
  size_t cap_bytes = kClientBufferCap;  ///< output buffer per client
  bool read_lines = true;               ///< false: discard all input
  size_t max_line = kMaxLineBytes;      ///< longest input line
  size_t max_clients = kMaxClients;     ///< further clients are closed
};

/// Clients accepted from one or more listening sockets. Input is split into
/// lines (or discarded); output is buffered per client up to `cap_bytes`, and
/// a client whose buffer would exceed that is disconnected, so a client that
/// does not read never slows the daemon.
class ClientSet {
 public:
  using Id = uint64_t;               ///< client handle, never reused
  using Options = ClientSetOptions;  ///< limits and input handling
  /// Callbacks; all run on the loop thread.
  struct Handlers {
    std::function<void(Id)> on_open;                    ///< accepted
    std::function<void(Id, std::string_view)> on_line;  ///< one input line
    std::function<void(Id)> on_close;                   ///< gone
  };
  /// Serves clients on `loop` with `options`.
  ClientSet(EventLoop* loop, Handlers handlers, Options options = {});
  ~ClientSet();
  ClientSet(const ClientSet&) = delete;
  ClientSet& operator=(const ClientSet&) = delete;

  /// Accepts clients from `listen_fd`.
  void AddListener(internal::UniqueFd listen_fd);
  /// Listens on host:port (0 picks a free port) and accepts clients from it.
  /// Returns the bound port, or 0 with `err`.
  [[nodiscard]] int AddTcpListener(const std::string& host, int port,
                                   std::string* err);
  /// Queues `data` for client `id`; drops the client on overflow.
  void Send(Id id, std::string_view data);
  /// Disconnects a client.
  void Close(Id id);
  /// Ids of all connected clients.
  [[nodiscard]] std::vector<Id> Ids() const;
  /// Number of connected clients.
  [[nodiscard]] size_t Count() const { return clients_.size(); }
  /// Clients dropped because their buffer overflowed.
  [[nodiscard]] uint64_t Dropped() const { return dropped_; }

 private:
  struct Client {
    internal::UniqueFd fd;
    std::string in;
    std::string out;
  };
  void Accept(int listen_fd);
  void OnEvents(Id id, short revents);
  void Flush(Id id, Client* c);

  EventLoop* loop_;
  Handlers h_;
  Options opt_;
  bool accept_paused_ = false;  // out of file descriptors
  std::vector<internal::UniqueFd> listeners_;
  std::map<Id, std::unique_ptr<Client>> clients_;
  Id next_id_ = 1;
  uint64_t dropped_ = 0;
};

}  // namespace lgrtk::daemon

#endif  // LGRTK_SRC_DAEMON_EVENT_LOOP_H_
