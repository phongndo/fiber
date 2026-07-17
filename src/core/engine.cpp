#include "core/engine.hpp"

#include "core/input.hpp"
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
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fiber::core {
namespace {

constexpr auto command_attach = protocol::wire_byte(protocol::ControlCommand::attach);
constexpr auto command_list = protocol::wire_byte(protocol::ControlCommand::list);
constexpr auto command_kill = protocol::wire_byte(protocol::ControlCommand::kill);
constexpr auto response_attached = protocol::wire_byte(protocol::AttachResponse::attached);
constexpr auto response_busy = protocol::wire_byte(protocol::AttachResponse::busy);
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

[[nodiscard]] auto send_safe_title(const int socket, const std::string_view title) noexcept
    -> bool {
  std::array<char, 256> sanitized{};
  std::size_t used = 0;
  const std::span<const char> title_characters(title.data(), title.size());
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

[[nodiscard]] auto send_listing(const int socket, const std::string_view session, const pid_t child,
                                const bool attached, const vt::Terminal& terminal) noexcept
    -> bool {
  const auto size = terminal.size();
  const auto title = terminal.title();
  const auto title_value =
      title.has_value() && !title->empty() ? *title : std::string_view{"shell"};
  return send_text(socket, "fiber session \"") && send_safe_title(socket, session) &&
         send_text(socket, "\": pane 0, pid ") &&
         send_number(socket, static_cast<std::uint64_t>(child)) &&
         send_text(socket, attached ? ", attached, " : ", detached, ") &&
         send_number(socket, size.columns) && send_text(socket, "x") &&
         send_number(socket, size.rows) && send_text(socket, ", title \"") &&
         send_safe_title(socket, title_value) && send_text(socket, "\"\n");
}

[[nodiscard]] auto handle_connection(const int connection, int& client,
                                     const std::string_view session, const int pty,
                                     const pid_t child, vt::Terminal& terminal,
                                     ClientPacketParser& parser, FrameBuffer& frame,
                                     ClientOutputState& output, bool& running) noexcept -> bool {
  std::array<std::byte, 1> command{};
  if (!read_exact(connection, command)) {
    return true;
  }
  if (command.front() == command_list) {
    return send_listing(connection, session, child, client >= 0, terminal);
  }
  if (command.front() == command_kill) {
    running = false;
    return send_text(connection, "fiber session \"") && send_safe_title(connection, session) &&
           send_text(connection, "\" stopped\n");
  }
  if (command.front() != command_attach) {
    return false;
  }

  std::array<std::byte, 4> dimensions{};
  if (!read_exact(connection, dimensions)) {
    return false;
  }
  if (client >= 0) {
    return send_all(connection, std::span(&response_busy, 1));
  }

  const auto requested_size = protocol::decode_dimensions(dimensions);
  if (!apply_resize(pty, terminal, requested_size.columns, requested_size.rows) ||
      !send_all(connection, std::span(&response_attached, 1)) ||
      !send_frame(connection, terminal, frame, true) || !set_nonblocking(connection)) {
    return false;
  }

  client = connection;
  parser.reset();
  output.reset();
  return true;
}

// The branches are the explicit bounded stages of the current single-owner reactor.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto run_session_impl(const int listener, const std::string_view session,
                                    const EndpointRelease release_endpoint,
                                    void* const release_context) noexcept -> int {
  EndpointReleaseGuard endpoint_release(release_endpoint, release_context);
  vt::TerminalOptions options;
  options.size = {.columns = 80, .rows = 24};
  auto terminal_result = vt::Terminal::create(options);
  if (!terminal_result.has_value()) {
    return 1;
  }
  auto terminal = std::move(*terminal_result);

  int pty = -1;
  const auto child = platform::spawn_login_shell(pty);
  if (child <= 0) {
    return 1;
  }
  if (!set_nonblocking(pty)) {
    static_cast<void>(::kill(child, SIGHUP));
    close_descriptor(pty);
    endpoint_release.release();
    static_cast<void>(::waitpid(child, nullptr, 0));
    return 1;
  }

  auto frame = std::unique_ptr<FrameBuffer>(new (std::nothrow) FrameBuffer{});
  if (frame == nullptr) {
    static_cast<void>(::kill(child, SIGHUP));
    close_descriptor(pty);
    endpoint_release.release();
    static_cast<void>(::waitpid(child, nullptr, 0));
    return 1;
  }

  int client = -1;
  bool running = true;
  bool frame_pending = false;
  auto frame_deadline = std::chrono::steady_clock::time_point{};
  constexpr auto frame_delay = std::chrono::milliseconds(2);
  ClientPacketParser parser;
  ClientOutputState client_output;
  while (running) {
    int poll_timeout_ms = -1;
    if (frame_pending && client >= 0 && !client_output.busy()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= frame_deadline) {
        poll_timeout_ms = 0;
      } else {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(frame_deadline - now);
        poll_timeout_ms = static_cast<int>(std::max(remaining.count(), std::int64_t{1}));
      }
    }

    const auto client_poll_events =
        static_cast<short>(POLLIN | (client_output.busy() ? static_cast<short>(POLLOUT) : 0));
    std::array<pollfd, 3> descriptors{{
        {.fd = listener, .events = POLLIN, .revents = 0},
        {.fd = pty, .events = POLLIN, .revents = 0},
        {.fd = client, .events = client_poll_events, .revents = 0},
    }};
    const auto poll_result = ::poll(descriptors.data(), descriptors.size(), poll_timeout_ms);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    const auto& listener_events = descriptors.front();
    const auto& pty_events = std::span(descriptors).subspan<1, 1>().front();
    const auto& client_events = descriptors.back();
    if ((listener_events.revents & POLLIN) != 0) {
      int connection = ::accept(listener, nullptr, nullptr);
      if (connection >= 0) {
        const auto retained = handle_connection(connection, client, session, pty, child, terminal,
                                                parser, *frame, client_output, running) &&
                              connection == client;
        if (!retained) {
          close_descriptor(connection);
        } else {
          frame_pending = false;
          client_output.reset();
        }
      }
    }

    if ((pty_events.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      const auto drained = drain_pty(pty, terminal);
      running = drained.alive;
      if (drained.changed && client >= 0 && !frame_pending) {
        frame_pending = true;
        frame_deadline = drained.alive ? std::chrono::steady_clock::now() + frame_delay
                                       : std::chrono::steady_clock::now();
      }
    }

    if (client >= 0 && (client_events.revents & POLLOUT) != 0 &&
        !flush_frame(client, *frame, client_output)) {
      close_descriptor(client);
      parser.reset();
      client_output.reset();
      frame_pending = false;
    }

    if (client >= 0 && (client_events.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      const auto parse_result = parser.receive(client, pty, terminal);
      if (parse_result != ParseResult::keep) {
        close_descriptor(client);
        parser.reset();
        client_output.reset();
        frame_pending = false;
      }
    }

    if (frame_pending && client >= 0 && !client_output.busy() &&
        std::chrono::steady_clock::now() >= frame_deadline) {
      if (!queue_frame(client, terminal, *frame, client_output)) {
        close_descriptor(client);
        parser.reset();
        client_output.reset();
      }
      frame_pending = false;
    }
  }

  close_descriptor(client);
  static_cast<void>(::kill(child, SIGHUP));
  close_descriptor(pty);
  endpoint_release.release();
  static_cast<void>(::waitpid(child, nullptr, 0));
  return 0;
}

} // namespace

[[nodiscard]] auto run_session(const int listener, const std::string_view session,
                               const EndpointRelease release_endpoint,
                               void* const release_context) noexcept -> int {
  return run_session_impl(listener, session, release_endpoint, release_context);
}

} // namespace fiber::core
