#include "core/engine.hpp"

#include "core/input.hpp"
#include "fiber/limits.hpp"
#include "fiber/terminal/terminal.hpp"
#include "platform/io.hpp"
#include "platform/pty.hpp"
#include "protocol/single_pane.hpp"
#include "render/single_pane.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fiber::core {
namespace {

constexpr auto command_attach = protocol::wire_byte(protocol::ControlCommand::attach);
constexpr auto command_create = protocol::wire_byte(protocol::ControlCommand::create);
constexpr auto command_list = protocol::wire_byte(protocol::ControlCommand::list);
constexpr auto command_list_workspace =
    protocol::wire_byte(protocol::ControlCommand::list_workspace);
constexpr auto command_kill = protocol::wire_byte(protocol::ControlCommand::kill);
constexpr auto command_kill_all = protocol::wire_byte(protocol::ControlCommand::kill_all);
constexpr auto response_ready = protocol::wire_byte(protocol::ControlResponse::ready);
constexpr auto response_busy = protocol::wire_byte(protocol::ControlResponse::busy);
constexpr auto response_missing = protocol::wire_byte(protocol::ControlResponse::missing);
constexpr auto response_capacity = protocol::wire_byte(protocol::ControlResponse::capacity);
constexpr auto response_failed = protocol::wire_byte(protocol::ControlResponse::failed);
using platform::close_descriptor;
using platform::read_exact;
using platform::send_all;
using platform::send_text;
using platform::set_nonblocking;
using platform::write_all;
using render::ClientOutputState;
using render::flush_frame;
using render::FrameBuffer;
using render::queue_frame;
using render::send_frame;

class EndpointReleaseGuard final {
public:
  EndpointReleaseGuard(const EndpointRelease release, void* const context) noexcept
      : release_(release), context_(context) {}

  EndpointReleaseGuard(const EndpointReleaseGuard&) = delete;
  auto operator=(const EndpointReleaseGuard&) -> EndpointReleaseGuard& = delete;
  EndpointReleaseGuard(EndpointReleaseGuard&&) = delete;
  auto operator=(EndpointReleaseGuard&&) -> EndpointReleaseGuard& = delete;

  ~EndpointReleaseGuard() { release(); }

  void release() noexcept {
    if (release_ == nullptr) {
      return;
    }
    release_(context_);
    release_ = nullptr;
  }

private:
  EndpointRelease release_;
  void* context_;
};

[[nodiscard]] auto apply_resize(const int pty, vt::Terminal& terminal,
                                const std::uint16_t requested_columns,
                                const std::uint16_t requested_rows) noexcept -> bool {
  const auto columns = std::clamp(requested_columns, std::uint16_t{1}, protocol::columns_max);
  const auto rows = std::clamp(requested_rows, std::uint16_t{1}, protocol::rows_max);
  return platform::resize_pty(pty, columns, rows) &&
         terminal.resize({.columns = columns, .rows = rows}).has_value();
}

enum class ParseResult : std::uint8_t {
  keep,
  detach,
  error,
};

class ClientPacketParser final {
public:
  [[nodiscard]] auto receive(const int client, const int pty, vt::Terminal& terminal) noexcept
      -> ParseResult {
    const auto available = decoder_.writable_bytes();
    if (available.empty()) {
      return ParseResult::error;
    }
    const auto bytes_read = ::recv(client, available.data(), available.size(), 0);
    if (bytes_read == 0) {
      return ParseResult::detach;
    }
    if (bytes_read < 0) {
      return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ? ParseResult::keep
                                                                       : ParseResult::detach;
    }
    if (!decoder_.commit(static_cast<std::size_t>(bytes_read)).has_value()) {
      return ParseResult::error;
    }
    return parse(pty, terminal);
  }

