#include "fiber/config.hpp"

#include <lua.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

namespace fiber::config {
namespace {

struct RuntimeLimits final {
  std::size_t bytes_current{0};
  std::size_t bytes_max{0};
  std::uint64_t instructions_remaining{0};
  std::uint32_t hook_interval{0};
  bool instruction_limit_reached{false};
};

// Lua's allocator ABI requires realloc/free semantics.
// NOLINTBEGIN(cppcoreguidelines-no-malloc)
void* allocate(void* userdata, void* memory, const std::size_t old_size,
               const std::size_t new_size) noexcept {
  auto& limits = *static_cast<RuntimeLimits*>(userdata);
  if (new_size == 0) {
    std::free(memory);
    if (memory != nullptr) {
      limits.bytes_current -= old_size;
    }
    return nullptr;
  }

  const auto accounted_old_size = memory == nullptr ? std::size_t{0} : old_size;
  if (new_size > limits.bytes_max - (limits.bytes_current - accounted_old_size)) {
    return nullptr;
  }
  void* const resized = std::realloc(memory, new_size);
  if (resized != nullptr) {
    limits.bytes_current = limits.bytes_current - accounted_old_size + new_size;
  }
  return resized;
}
// NOLINTEND(cppcoreguidelines-no-malloc)

void instruction_hook(lua_State* state, [[maybe_unused]] lua_Debug* debug) {
  void* userdata = nullptr;
  static_cast<void>(lua_getallocf(state, &userdata));
  auto& limits = *static_cast<RuntimeLimits*>(userdata);
  if (limits.instructions_remaining <= limits.hook_interval) {
    limits.instruction_limit_reached = true;
    // luaL_error is variadic even when no format arguments are required.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    static_cast<void>(luaL_error(state, "fiber configuration instruction limit"));
    return;
  }
  limits.instructions_remaining -= limits.hook_interval;
}

void remove_global(lua_State* state, const char* name) noexcept {
  lua_pushnil(state);
  lua_setglobal(state, name);
}

void open_restricted_libraries(lua_State* state) noexcept {
  struct Library final {
    const char* name;
    lua_CFunction open;
  };
  constexpr std::array libraries{
      Library{.name = LUA_GNAME, .open = luaopen_base},
      Library{.name = LUA_TABLIBNAME, .open = luaopen_table},
      Library{.name = LUA_STRLIBNAME, .open = luaopen_string},
      Library{.name = LUA_MATHLIBNAME, .open = luaopen_math},
      Library{.name = LUA_UTF8LIBNAME, .open = luaopen_utf8},
  };
  for (const auto& library : libraries) {
    luaL_requiref(state, library.name, library.open, 1);
    lua_pop(state, 1);
  }
  remove_global(state, "collectgarbage");
  remove_global(state, "dofile");
  remove_global(state, "load");
  remove_global(state, "loadfile");
}

[[nodiscard]] auto integer_field(lua_State* state, const char* name, const lua_Integer fallback,
                                 const lua_Integer minimum, const lua_Integer maximum) noexcept
    -> std::expected<lua_Integer, Error> {
  const auto type = lua_getfield(state, -1, name);
  if (type == LUA_TNIL) {
    lua_pop(state, 1);
    return fallback;
  }
  int is_integer = 0;
  const auto value = lua_tointegerx(state, -1, &is_integer);
  lua_pop(state, 1);
  if (is_integer == 0 || value < minimum || value > maximum) {
    return std::unexpected(Error::invalid_result);
  }
  return value;
}

[[nodiscard]] auto prefix_field(lua_State* state) noexcept -> std::expected<std::uint8_t, Error> {
  const auto type = lua_getfield(state, -1, "prefix");
  if (type == LUA_TNIL) {
    lua_pop(state, 1);
    return std::uint8_t{0x02};
  }
  std::size_t length = 0;
  const char* const text = lua_tolstring(state, -1, &length);
  if (text == nullptr) {
    lua_pop(state, 1);
    return std::unexpected(Error::invalid_result);
  }
  const auto characters = std::span(text, length);
  std::uint8_t prefix = 0;
  if (length == 1) {
    prefix = static_cast<std::uint8_t>(static_cast<unsigned char>(characters.front()));
  } else if (length == 3 && characters.front() == 'C' && characters.subspan(1, 1).front() == '-' &&
             characters.subspan(2, 1).front() >= 'a' && characters.subspan(2, 1).front() <= 'z') {
    prefix = static_cast<std::uint8_t>(characters.subspan(2, 1).front() - 'a' + 1);
  }
  lua_pop(state, 1);
  if (prefix == 0 || prefix > 0x7FU) {
    return std::unexpected(Error::invalid_result);
  }
  return prefix;
}

[[nodiscard]] auto extract_settings(lua_State* state) noexcept -> std::expected<Settings, Error> {
  if (lua_type(state, -1) != LUA_TTABLE) {
    return std::unexpected(Error::invalid_result);
  }
  const auto prefix = prefix_field(state);
  const auto frame_delay = integer_field(state, "frame_delay_us", 2'000, 0, 1'000'000);
  const auto scrollback = integer_field(
      state, "scrollback_rows", static_cast<lua_Integer>(limits::terminal_scrollback_rows_default),
      0, static_cast<lua_Integer>(limits::terminal_scrollback_rows_hard_max));
  if (!prefix.has_value() || !frame_delay.has_value() || !scrollback.has_value()) {
    return std::unexpected(Error::invalid_result);
  }
  return Settings{
      .prefix = *prefix,
      .frame_delay_us = static_cast<std::uint32_t>(*frame_delay),
      .scrollback_rows = static_cast<std::size_t>(*scrollback),
  };
}

} // namespace

// Lua errors are contained by lua_pcall and converted into typed external errors.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto load(const std::string_view source, const Limits requested_limits) noexcept
    -> std::expected<Settings, Error> {
  if (source.size() > source_bytes_max) {
    return std::unexpected(Error::source_too_large);
  }
  if (requested_limits.memory_bytes == 0 || requested_limits.instructions == 0 ||
      requested_limits.memory_bytes > limits::terminal_allocation_bytes_hard_max) {
    return std::unexpected(Error::invalid_result);
  }

  RuntimeLimits runtime{
      .bytes_max = requested_limits.memory_bytes,
      .instructions_remaining = requested_limits.instructions,
      .hook_interval =
          static_cast<std::uint32_t>(std::min<std::uint64_t>(requested_limits.instructions, 1'000)),
  };
  lua_State* const state = lua_newstate(&allocate, &runtime, 0);
  if (state == nullptr) {
    return std::unexpected(Error::out_of_memory);
  }
  open_restricted_libraries(state);
  lua_sethook(state, &instruction_hook, LUA_MASKCOUNT, static_cast<int>(runtime.hook_interval));

  const auto loaded = luaL_loadbufferx(state, source.data(), source.size(), "fiber-config", "t");
  if (loaded != LUA_OK) {
    const auto error = loaded == LUA_ERRMEM ? Error::out_of_memory : Error::syntax;
    lua_close(state);
    return std::unexpected(error);
  }
  const auto called = lua_pcall(state, 0, 1, 0);
  if (called != LUA_OK) {
    auto error = Error::invalid_result;
    if (runtime.instruction_limit_reached) {
      error = Error::instruction_limit;
    } else if (called == LUA_ERRMEM) {
      error = Error::out_of_memory;
    }
    lua_close(state);
    return std::unexpected(error);
  }

  auto settings = extract_settings(state);
  lua_close(state);
  return settings;
}

} // namespace fiber::config
