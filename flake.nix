{
  description = "fiber C++23 development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    hk.url = "github:jdx/hk/v1.50.0";
  };

  outputs =
    { nixpkgs, hk, ... }:
    let
      systems = [
        "aarch64-darwin"
        "aarch64-linux"
        "x86_64-darwin"
        "x86_64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          llvm = pkgs.llvmPackages_22;
          useHomebrewLlvm = pkgs.stdenv.isDarwin;
        in
        {
          default = pkgs.mkShell {
            packages = pkgs.lib.optionals (!useHomebrewLlvm) [
              llvm.clang
              llvm.clang-tools
            ] ++ [
              pkgs.ccache
              pkgs.cmake
              pkgs.conan
              pkgs.just
              pkgs.ninja
              pkgs.nixpkgs-fmt
              pkgs.zig_0_15
              hk.packages.${system}.default
            ];

            CMAKE_GENERATOR = "Ninja";

            shellHook =
              if useHomebrewLlvm then ''
                if ! command -v brew >/dev/null 2>&1; then
                  echo "fiber: Homebrew is required to locate LLVM" >&2
                  return 1
                fi
                llvm_prefix="$(brew --prefix llvm)"
                export PATH="$llvm_prefix/bin:$PATH"
                export CC="$llvm_prefix/bin/clang"
                export CXX="$llvm_prefix/bin/clang++"
                unset llvm_prefix
              '' else ''
                export CC="${llvm.clang}/bin/clang"
                export CXX="${llvm.clang}/bin/clang++"
              '';
          };
        }
      );

      formatter = forAllSystems (system: (import nixpkgs { inherit system; }).nixpkgs-fmt);
    };
}