  void reset() noexcept { decoder_.reset(); }

private:
  [[nodiscard]] auto parse(const int pty, vt::Terminal& terminal) noexcept -> ParseResult {
    while (true) {
      const auto decoded = decoder_.next();
      if (!decoded.has_value()) {
        return ParseResult::error;
      }
      if (!decoded->has_value()) {
        return ParseResult::keep;
      }

      const auto& message = **decoded;
      switch (message.kind) {
      case protocol::ClientMessageKind::detach:
        decoder_.consume();
        return ParseResult::detach;
      case protocol::ClientMessageKind::resize:
        if (!apply_resize(pty, terminal, message.dimensions.columns, message.dimensions.rows)) {
          return ParseResult::error;
        }
        break;
      case protocol::ClientMessageKind::input:
        if (!write_normalized_input(pty, terminal, message.input)) {
          return ParseResult::error;
        }
        break;
      }
      decoder_.consume();
    }
  }

  protocol::ClientDecoder decoder_{};
};

[[nodiscard]] auto drain_terminal_responses(const int pty, vt::Terminal& terminal) noexcept
    -> bool {
  std::array<std::byte, std::size_t{4} * 1'024U> response{};
  while (terminal.pending_pty_response_bytes() > 0) {
    const auto size = terminal.read_pty_responses(response);
    if (size == 0 || !write_all(pty, std::span(response).first(size))) {
      return false;
    }
  }
  return true;
}

struct PtyDrainResult final {
  bool alive{true};
  bool changed{false};
};

[[nodiscard]] auto drain_pty(const int pty, vt::Terminal& terminal) noexcept -> PtyDrainResult {
  constexpr std::size_t reads_per_turn_max = 4;
  std::array<std::byte, std::size_t{64} * 1'024U> output{};
  PtyDrainResult drain{};
  for (std::size_t read_count = 0; read_count < reads_per_turn_max; ++read_count) {
    const auto bytes_read = ::read(pty, output.data(), output.size());
    if (bytes_read > 0) {
      terminal.write(std::span(output).first(static_cast<std::size_t>(bytes_read)));
      drain.changed = true;
      if (!drain_terminal_responses(pty, terminal)) {
        drain.alive = false;
        return drain;
      }
      continue;
    }
    if (bytes_read == 0) {
      drain.alive = false;
      return drain;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      drain.alive = false;
    }
    return drain;
  }
  return drain;
}

struct WorkspaceName final {
  std::array<char, protocol::workspace_name_bytes_max> bytes{};
  std::size_t size{0};

  [[nodiscard]] auto view() const noexcept -> std::string_view { return {bytes.data(), size}; }
};

[[nodiscard]] constexpr auto valid_workspace_name(const std::string_view workspace) noexcept
    -> bool {
  if (workspace.empty() || workspace.size() > protocol::workspace_name_bytes_max) {
    return false;
  }
  return std::ranges::all_of(workspace, [](const char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-';
  });
}

[[nodiscard]] auto read_workspace_name(const int connection) noexcept
    -> std::optional<WorkspaceName> {
  std::array<std::byte, 1> encoded_size{};
  if (!read_exact(connection, encoded_size)) {
    return std::nullopt;
  }
  WorkspaceName workspace;
  workspace.size = protocol::decode_workspace_name_size(encoded_size.front());
  if (workspace.size == 0 || workspace.size > workspace.bytes.size() ||
      !read_exact(connection,
                  std::as_writable_bytes(std::span(workspace.bytes).first(workspace.size))) ||
      !valid_workspace_name(workspace.view())) {
    return std::nullopt;
  }
  return workspace;
}

struct Workspace final {
  Workspace(const std::string_view workspace_name, vt::Terminal&& created_terminal,
            std::unique_ptr<FrameBuffer> created_frame) noexcept
      : name_size(workspace_name.size()), terminal(std::move(created_terminal)),
        frame(std::move(created_frame)) {
    std::memcpy(name.data(), workspace_name.data(), workspace_name.size());
  }

  Workspace(const Workspace&) = delete;
  auto operator=(const Workspace&) -> Workspace& = delete;
  Workspace(Workspace&&) = delete;
  auto operator=(Workspace&&) -> Workspace& = delete;

  ~Workspace() {
    close_descriptor(client);
    if (child > 0) {
      static_cast<void>(::kill(child, SIGHUP));
      child = -1;
    }
    close_descriptor(pty);
  }

  [[nodiscard]] auto workspace_name() const noexcept -> std::string_view {
    return {name.data(), name_size};
  }

  void detach_client() noexcept {
    close_descriptor(client);
    parser.reset();
    output.reset();
    frame_pending = false;
  }

