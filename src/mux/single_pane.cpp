#include "fiber/mux/single_pane.hpp"

#include "fiber/assert.hpp"
#include "fiber/vt/terminal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#include <util.h>
#elifdef __linux__
#include <pty.h>
#else
#error "fiber single-pane mux requires forkpty"
#endif

namespace fiber::mux {
namespace {

constexpr std::byte command_attach{'A'};
constexpr std::byte command_list{'L'};
constexpr std::byte command_kill{'K'};
constexpr std::byte response_attached{'Y'};
constexpr std::byte response_busy{'B'};
constexpr std::byte packet_input{'I'};
constexpr std::byte packet_resize{'R'};
constexpr std::byte packet_detach{'D'};
constexpr std::size_t input_bytes_max = std::size_t{4} * 1'024U;
constexpr std::size_t parser_bytes_max = std::size_t{16} * 1'024U;
constexpr std::size_t snapshot_bytes_max = std::size_t{4} * 1'024U * 1'024U;
constexpr std::uint16_t columns_max = 500;
constexpr std::uint16_t rows_max = 200;
using FrameBuffer = std::array<std::byte, snapshot_bytes_max>;

volatile sig_atomic_t resize_pending = 0;

void on_window_changed([[maybe_unused]] const int signal_number) noexcept { resize_pending = 1; }

[[nodiscard]] auto write_all(const int descriptor, const std::span<const std::byte> bytes) noexcept
    -> bool {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto result = ::write(descriptor, bytes.subspan(offset).data(), bytes.size() - offset);
    if (result > 0) {
      offset += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] auto send_all(const int socket, const std::span<const std::byte> bytes) noexcept
    -> bool {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto result =
        ::send(socket, bytes.subspan(offset).data(), bytes.size() - offset, MSG_NOSIGNAL);
    if (result > 0) {
      offset += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] auto send_text(const int socket, const std::string_view text) noexcept -> bool {
  return send_all(socket, std::as_bytes(std::span(text.data(), text.size())));
}

[[nodiscard]] auto write_text(const int descriptor, const std::string_view text) noexcept -> bool {
  return write_all(descriptor, std::as_bytes(std::span(text.data(), text.size())));
}

[[nodiscard]] auto read_exact(const int socket, const std::span<std::byte> output) noexcept
    -> bool {
  std::size_t offset = 0;
  while (offset < output.size()) {
    const auto result = ::recv(socket, output.subspan(offset).data(), output.size() - offset, 0);
    if (result > 0) {
      offset += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

void close_descriptor(int& descriptor) noexcept {
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
    descriptor = -1;
  }
}

[[nodiscard]] auto set_nonblocking(const int descriptor) noexcept -> bool {
  // fcntl is variadic because its third argument depends on the command.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const auto flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  return ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

[[nodiscard]] constexpr auto valid_session_name(const std::string_view session) noexcept -> bool {
  if (session.empty() || session.size() > 32) {
    return false;
  }
  return std::ranges::all_of(session, [](const char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-';
  });
}

[[nodiscard]] auto socket_filename(const std::string_view session) -> std::string {
  auto filename = "fiber-" + std::to_string(::getuid());
  if (session != default_session) {
    filename += "-";
    filename += session;
  }
  filename += ".sock";
  return filename;
}

[[nodiscard]] auto socket_path(const std::string_view session) -> std::string {
  return "/tmp/" + socket_filename(session);
}

[[nodiscard]] auto socket_address(const std::string& path) noexcept
    -> std::expected<sockaddr_un, int> {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    return std::unexpected(ENAMETOOLONG);
  }
  const auto path_storage = std::span(address.sun_path);
  std::memcpy(path_storage.data(), path.c_str(), path.size() + 1U);
  return address;
}

[[nodiscard]] auto open_connection(const std::string& path) noexcept -> int {
  int connection = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (connection < 0) {
    return -1;
  }

  const auto address = socket_address(path);
  if (!address.has_value()) {
    close_descriptor(connection);
    return -1;
  }

  // The C socket ABI erases the concrete sockaddr type.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* generic_address = reinterpret_cast<const sockaddr*>(&*address);
  if (::connect(connection, generic_address, sizeof(*address)) != 0) {
    close_descriptor(connection);
    return -1;
  }
  return connection;
}

[[nodiscard]] auto discover_sessions() -> std::vector<std::string> {
  std::vector<std::string> sessions;
  DIR* const directory = ::opendir("/tmp");
  if (directory == nullptr) {
    return sessions;
  }

  const auto base = "fiber-" + std::to_string(::getuid());
  const auto default_filename = base + ".sock";
  const auto named_prefix = base + "-";
  constexpr std::string_view extension = ".sock";
  while (const auto* entry = ::readdir(directory)) {
    // dirent exposes a null-terminated C array owned by the directory stream.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const std::string_view filename(entry->d_name);
    std::string session;
    if (filename == default_filename) {
      session = default_session;
    } else if (filename.starts_with(named_prefix) && filename.ends_with(extension)) {
      const auto name_begin = base.size() + 1U;
      const auto name_size = filename.size() - name_begin - extension.size();
      session = filename.substr(name_begin, name_size);
    }
    if (session.empty() || !valid_session_name(session)) {
      continue;
    }

    const auto path = "/tmp/" + std::string(filename);
    struct stat metadata{};
    if (::lstat(path.c_str(), &metadata) == 0 && S_ISSOCK(metadata.st_mode) &&
        metadata.st_uid == ::getuid()) {
      sessions.push_back(std::move(session));
    }
    if (sessions.size() >= limits::sessions_hard_max) {
      break;
    }
  }
  static_cast<void>(::closedir(directory));
  std::ranges::sort(sessions);
  return sessions;
}

[[nodiscard]] auto acquire_session_lock(const std::string& path, int& lock_descriptor) noexcept
    -> bool {
  std::array<char, 256> lock_path{};
  constexpr std::string_view extension = ".lock";
  if (path.size() + extension.size() + 1U > lock_path.size()) {
    return false;
  }
  auto destination = std::span(lock_path);
  std::memcpy(destination.data(), path.data(), path.size());
  std::memcpy(destination.subspan(path.size()).data(), extension.data(), extension.size());

  // open is variadic because O_CREAT requires a file mode.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  lock_descriptor = ::open(lock_path.data(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock_descriptor < 0 || ::flock(lock_descriptor, LOCK_EX | LOCK_NB) != 0) {
    close_descriptor(lock_descriptor);
    return false;
  }
  return true;
}

[[nodiscard]] auto remove_stale_socket(const std::string& path) noexcept -> bool {
  struct stat existing{};
  if (::lstat(path.c_str(), &existing) != 0) {
    return errno == ENOENT;
  }
  if (!S_ISSOCK(existing.st_mode) || existing.st_uid != ::getuid()) {
    return false;
  }

  int existing_server = open_connection(path);
  if (existing_server >= 0) {
    close_descriptor(existing_server);
    return false;
  }
  return ::unlink(path.c_str()) == 0;
}

[[nodiscard]] auto create_listener(const std::string& path, int& lock_descriptor) noexcept -> int {
  if (!acquire_session_lock(path, lock_descriptor) || !remove_stale_socket(path)) {
    return -1;
  }

  int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener < 0) {
    return -1;
  }

  const auto address = socket_address(path);
  if (!address.has_value()) {
    close_descriptor(listener);
    return -1;
  }

  // The C socket ABI erases the concrete sockaddr type.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* generic_address = reinterpret_cast<const sockaddr*>(&*address);
  if (::bind(listener, generic_address, sizeof(*address)) != 0) {
    close_descriptor(listener);
    return -1;
  }
  if (::chmod(path.c_str(), 0600) != 0 || ::listen(listener, 8) != 0) {
    close_descriptor(listener);
    static_cast<void>(::unlink(path.c_str()));
    return -1;
  }
  return listener;
}

[[nodiscard]] constexpr auto decode_u16(const std::byte high, const std::byte low) noexcept
    -> std::uint16_t {
  const auto high_value = std::to_integer<std::uint16_t>(high);
  const auto low_value = std::to_integer<std::uint16_t>(low);
  return static_cast<std::uint16_t>((high_value << 8U) | low_value);
}

void encode_u16(const std::uint16_t value, const std::span<std::byte, 2> output) noexcept {
  output.front() = static_cast<std::byte>(value >> 8U);
  output.back() = static_cast<std::byte>(value & 0xFFU);
}

[[nodiscard]] auto terminal_size() noexcept -> vt::TerminalSize {
  winsize native_size{};
  // ioctl is variadic because its third argument depends on the request.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &native_size) != 0 || native_size.ws_col == 0 ||
      native_size.ws_row == 0) {
    return {};
  }
  return {
      .columns = std::min(native_size.ws_col, columns_max),
      .rows = std::min(native_size.ws_row, rows_max),
      .cell_width_px = 0,
      .cell_height_px = 0,
  };
}

[[nodiscard]] auto send_resize(const int connection, const vt::TerminalSize& size) noexcept
    -> bool {
  std::array<std::byte, 5> packet{packet_resize};
  encode_u16(size.columns, std::span(packet).subspan<1, 2>());
  encode_u16(size.rows, std::span(packet).subspan<3, 2>());
  return send_all(connection, packet);
}

[[nodiscard]] auto apply_resize(const int pty, vt::Terminal& terminal,
                                const std::uint16_t requested_columns,
                                const std::uint16_t requested_rows) noexcept -> bool {
  const auto columns = std::clamp(requested_columns, std::uint16_t{1}, columns_max);
  const auto rows = std::clamp(requested_rows, std::uint16_t{1}, rows_max);
  winsize native_size{
      .ws_row = rows,
      .ws_col = columns,
      .ws_xpixel = 0,
      .ws_ypixel = 0,
  };
  // ioctl is variadic because its third argument depends on the request.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  if (::ioctl(pty, TIOCSWINSZ, &native_size) != 0) {
    return false;
  }
  return terminal.resize({.columns = columns, .rows = rows}).has_value();
}

class RawTerminal final {
public:
  [[nodiscard]] auto enter() noexcept -> bool {
    if (::tcgetattr(STDIN_FILENO, &original_) != 0) {
      return false;
    }

    auto raw = original_;
    raw.c_iflag &= static_cast<tcflag_t>(~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
      return false;
    }
    active_ = true;
    return true;
  }

