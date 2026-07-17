#include "render/single_pane.hpp"

#include "fiber/assert.hpp"
#include "platform/io.hpp"

#include <cerrno>
#include <span>

#include <sys/socket.h>

namespace fiber::render {

[[nodiscard]] auto send_frame(const int client, vt::Terminal& terminal, FrameBuffer& frame,
                              const bool force_full) noexcept -> bool {
  const auto rendered = terminal.render_ansi(frame, force_full);
  return rendered.has_value() &&
         platform::send_all(client, std::span(frame).first(rendered->bytes));
}

[[nodiscard]] auto flush_frame(const int client, const FrameBuffer& frame,
                               ClientOutputState& output) noexcept -> bool {
  while (output.busy()) {
    const auto remaining = std::span(frame).first(output.size).subspan(output.offset);
    const auto sent = ::send(client, remaining.data(), remaining.size(), MSG_NOSIGNAL);
    if (sent > 0) {
      output.offset += static_cast<std::size_t>(sent);
      continue;
    }
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return true;
    }
    return false;
  }
  output.reset();
  return true;
}

[[nodiscard]] auto queue_frame(const int client, vt::Terminal& terminal, FrameBuffer& frame,
                               ClientOutputState& output) noexcept -> bool {
  FIBER_ASSERT(!output.busy());
  const auto rendered = terminal.render_ansi(frame, false);
  if (!rendered.has_value()) {
    return false;
  }
  output.size = rendered->bytes;
  output.offset = 0;
  return flush_frame(client, frame, output);
}

} // namespace fiber::render
