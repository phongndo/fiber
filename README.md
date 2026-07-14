# fiber

A strict C++23 starting point backed by the vendored `libghostty-vt` library.

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
just build                  # Debug build (use `profile=release` for release)
just run                    # Run fiber
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

For example, use `just build profile=release` or `just bench profile=release`
for an optimized build.

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
