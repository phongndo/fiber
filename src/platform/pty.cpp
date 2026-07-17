#include "platform/pty.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <string_view>

#include <pwd.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __APPLE__
#include <util.h>
#elifdef __linux__
#include <pty.h>
#else
#error "fiber requires forkpty"
#endif

namespace fiber::platform {

[[nodiscard]] auto spawn_login_shell(int& pty_descriptor) noexcept -> pid_t {
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

[[nodiscard]] auto resize_pty(const int pty_descriptor, const std::uint16_t columns,
                              const std::uint16_t rows) noexcept -> bool {
  winsize native_size{
      .ws_row = rows,
      .ws_col = columns,
      .ws_xpixel = 0,
      .ws_ypixel = 0,
  };
  // ioctl is variadic because its third argument depends on the request.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  return ::ioctl(pty_descriptor, TIOCSWINSZ, &native_size) == 0;
}

} // namespace fiber::platform
