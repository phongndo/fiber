# fiber

A bounded, data-oriented C++23 foundation for a fast terminal multiplexer, backed by the
vendored `libghostty-vt` library. Lua 5.5 is the configuration language, and Zstandard supports
bounded snapshots and application-owned compressed state.

The current vertical slice provides named persistent sessions, each with one PTY pane and start,
attach, detach, list, and kill commands, plus release-enabled invariant assertions, generational
IDs, bounded byte queues,
and an isolated Ghostty terminal adapter. The adapter owns the canonical terminal and dirty render
state, captures terminal effects into bounded queues, and enforces a quota-tracked allocator. See
[`docs/architecture.md`](docs/architecture.md) for the ownership model and system invariants, and
[`docs/performance.md`](docs/performance.md) for measured renderer and multiplexer results.

## Toolchain

On macOS, the locked Nix shell uses the LLVM 22 installation from
`brew --prefix llvm`; it does not install or select Nix's Clang. On Linux, the
shell supplies the matching Nix LLVM 22 toolchain. It also supplies CMake,
Ninja, ccache, Conan 2, Zig (required to build `libghostty-vt`), hk, and the
formatters. Conan supplies GoogleTest, GoogleMock, and Google Benchmark. CMake
rejects non-LLVM C++ compilers, requires C++23 without compiler extensions,
exports the compilation database for clangd, and promotes the strict warning
set to errors.

```sh
git submodule update --init --recursive
nix develop
just versions
just build
just test
```

The first build lets Conan 2 download/build its pinned packages and lets Zig
build the pinned Ghostty source under [`vendor/ghostty`](vendor/ghostty).
Subsequent C++ compilations use ccache.

## Commands

```sh
just configure              # Conan install + CMake/Ninja generation
just build                  # Debug build (`just profile=release build` for release)
just run                    # Show fiber usage
just demo                   # Run the scripted libghostty-vt demo
just build && ./build/debug/fiber new  # Start and attach to pane 0
just test                   # GoogleTest and GoogleMock
just bench                  # Google Benchmark
just fmt                    # Apply clang-format and nixpkgs-fmt
just fmt-check              # Verify formatting only
just lint                   # clang-tidy; every diagnostic is an error
just lsp-check              # clangd parse/diagnostic check
just lsp                    # Start clangd for an editor
just check                  # All format, lint, LSP, build, and test checks
just hooks                  # Validate hk config and install Git hooks
```

For example, use `just profile=release build` or `just profile=release bench`
for an optimized build.

## Single-pane mux

```sh
./build/debug/fiber new work       # start session "work" and attach
# Press C-b d to detach.
./build/debug/fiber new logs       # create another independent session
./build/debug/fiber list           # list all sessions
./build/debug/fiber attach work    # reattach to work
./build/debug/fiber kill work      # stop one session
./build/debug/fiber kill-all       # stop every session
```

Each session currently owns one pane and permits one attached client. It starts the account's login
shell, preserves the launch environment, advertises `xterm-256color`, and forwards terminal resizing
to the PTY. Session names contain 1-32 ASCII letters, digits, underscores, or hyphens. `C-b C-b`
sends a literal `C-b`. Launch `fiber` directly from the normal shell rather
than through `nix develop` when testing personal shell configuration.

## Editor and commit hooks

Point the editor at `clangd` from `nix develop`. [`.clangd`](.clangd) uses
`build/debug/compile_commands.json`, strict missing/unused include diagnostics,
clang-tidy diagnostics, background indexing, and inlay hints. Run
`just configure` before opening the project in an editor.

[`hk.pkl`](hk.pkl) runs clang-format and clang-tidy over changed C++ files and
runs clangd checks over changed production sources/headers on pre-commit and
pre-push. Install it after configuration:

```sh
just hooks
```

The hook formats and re-stages changed C++ files. Bypass hk for one command only
when necessary with `HK=0 git commit`.

## Vendored dependency

Ghostty is a Git submodule pinned to commit
`55a3e33ab26a23d75b274b23c7f76d837db00578`. Its CMake wrapper invokes Zig to
produce `libghostty-vt`; fiber links the static target. Update it deliberately
by checking out a reviewed Ghostty commit in `vendor/ghostty` and committing the
new submodule pointer.