  RawTerminal() = default;
  RawTerminal(const RawTerminal&) = delete;
  auto operator=(const RawTerminal&) -> RawTerminal& = delete;
  RawTerminal(RawTerminal&&) = delete;
  auto operator=(RawTerminal&&) -> RawTerminal& = delete;

  ~RawTerminal() {
    if (active_) {
      static_cast<void>(::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_));
    }
  }

private:
  termios original_{};
  bool active_{false};
};

struct PrefixResult final {
  std::size_t bytes{0};
  bool detach{false};
};

class PrefixParser final {
public:
  [[nodiscard]] auto parse(const std::span<const std::byte> input,
                           const std::span<std::byte> output) noexcept -> PrefixResult {
    PrefixResult result{};
    for (const auto byte : input) {
      if (prefix_) {
        prefix_ = false;
        if (byte == std::byte{'d'}) {
          result.detach = true;
          break;
        }
        if (byte == std::byte{0x02}) {
          output.subspan(result.bytes, 1).front() = byte;
          ++result.bytes;
          continue;
        }
        output.subspan(result.bytes, 1).front() = std::byte{0x02};
        ++result.bytes;
      } else if (byte == std::byte{0x02}) {
        prefix_ = true;
        continue;
      }

      output.subspan(result.bytes, 1).front() = byte;
      ++result.bytes;
    }
    FIBER_ASSERT(result.bytes <= output.size());
    return result;
  }

private:
  bool prefix_{false};
};

[[nodiscard]] auto send_input(const int connection, const std::span<const std::byte> input) noexcept
    -> bool {
  FIBER_ASSERT(input.size() <= input_bytes_max * 2U);
  std::array<std::byte, 3> header{packet_input};
  encode_u16(static_cast<std::uint16_t>(input.size()), std::span(header).subspan<1, 2>());
  return send_all(connection, header) && send_all(connection, input);
}

enum class ParseResult : std::uint8_t {
  keep,
  detach,
  error,
};

[[nodiscard]] constexpr auto control_key(const std::byte byte) noexcept -> vt::Key {
  const auto value = std::to_integer<std::uint8_t>(byte);
  FIBER_ASSERT(value >= 1 && value <= 26);
  return static_cast<vt::Key>(static_cast<std::uint8_t>(vt::Key::a) + value - 1U);
}

[[nodiscard]] constexpr auto arrow_key(const std::byte final) noexcept -> vt::Key {
  switch (std::to_integer<char>(final)) {
  case 'A':
    return vt::Key::arrow_up;
  case 'B':
    return vt::Key::arrow_down;
  case 'C':
    return vt::Key::arrow_right;
  case 'D':
    return vt::Key::arrow_left;
  case 'F':
    return vt::Key::end;
  case 'H':
    return vt::Key::home;
  default:
    return vt::Key::unidentified;
  }
}

// This is the bounded translation state machine for legacy terminal input.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto write_normalized_input(const int pty, vt::Terminal& terminal,
                                          const std::span<const std::byte> input) noexcept -> bool {
  std::array<std::byte, input_bytes_max * 4U> encoded{};
  std::size_t input_offset = 0;
  std::size_t output_size = 0;
  while (input_offset < input.size()) {
    vt::KeyEvent event{};
    std::size_t consumed = 0;
    const auto byte = input.subspan(input_offset, 1).front();
    const auto value = std::to_integer<std::uint8_t>(byte);
    if (value >= 1 && value <= 26) {
      event.key = control_key(byte);
      event.modifiers = vt::key_modifier_control;
      event.unshifted_codepoint = static_cast<std::uint32_t>('a' + value - 1U);
      consumed = 1;
    } else if (input.size() - input_offset >= 3 && byte == std::byte{0x1B} &&
               (input.subspan(input_offset + 1, 1).front() == std::byte{'['} ||
                input.subspan(input_offset + 1, 1).front() == std::byte{'O'})) {
      event.key = arrow_key(input.subspan(input_offset + 2, 1).front());
      consumed = event.key == vt::Key::unidentified ? 0 : 3;
    }

    if (consumed == 0) {
      if (output_size == encoded.size() && !write_all(pty, std::span(encoded).first(output_size))) {
        return false;
      }
      if (output_size == encoded.size()) {
        output_size = 0;
      }
      std::span(encoded).subspan(output_size, 1).front() = byte;
      ++output_size;
      ++input_offset;
      continue;
    }

    constexpr std::size_t key_bytes_max = 128;
    if (encoded.size() - output_size < key_bytes_max) {
      if (!write_all(pty, std::span(encoded).first(output_size))) {
        return false;
      }
      output_size = 0;
    }
    auto available = std::span(encoded).subspan(output_size);
    auto key_bytes = terminal.encode_key(event, available);
    if (!key_bytes.has_value()) {
      return false;
    }
    output_size += *key_bytes;
    input_offset += consumed;
  }
  return output_size == 0 || write_all(pty, std::span(encoded).first(output_size));
}

