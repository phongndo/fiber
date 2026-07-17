#include "daemon/session.hpp"

#include "core/engine.hpp"
#include "fiber/limits.hpp"
#include "platform/io.hpp"
#include "protocol/single_pane.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fiber::daemon {
namespace {

constexpr auto command_list = protocol::wire_byte(protocol::ControlCommand::list);
constexpr auto command_kill = protocol::wire_byte(protocol::ControlCommand::kill);
using platform::close_descriptor;
using platform::read_exact;
using platform::send_all;
using platform::write_all;
using platform::write_text;

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

struct OwnedEndpoint final {
  const char* path;
  int listener;
  int session_lock;
};

void release_owned_endpoint(void* const context) noexcept {
  auto& endpoint = *static_cast<OwnedEndpoint*>(context);
  close_descriptor(endpoint.listener);
  static_cast<void>(::unlink(endpoint.path));
  close_descriptor(endpoint.session_lock);
}

[[nodiscard]] auto run_owned_server(const std::string& path,
                                    const std::string_view session) noexcept -> int {
  static_cast<void>(::signal(SIGPIPE, SIG_IGN));
  const auto previous_mask = ::umask(0077);
  int session_lock = -1;
  int listener = create_listener(path, session_lock);
  static_cast<void>(::umask(previous_mask));
  if (listener < 0) {
    close_descriptor(session_lock);
    return 1;
  }

  OwnedEndpoint endpoint{
      .path = path.c_str(),
      .listener = listener,
      .session_lock = session_lock,
  };
  return core::run_session(listener, session, &release_owned_endpoint, &endpoint);
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
    ::_exit(run_owned_server(path, session));
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

} // namespace

[[nodiscard]] auto validate_session(const std::string_view session) noexcept -> bool {
  if (valid_session_name(session)) {
    return true;
  }
  static_cast<void>(write_text(
      STDERR_FILENO,
      "invalid session name; use 1-32 ASCII letters, digits, underscores, or hyphens\n"));
  return false;
}

[[nodiscard]] auto open_session_connection(const std::string_view session) -> int {
  return open_connection(socket_path(session));
}

[[nodiscard]] auto ensure(const std::string_view session) -> int {
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
  return 0;
}

auto start(const std::string_view session) -> int {
  return ensure(session) == 0 ? list(session) : 1;
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

} // namespace fiber::daemon
