nix := "nix develop --command"
profile := "debug"
build_type := if profile == "release" { "Release" } else { "Debug" }
cpp_files := "include src tests benchmarks"

_default:
    @just --list

# Show the pinned development tool versions.
versions:
    {{ nix }} clang++ --version
    {{ nix }} clangd --version
    {{ nix }} clang-tidy --version
    {{ nix }} cmake --version
    {{ nix }} ninja --version
    {{ nix }} ccache --version
    {{ nix }} conan --version
    {{ nix }} zig version
    {{ nix }} hk --version

# Install Conan dependencies for the selected profile.
deps:
    rm -f CMakeUserPresets.json
    {{ nix }} conan install . \
        --output-folder=build/{{ profile }}/conan \
        --profile:all=conan/profiles/llvm \
        --settings=build_type={{ build_type }} \
        --conf=tools.cmake.cmaketoolchain:user_presets= \
        --build=missing

# Generate Ninja files and compile_commands.json.
configure: deps
    {{ nix }} cmake --preset {{ profile }} \
        -DFIBER_BUILD_TESTS=ON -DFIBER_BUILD_BENCHMARKS=ON

# Build the application, tests, and benchmarks.
build: configure
    {{ nix }} cmake --build --preset {{ profile }}

# Run the application.
run: build
    {{ nix }} ./build/{{ profile }}/fiber

# Run the scripted libghostty-vt demo.
demo: build
    {{ nix }} ./build/{{ profile }}/fiber demo

# Run the GoogleTest/GoogleMock suite.
test: build
    {{ nix }} ctest --preset {{ profile }}

# Run Google Benchmark.
bench: build
    {{ nix }} ./build/{{ profile }}/fiber_benchmarks

# Format C++ and Nix files in place.
fmt:
    {{ nix }} bash -c "find {{ cpp_files }} -type f \
        \\( -name '*.cpp' -o -name '*.hpp' \\) -print0 | xargs -0 clang-format -i"
    {{ nix }} nixpkgs-fmt flake.nix

# Check formatting without changing files.
fmt-check:
    {{ nix }} bash -c "find {{ cpp_files }} -type f \
        \\( -name '*.cpp' -o -name '*.hpp' \\) -print0 | \
        xargs -0 clang-format --dry-run --Werror"
    {{ nix }} nixpkgs-fmt --check flake.nix

# Run clang-tidy with all diagnostics promoted to errors.
lint: configure
    {{ nix }} clang-tidy --quiet -p build/{{ profile }} \
        src/*.cpp src/mux/*.cpp src/vt/*.cpp tests/*.cpp benchmarks/*.cpp

# Check public headers and production translation units through clangd.
lsp-check: configure
    {{ nix }} cmake/check-clangd.sh \
        include/fiber/*.hpp include/fiber/mux/*.hpp include/fiber/vt/*.hpp \
        src/*.cpp src/vt/*.cpp

# Start clangd for editor integrations.
lsp:
    {{ nix }} clangd --enable-config

# Run formatting, lint, LSP diagnostics, build, and tests.
check: fmt-check lint lsp-check test

# Run the merge-blocking C++ suite used by GitHub Actions.
ci-cpp:
    {{ nix }} scripts/ci/cpp

# Build and exercise every benchmark with short, non-thresholded samples.
ci-benchmarks:
    {{ nix }} scripts/ci/benchmarks smoke

# Test CI orchestration and lint Actions workflows and shell scripts.
ci-workflows:
    {{ nix }} scripts/ci/workflows

# Reproduce every merge-blocking CI lane locally.
ci-check: ci-cpp ci-benchmarks ci-workflows

# Configure the debug tree and install this repository's hk hooks.
hooks:
    {{ nix }} bash -c 'scripts/ci/configure debug -DFIBER_BUILD_TESTS=ON -DFIBER_BUILD_BENCHMARKS=ON && hk validate && hk install'

# Run every fast and pre-push hk check over the repository.
hooks-check:
    {{ nix }} hk check --all --check

# Apply every safe pre-commit-tier hk fixer.
hooks-fix:
    {{ nix }} hk fix --all

# Remove generated build and vendored Zig output.
clean:
    rm -rf build CMakeUserPresets.json .zig-cache \
        third_party/ghostty/.zig-cache third_party/ghostty/zig-out third_party/ghostty/zig-pkg
    {{ nix }} ccache --clear