class ClientPacketParser final {
public:
  [[nodiscard]] auto receive(const int client, const int pty, vt::Terminal& terminal) noexcept
      -> ParseResult {
    FIBER_ASSERT(used_ <= storage_.size());
    const auto available = std::span(storage_).subspan(used_);
    const auto bytes_read = ::recv(client, available.data(), available.size(), 0);
    if (bytes_read == 0) {
      return ParseResult::detach;
    }
    if (bytes_read < 0) {
      return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ? ParseResult::keep
                                                                       : ParseResult::detach;
    }
    used_ += static_cast<std::size_t>(bytes_read);
    return parse(pty, terminal);
  }

  void reset() noexcept { used_ = 0; }

private:
  // The explicit branches are the complete bounded wire-protocol state machine.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] auto parse(const int pty, vt::Terminal& terminal) noexcept -> ParseResult {
    while (used_ > 0) {
      const auto buffered = std::span(storage_).first(used_);
      const auto type = buffered.front();
      if (type == packet_detach) {
        consume(1);
        return ParseResult::detach;
      }
      if (type == packet_resize) {
        if (used_ < 5) {
          return ParseResult::keep;
        }
        const auto dimensions = buffered.subspan(1, 4);
        const auto columns = decode_u16(dimensions.front(), dimensions.subspan(1).front());
        const auto rows = decode_u16(dimensions.subspan(2).front(), dimensions.subspan(3).front());
        if (!apply_resize(pty, terminal, columns, rows)) {
          return ParseResult::error;
        }
        consume(5);
        continue;
      }
      if (type != packet_input || used_ < 3) {
        return type == packet_input ? ParseResult::keep : ParseResult::error;
      }

      const auto length = decode_u16(buffered.subspan(1).front(), buffered.subspan(2).front());
      if (length > input_bytes_max * 2U) {
        return ParseResult::error;
      }
      const auto packet_size = std::size_t{3} + length;
      if (used_ < packet_size) {
        return ParseResult::keep;
      }
      if (!write_normalized_input(pty, terminal, std::span(storage_).subspan(3, length))) {
        return ParseResult::error;
      }
      consume(packet_size);
    }
    return ParseResult::keep;
  }

  void consume(const std::size_t count) noexcept {
    FIBER_ASSERT(count <= used_);
    std::memmove(storage_.data(), std::span(storage_).subspan(count).data(), used_ - count);
    used_ -= count;
  }

  std::array<std::byte, parser_bytes_max> storage_{};
  std::size_t used_{0};
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

