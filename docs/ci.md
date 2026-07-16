# Continuous integration

## Workflow model

[`quality.yml`](../.github/workflows/quality.yml) is the only pull-request
workflow that validates source changes. It computes the complete affected diff,
selects the lanes that can be affected, runs them in parallel, and joins them
behind one stable required check:

```text
Classify changes
  ├─ Workflow and CI-script lint
  ├─ C++ correctness
  └─ Release benchmark smoke
          ↓
       CI gate
```

The C++ lane checks C++ and Nix formatting, evaluates the flake, installs the
exact Conan lock, builds and tests the debug tree, runs clang-tidy and clangd,
and smokes command dispatch. The benchmark lane independently builds the
release benchmark target and briefly executes every registered benchmark. It
checks compilation and runtime invariants; it does not compare timings from a
shared GitHub runner.

The conservative mapping lives in
[`scripts/ci/changes.py`](../scripts/ci/changes.py), with contract tests in
[`tools/test_ci_changes.py`](../tools/test_ci_changes.py):

- production headers, sources, or the Ghostty submodule select C++ and
  benchmarks;
- test-only changes select C++;
- benchmark-only changes select their self-contained format, tidy, build, and
  runtime lane without rebuilding the debug test tree;
- CMake, Conan, and dependency changes select both build modes;
- CI orchestration changes deliberately select every lane;
- unrelated documentation can proceed directly from classification to the
  gate.

Pull requests and merge groups use merge-base-to-head changes. Pushes compare
the old and new ref endpoints so force-push removals and reverts are not lost.
When no trustworthy base commit is available, classification fails safe by
selecting every lane. Do not replace the classifier with workflow-level `paths`
filters: GitHub can leave an entirely skipped required workflow pending.

## Extended validation

[`extended.yml`](../.github/workflows/extended.yml) runs daily and on demand.
It covers the four systems exported by the Nix flake:

- `x86_64-linux`;
- `aarch64-linux`;
- `x86_64-darwin`;
- `aarch64-darwin`.

It also runs AddressSanitizer plus UndefinedBehaviorSanitizer on Linux in an
isolated `build/sanitizers` tree and uploads a longer Google Benchmark JSON
sample for 14 days. Benchmark samples
are observational because hosted-runner timing is machine-sensitive; durable
performance claims and local baselines remain documented in
[`performance.md`](performance.md).

Both workflows check out the pinned Ghostty submodule and enter the locked Nix
development environment. Actions are pinned to immutable commit SHAs and jobs
are read-only. Conan packages, ccache objects, and Zig's content-addressed
cache are restored by toolchain/profile key; only successful trusted `main`
pushes (or scheduled `main` jobs) can save them. Pull requests can read those
caches but cannot replace the default branch's entries.

## Local hooks

[`hk.pkl`](../hk.pkl) mirrors the validation tiers without making every commit
wait for a build. Pre-commit operates only on staged files, applies safe
formatters, preserves unstaged changes, and runs hygiene/actionlint/ShellCheck
or classifier tests only when their inputs are staged. Pre-push adds one
incremental Conan/CMake refresh and debug build/test pass, then clang-tidy and
clangd only for affected C++ files. Expensive release
benchmarks, sanitizer builds, and platform matrices remain in CI.

Install both hooks with `just hooks`. Use `just hooks-check` to run fast and
pre-push checks over the entire repository, and `just hooks-fix` to apply the
safe pre-commit-tier fixers. Run Git from `nix develop` so the pinned hook tools are on
`PATH`.

## Required repository setting

Configure the `main` branch ruleset with:

- pull requests required;
- `CI gate` (from GitHub Actions) required;
- merge queue enabled only while the existing `merge_group` trigger remains.

Individual conditional lanes should not be required checks. `CI gate` verifies
that every selected lane succeeded while allowing intentionally skipped lanes.

## Local reproduction

Run the same merge-blocking suites used by Actions:

```sh
just ci-cpp
just ci-benchmarks
just ci-workflows
```

`just ci-check` runs all three. The jobs can also be invoked directly from an
already-entered `nix develop` shell:

```sh
scripts/ci/cpp
scripts/ci/benchmarks smoke
scripts/ci/workflows
```

The scheduled-only suites are:

```sh
scripts/ci/platform
scripts/ci/sanitizers
scripts/ci/benchmarks extended
```

CI always passes `conan.lock` explicitly. If dependencies change, regenerate
and review the lock rather than bypassing it with a partial or unlocked install.