  std::array<char, protocol::workspace_name_bytes_max> name{};
  std::size_t name_size{0};
  vt::Terminal terminal;
  std::unique_ptr<FrameBuffer> frame;
  ClientPacketParser parser;
  ClientOutputState output;
  int pty{-1};
  pid_t child{-1};
  int client{-1};
  bool active{true};
  bool frame_pending{false};
  std::chrono::steady_clock::time_point frame_deadline;
};

[[nodiscard]] auto create_workspace(const std::string_view name) noexcept
    -> std::unique_ptr<Workspace> {
  vt::TerminalOptions options;
  options.size = {.columns = 80, .rows = 24};
  auto terminal_result = vt::Terminal::create(options);
  if (!terminal_result.has_value()) {
    return nullptr;
  }
  auto frame = std::unique_ptr<FrameBuffer>(new (std::nothrow) FrameBuffer{});
  if (frame == nullptr) {
    return nullptr;
  }
  auto workspace = std::unique_ptr<Workspace>(
      new (std::nothrow) Workspace(name, std::move(*terminal_result), std::move(frame)));
  if (workspace == nullptr) {
    return nullptr;
  }
  workspace->child = platform::spawn_login_shell(workspace->pty);
  if (workspace->child <= 0 || !set_nonblocking(workspace->pty)) {
    return nullptr;
  }
  return workspace;
}

[[nodiscard]] auto send_safe_title(const int socket, const std::string_view title) noexcept
    -> bool {
  std::array<char, 256> sanitized{};
  std::size_t used = 0;
  const std::span<const char> title_characters(title);
  for (const char character :
       title_characters.first(std::min(title_characters.size(), sanitized.size()))) {
    const auto value = static_cast<unsigned char>(character);
    std::span(sanitized).subspan(used, 1).front() =
        value < 0x20U || value == 0x7FU ? '?' : character;
    ++used;
  }
  return send_text(socket, std::string_view(sanitized.data(), used));
}

[[nodiscard]] auto send_number(const int socket, const std::uint64_t value) noexcept -> bool {
  std::array<char, 32> buffer{};
  const auto result = std::to_chars(buffer.begin(), buffer.end(), value);
  if (result.ec != std::errc{}) {
    return false;
  }
  const auto size = static_cast<std::size_t>(std::distance(buffer.begin(), result.ptr));
  return send_text(socket, std::string_view(buffer.data(), size));
}

[[nodiscard]] auto send_listing(const int socket, const Workspace& workspace) noexcept -> bool {
  const auto size = workspace.terminal.size();
  const auto title = workspace.terminal.title();
  const auto title_value =
      title.has_value() && !title->empty() ? *title : std::string_view{"shell"};
  return send_text(socket, "fiber workspace \"") &&
         send_safe_title(socket, workspace.workspace_name()) &&
         send_text(socket, "\": terminal 0, pid ") &&
         send_number(socket, static_cast<std::uint64_t>(workspace.child)) &&
         send_text(socket, workspace.client >= 0 ? ", attached, " : ", detached, ") &&
         send_number(socket, size.columns) && send_text(socket, "x") &&
         send_number(socket, size.rows) && send_text(socket, ", title \"") &&
         send_safe_title(socket, title_value) && send_text(socket, "\"\n");
}

using Workspaces =
    std::array<std::unique_ptr<Workspace>, static_cast<std::size_t>(limits::workspaces_hard_max)>;

[[nodiscard]] auto find_workspace(Workspaces& workspaces, const std::string_view name) noexcept
    -> Workspace* {
  for (auto& workspace : workspaces) {
    if (workspace != nullptr && workspace->active && workspace->workspace_name() == name) {
      return workspace.get();
    }
  }
  return nullptr;
}

void reclaim_inactive_workspaces(Workspaces& workspaces) noexcept {
  for (auto& workspace : workspaces) {
    if (workspace != nullptr && !workspace->active) {
      workspace.reset();
    }
  }
}

[[nodiscard]] auto empty_workspace_slot(Workspaces& workspaces) noexcept
    -> std::unique_ptr<Workspace>* {
  auto* const slot = std::ranges::find(workspaces, nullptr);
  return slot == workspaces.end() ? nullptr : &*slot;
}

[[nodiscard]] auto send_all_listings(const int connection, const Workspaces& workspaces) noexcept
    -> bool {
  std::size_t listed = 0;
  for (const auto& workspace : workspaces) {
    if (workspace != nullptr && workspace->active) {
      if (!send_listing(connection, *workspace)) {
        return false;
      }
      ++listed;
    }
  }
  return listed > 0 || send_text(connection, "no fiber workspaces\n");
}

[[nodiscard]] auto attach_connection(const int connection, Workspace& workspace) noexcept -> bool {
  std::array<std::byte, 4> dimensions{};
  if (!read_exact(connection, dimensions)) {
    return false;
  }
  if (workspace.client >= 0) {
    return send_all(connection, std::span(&response_busy, 1));
  }

  const auto requested_size = protocol::decode_dimensions(dimensions);
  if (!apply_resize(workspace.pty, workspace.terminal, requested_size.columns,
                    requested_size.rows) ||
      !send_all(connection, std::span(&response_ready, 1)) ||
      !send_frame(connection, workspace.terminal, *workspace.frame, true) ||
      !set_nonblocking(connection)) {
    return false;
  }
  workspace.client = connection;
  workspace.parser.reset();
  workspace.output.reset();
  workspace.frame_pending = false;
  return true;
}

// Control setup remains deliberately simple in the current unversioned protocol.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto handle_connection(const int connection, Workspaces& workspaces) noexcept
    -> bool {
  std::array<std::byte, 1> command{};
  if (!read_exact(connection, command)) {
    return false;
  }
  if (command.front() == command_list) {
    static_cast<void>(send_all_listings(connection, workspaces));
    return false;
  }
  if (command.front() == command_kill_all) {
    for (auto& workspace : workspaces) {
      if (workspace != nullptr) {
        workspace->active = false;
      }
    }
    static_cast<void>(send_text(connection, "all fiber workspaces stopped\n"));
    return false;
  }
  if (command.front() != command_attach && command.front() != command_create &&
      command.front() != command_list_workspace && command.front() != command_kill) {
    return false;
  }

  const auto name = read_workspace_name(connection);
  if (!name.has_value()) {
    return false;
  }
  Workspace* workspace = find_workspace(workspaces, name->view());
  if (command.front() == command_create) {
    if (workspace != nullptr) {
      static_cast<void>(send_all(connection, std::span(&response_ready, 1)));
      return false;
    }
    auto* const slot = empty_workspace_slot(workspaces);
    if (slot == nullptr) {
      static_cast<void>(send_all(connection, std::span(&response_capacity, 1)));
      return false;
    }
    auto created = create_workspace(name->view());
    if (created == nullptr) {
      static_cast<void>(send_all(connection, std::span(&response_failed, 1)));
      return false;
    }
    *slot = std::move(created);
    static_cast<void>(send_all(connection, std::span(&response_ready, 1)));
    return false;
  }
  if (workspace == nullptr) {
    static_cast<void>(send_all(connection, std::span(&response_missing, 1)));
    return false;
  }
  if (command.front() == command_attach) {
    return attach_connection(connection, *workspace) && workspace->client == connection;
  }
  if (command.front() == command_list_workspace) {
    static_cast<void>(send_listing(connection, *workspace));
    return false;
  }

  static_cast<void>(send_text(connection, "fiber workspace \"") &&
                    send_safe_title(connection, workspace->workspace_name()) &&
                    send_text(connection, "\" stopped\n"));
  workspace->active = false;
  return false;
}

[[nodiscard]] auto poll_timeout(const Workspaces& workspaces) noexcept -> int {
  auto timeout = -1;
  const auto now = std::chrono::steady_clock::now();
  for (const auto& workspace : workspaces) {
    if (workspace == nullptr || !workspace->active || !workspace->frame_pending ||
        workspace->client < 0 || workspace->output.busy()) {
      continue;
    }
    if (now >= workspace->frame_deadline) {
      return 0;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(workspace->frame_deadline - now);
    const auto candidate = static_cast<int>(std::max(remaining.count(), std::int64_t{1}));
    timeout = timeout < 0 ? candidate : std::min(timeout, candidate);
  }
  return timeout;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void process_workspace_events(Workspace& workspace, const pollfd& pty_events,
                              const pollfd& client_events) noexcept {
  constexpr auto frame_delay = std::chrono::milliseconds(2);
  if ((pty_events.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
    const auto drained = drain_pty(workspace.pty, workspace.terminal);
    workspace.active = drained.alive;
    if (drained.changed && workspace.client >= 0 && !workspace.frame_pending) {
      workspace.frame_pending = true;
      workspace.frame_deadline = drained.alive ? std::chrono::steady_clock::now() + frame_delay
                                               : std::chrono::steady_clock::now();
    }
  }

  if (workspace.client >= 0 && (client_events.revents & POLLOUT) != 0 &&
      !flush_frame(workspace.client, *workspace.frame, workspace.output)) {
    workspace.detach_client();
  }
  if (workspace.client >= 0 && (client_events.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
    const auto parse_result =
        workspace.parser.receive(workspace.client, workspace.pty, workspace.terminal);
    if (parse_result != ParseResult::keep) {
      workspace.detach_client();
    }
  }
  if (workspace.frame_pending && workspace.client >= 0 && !workspace.output.busy() &&
      std::chrono::steady_clock::now() >= workspace.frame_deadline) {
    if (!queue_frame(workspace.client, workspace.terminal, *workspace.frame, workspace.output)) {
      workspace.detach_client();
    }
    workspace.frame_pending = false;
  }
}

// The branches are the explicit bounded stages of the current single-owner reactor.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto run_server_impl(const int listener, const EndpointRelease release_endpoint,
                                   void* const release_context) noexcept -> int {
  EndpointReleaseGuard endpoint_release(release_endpoint, release_context);
  Workspaces workspaces;
  constexpr auto descriptor_count_max =
      std::size_t{1} + (static_cast<std::size_t>(limits::workspaces_hard_max) * 2U);
  std::array<pollfd, descriptor_count_max> descriptors{};
  std::array<Workspace*, static_cast<std::size_t>(limits::workspaces_hard_max)> ready_workspaces{};

  while (true) {
    std::size_t descriptor_count = 1;
    std::size_t ready_workspace_count = 0;
    descriptors.front() = {.fd = listener, .events = POLLIN, .revents = 0};
    for (const auto& workspace : workspaces) {
      if (workspace == nullptr) {
        continue;
      }
      const auto client_events =
          static_cast<short>(POLLIN | (workspace->output.busy() ? static_cast<short>(POLLOUT) : 0));
      std::span(ready_workspaces).subspan(ready_workspace_count, 1).front() = workspace.get();
      ++ready_workspace_count;
      std::span(descriptors).subspan(descriptor_count, 1).front() = {
          .fd = workspace->pty, .events = POLLIN, .revents = 0};
      ++descriptor_count;
      std::span(descriptors).subspan(descriptor_count, 1).front() = {
          .fd = workspace->client, .events = client_events, .revents = 0};
      ++descriptor_count;
    }

    const auto poll_result =
        ::poll(descriptors.data(), static_cast<nfds_t>(descriptor_count), poll_timeout(workspaces));
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return 1;
    }

    for (std::size_t index = 0; index < ready_workspace_count; ++index) {
      auto* const workspace = std::span(ready_workspaces).subspan(index, 1).front();
      const auto& pty_events = std::span(descriptors).subspan(1U + (index * 2U), 1).front();
      const auto& client_events = std::span(descriptors).subspan(2U + (index * 2U), 1).front();
      process_workspace_events(*workspace, pty_events, client_events);
    }

    reclaim_inactive_workspaces(workspaces);

    if ((descriptors.front().revents & POLLIN) != 0) {
      int connection = ::accept(listener, nullptr, nullptr);
      if (connection >= 0 && !handle_connection(connection, workspaces)) {
        close_descriptor(connection);
      }
    }

    reclaim_inactive_workspaces(workspaces);
  }
}

} // namespace

[[nodiscard]] auto run_server(const int listener, const EndpointRelease release_endpoint,
                              void* const release_context) noexcept -> int {
  return run_server_impl(listener, release_endpoint, release_context);
}

} // namespace fiber::core