[[nodiscard]] auto send_frame(const int client, vt::Terminal& terminal, FrameBuffer& frame,
                              const bool force_full) noexcept -> bool {
  const auto rendered = terminal.render_ansi(frame, force_full);
  return rendered.has_value() && send_all(client, std::span(frame).first(rendered->bytes));
}

struct ClientOutputState final {
  std::size_t size{0};
  std::size_t offset{0};

  [[nodiscard]] auto busy() const noexcept -> bool { return offset < size; }
  void reset() noexcept {
    size = 0;
    offset = 0;
  }
};

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

[[nodiscard]] auto spawn_shell(int& pty_descriptor) noexcept -> pid_t {
  winsize initial_size{.ws_row = 24, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0};
  const auto child = ::forkpty(&pty_descriptor, nullptr, nullptr, &initial_size);
  if (child != 0) {
    return child;
  }

  std::array fallback_shell{'/', 'b', 'i', 'n', '/', 's', 'h', '\0'};
  std::array<char, std::size_t{16} * 1'024U> account_buffer{};
  struct passwd account{};
  struct passwd* account_result = nullptr;
  char* shell = fallback_shell.data();
  if (::getpwuid_r(::getuid(), &account, account_buffer.data(), account_buffer.size(),
                   &account_result) == 0 &&
      account_result != nullptr && account.pw_shell != nullptr) {
    const std::string_view configured_shell(account.pw_shell);
    if (!configured_shell.empty() && configured_shell.front() == '/' &&
        ::access(account.pw_shell, X_OK) == 0) {
      shell = account.pw_shell;
    }
  }

  if (::setenv("TERM", "xterm-256color", 1) != 0 || ::setenv("COLORTERM", "truecolor", 1) != 0 ||
      ::setenv("TERM_PROGRAM", "fiber", 1) != 0) {
    ::_exit(127);
  }

  std::array login_argument{'-', 'l', '\0'};
  const std::array arguments{shell, login_argument.data(), static_cast<char*>(nullptr)};
  ::execv(shell, arguments.data());
  ::_exit(127);
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

  const auto dimension_view = std::span(dimensions);
  const auto columns = decode_u16(dimension_view.front(), dimension_view.subspan(1).front());
  const auto rows =
      decode_u16(dimension_view.subspan(2).front(), dimension_view.subspan(3).front());
  if (!apply_resize(pty, terminal, columns, rows) ||
      !send_all(connection, std::span(&response_attached, 1)) ||
      !send_frame(connection, terminal, frame, true) || !set_nonblocking(connection)) {
    return false;
  }

  client = connection;
  parser.reset();
  output.reset();
  return true;
}

// This is the single-owner reactor; branches correspond directly to three descriptors.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto run_server(const std::string& path, const std::string_view session) noexcept
    -> int {
  static_cast<void>(::signal(SIGPIPE, SIG_IGN));
  const auto previous_mask = ::umask(0077);
  int session_lock = -1;
  int listener = create_listener(path, session_lock);
  static_cast<void>(::umask(previous_mask));
  if (listener < 0) {
    close_descriptor(session_lock);
    return 1;
  }

  vt::TerminalOptions options;
  options.size = {.columns = 80, .rows = 24};
  auto terminal_result = vt::Terminal::create(options);
  if (!terminal_result.has_value()) {
    close_descriptor(listener);
    static_cast<void>(::unlink(path.c_str()));
    close_descriptor(session_lock);
    return 1;
  }
  auto terminal = std::move(*terminal_result);

  int pty = -1;
  const auto child = spawn_shell(pty);
  if (child <= 0) {
    close_descriptor(listener);
    static_cast<void>(::unlink(path.c_str()));
    close_descriptor(session_lock);
    return 1;
  }
  if (!set_nonblocking(pty)) {
    static_cast<void>(::kill(child, SIGHUP));
    close_descriptor(pty);
    close_descriptor(listener);
    static_cast<void>(::unlink(path.c_str()));
    close_descriptor(session_lock);
    static_cast<void>(::waitpid(child, nullptr, 0));
    return 1;
  }

  auto frame = std::unique_ptr<FrameBuffer>(new (std::nothrow) FrameBuffer{});
  if (frame == nullptr) {
    static_cast<void>(::kill(child, SIGHUP));
    close_descriptor(pty);
    close_descriptor(listener);
    static_cast<void>(::unlink(path.c_str()));
    close_descriptor(session_lock);
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
  close_descriptor(listener);
  static_cast<void>(::unlink(path.c_str()));
  close_descriptor(session_lock);
  static_cast<void>(::waitpid(child, nullptr, 0));
  return 0;
}

[[nodiscard]] auto server_available(const std::string& path) noexcept -> bool {
  int connection = open_connection(path);
  if (connection < 0) {
    return false;
  }
  const std::array command{command_list};
  const auto sent = send_all(connection, command);
  std::array<std::byte, 1> response{};
  const auto received = sent && read_exact(connection, response);
  close_descriptor(connection);
  return received;
}

void redirect_standard_descriptors() noexcept {
  // open is variadic when file creation mode is present.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  int null_descriptor = ::open("/dev/null", O_RDWR | O_NOCTTY);
  if (null_descriptor < 0) {
    return;
  }
  static_cast<void>(::dup2(null_descriptor, STDIN_FILENO));
  static_cast<void>(::dup2(null_descriptor, STDOUT_FILENO));
  static_cast<void>(::dup2(null_descriptor, STDERR_FILENO));
  if (null_descriptor > STDERR_FILENO) {
    close_descriptor(null_descriptor);
  }
}

[[nodiscard]] auto launch_server(const std::string& path, const std::string_view session) noexcept
    -> bool {
  const auto first_child = ::fork();
  if (first_child < 0) {
    return false;
  }
  if (first_child == 0) {
    if (::setsid() < 0) {
      ::_exit(1);
    }
    const auto daemon_child = ::fork();
    if (daemon_child < 0) {
      ::_exit(1);
    }
    if (daemon_child > 0) {
      ::_exit(0);
    }
    redirect_standard_descriptors();
    ::_exit(run_server(path, session));
  }

  static_cast<void>(::waitpid(first_child, nullptr, 0));
  for (std::size_t attempt = 0; attempt < 200; ++attempt) {
    if (server_available(path)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

[[nodiscard]] auto ensure_server(const std::string& path, const std::string_view session) noexcept
    -> bool {
  return server_available(path) || launch_server(path, session);
}

[[nodiscard]] auto send_attach_handshake(const int connection,
                                         const vt::TerminalSize& size) noexcept -> bool {
  std::array<std::byte, 5> handshake{command_attach};
  encode_u16(size.columns, std::span(handshake).subspan<1, 2>());
  encode_u16(size.rows, std::span(handshake).subspan<3, 2>());
  return send_all(connection, handshake);
}

// This is the client reactor; branches correspond directly to terminal and socket readiness.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto attach_client(const std::string& path) noexcept -> int {
  if (::isatty(STDIN_FILENO) == 0 || ::isatty(STDOUT_FILENO) == 0) {
    static_cast<void>(write_text(STDERR_FILENO, "fiber attach requires a terminal\n"));
    return 1;
  }

  int connection = open_connection(path);
  if (connection < 0) {
    static_cast<void>(write_text(STDERR_FILENO, "no fiber pane; run `fiber new`\n"));
    return 1;
  }
  if (!send_attach_handshake(connection, terminal_size())) {
    close_descriptor(connection);
    return 1;
  }

  std::array<std::byte, 1> response{};
  if (!read_exact(connection, response) || response.front() != response_attached) {
    static_cast<void>(write_text(STDERR_FILENO, response.front() == response_busy
                                                    ? "fiber pane is already attached\n"
                                                    : "fiber attach failed\n"));
    close_descriptor(connection);
    return 1;
  }

  RawTerminal raw_terminal;
  if (!raw_terminal.enter()) {
    close_descriptor(connection);
    return 1;
  }

  struct sigaction action{};
  struct sigaction previous_action{};
  action.sa_handler = &on_window_changed;
  static_cast<void>(sigemptyset(&action.sa_mask));
  action.sa_flags = 0;
  static_cast<void>(::sigaction(SIGWINCH, &action, &previous_action));

  static_cast<void>(write_text(STDOUT_FILENO, "\x1B[?1049h\x1B[2J\x1B[H"));
  PrefixParser prefix_parser;
  std::array<std::byte, input_bytes_max> input{};
  std::array<std::byte, input_bytes_max * 2U> encoded_input{};
  std::array<std::byte, std::size_t{64} * 1'024U> output{};
  bool attached = true;
  while (attached) {
    if (resize_pending != 0) {
      resize_pending = 0;
      if (!send_resize(connection, terminal_size())) {
        break;
      }
    }

    std::array<pollfd, 2> descriptors{{
        {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
        {.fd = connection, .events = POLLIN, .revents = 0},
    }};
    const auto poll_result = ::poll(descriptors.data(), descriptors.size(), -1);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    const auto& input_events = descriptors.front();
    const auto& server_events = descriptors.back();
    if ((server_events.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      const auto bytes_read = ::recv(connection, output.data(), output.size(), 0);
      if (bytes_read <= 0 || !write_all(STDOUT_FILENO, std::span(output).first(
                                                           static_cast<std::size_t>(bytes_read)))) {
        break;
      }
    }

    if ((input_events.revents & POLLIN) != 0) {
      const auto bytes_read = ::read(STDIN_FILENO, input.data(), input.size());
      if (bytes_read <= 0) {
        break;
      }
      const auto parsed = prefix_parser.parse(
          std::span(input).first(static_cast<std::size_t>(bytes_read)), encoded_input);
      if (parsed.bytes > 0 &&
          !send_input(connection, std::span(encoded_input).first(parsed.bytes))) {
        break;
      }
      if (parsed.detach) {
        const std::array detach{packet_detach};
        static_cast<void>(send_all(connection, detach));
        attached = false;
      }
    }
  }

  static_cast<void>(::sigaction(SIGWINCH, &previous_action, nullptr));
  constexpr std::string_view restore_terminal =
      "\x1B[0m\x1B[?2026l\x1B[?1l\x1B[?9l\x1B[?1000l\x1B[?1002l\x1B[?1003l"
      "\x1B[?1004l\x1B[?1005l\x1B[?1006l\x1B[?1007l\x1B[?1015l\x1B[?1016l"
      "\x1B[?2004l\x1B[?25h\x1B[?7h\x1B[?1049l";
  static_cast<void>(write_text(STDOUT_FILENO, restore_terminal));
  close_descriptor(connection);
  return 0;
}

[[nodiscard]] auto run_control_command(const std::string& path, const std::byte command,
                                       const bool report_missing) noexcept -> int {
  int connection = open_connection(path);
  if (connection < 0) {
    if (report_missing) {
      static_cast<void>(write_text(STDERR_FILENO, "no fiber session\n"));
    }
    return 1;
  }
  const std::array request{command};
  if (!send_all(connection, request)) {
    close_descriptor(connection);
    return 1;
  }

  std::array<std::byte, std::size_t{4} * 1'024U> response{};
  while (true) {
    const auto bytes_read = ::recv(connection, response.data(), response.size(), 0);
    if (bytes_read == 0) {
      break;
    }
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }
      close_descriptor(connection);
      return 1;
    }
    if (!write_all(STDOUT_FILENO,
                   std::span(response).first(static_cast<std::size_t>(bytes_read)))) {
      close_descriptor(connection);
      return 1;
    }
  }
  close_descriptor(connection);
  return 0;
}

[[nodiscard]] auto active_session_count() -> std::size_t {
  std::size_t active = 0;
  for (const auto& session : discover_sessions()) {
    if (server_available(socket_path(session))) {
      ++active;
    }
  }
  return active;
}

[[nodiscard]] auto validate_session(const std::string_view session) noexcept -> bool {
  if (valid_session_name(session)) {
    return true;
  }
  static_cast<void>(write_text(
      STDERR_FILENO,
      "invalid session name; use 1-32 ASCII letters, digits, underscores, or hyphens\n"));
  return false;
}

} // namespace

auto start(const std::string_view session) -> int {
  if (!validate_session(session)) {
    return 1;
  }
  const auto path = socket_path(session);
  if (!server_available(path) && active_session_count() >= limits::sessions_hard_max) {
    static_cast<void>(write_text(STDERR_FILENO, "fiber session capacity reached\n"));
    return 1;
  }
  if (!ensure_server(path, session)) {
    static_cast<void>(write_text(STDERR_FILENO, "failed to start fiber session\n"));
    return 1;
  }
  return list(session);
}

auto attach(const std::string_view session) -> int {
  return validate_session(session) ? attach_client(socket_path(session)) : 1;
}

auto create_and_attach(const std::string_view session) -> int {
  if (!validate_session(session)) {
    return 1;
  }
  const auto path = socket_path(session);
  if (!server_available(path) && active_session_count() >= limits::sessions_hard_max) {
    static_cast<void>(write_text(STDERR_FILENO, "fiber session capacity reached\n"));
    return 1;
  }
  if (!ensure_server(path, session)) {
    static_cast<void>(write_text(STDERR_FILENO, "failed to start fiber session\n"));
    return 1;
  }
  return attach_client(path);
}

auto list() -> int {
  const auto sessions = discover_sessions();
  std::size_t listed = 0;
  for (const auto& session : sessions) {
    const auto path = socket_path(session);
    if (run_control_command(path, command_list, false) == 0) {
      ++listed;
    } else {
      static_cast<void>(::unlink(path.c_str()));
    }
  }
  if (listed == 0) {
    static_cast<void>(write_text(STDOUT_FILENO, "no fiber sessions\n"));
  }
  return 0;
}

auto list(const std::string_view session) -> int {
  return validate_session(session) ? run_control_command(socket_path(session), command_list, true)
                                   : 1;
}

auto kill(const std::string_view session) -> int {
  return validate_session(session) ? run_control_command(socket_path(session), command_kill, true)
                                   : 1;
}

auto kill_all() -> int {
  const auto sessions = discover_sessions();
  int result = 0;
  for (const auto& session : sessions) {
    if (run_control_command(socket_path(session), command_kill, false) != 0) {
      result = 1;
    }
  }
  return result;
}

} // namespace fiber::mux
