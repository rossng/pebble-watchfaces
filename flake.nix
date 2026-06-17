{
  description = "Pebble Alloy watchface monorepo — TypeScript + pnpm toolchain";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems =
        f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      # Experimental: pebble-tool packaged from PyPI. See nix/pebble-tool.nix for
      # caveats — for everyday use prefer the dev shell, which bootstraps it with uv.
      packages = forAllSystems (pkgs: {
        pebble-tool = pkgs.callPackage ./nix/pebble-tool.nix { };
      });

      devShells = forAllSystems (
        pkgs:
        let
          lib = pkgs.lib;
          # Shared libraries the Pebble emulator (a QEMU build) loads at runtime.
          runtimeLibs =
            with pkgs;
            [
              SDL2
              glib
              pixman
              zlib
            ]
            ++ lib.optionals stdenv.isLinux [ sndio ];
        in
        {
          # mkShellNoCC (not mkShell): a plain mkShell pulls in the C compiler
          # stdenv, which exports binutils tool names as environment variables
          # (STRINGS, AR, NM, STRIP, …). Moddable's generated makefiles reference
          # `$(STRINGS)` expecting it to be empty, so a leaked `STRINGS=strings`
          # gives `mc.xsa` a bogus `strings` prerequisite and the build dies with
          # "No rule to make target 'strings'". We don't compile C here anyway —
          # the Pebble SDK ships its own ARM toolchain.
          default = pkgs.mkShellNoCC {
            packages =
              with pkgs;
              [
                nodejs_22
                pnpm
                python3 # pebble-tool needs Python 3.10+
                uv
                git
              ]
              # The emulator launches QEMU with an SDL display; on headless Linux
              # (CI) `xvfb-run` provides a virtual one for screenshot capture.
              ++ lib.optionals stdenv.isLinux [ xvfb-run ]
              ++ runtimeLibs;

            # On Linux the emulator binaries are dynamically linked against the
            # libraries above; expose them. (No-op on macOS.)
            LD_LIBRARY_PATH = lib.optionalString pkgs.stdenv.isLinux (
              lib.makeLibraryPath runtimeLibs
            );

            shellHook = ''
              # Defense in depth: even with mkShellNoCC, make sure no binutils
              # tool variables leak into Moddable's `make` (see note above).
              unset STRINGS AR AS NM RANLIB STRIP OBJCOPY OBJDUMP READELF SIZE LD CC CXX

              # Keep the uv-managed pebble-tool inside the repo so the shell is
              # self-contained and doesn't touch the user's global uv tools.
              export UV_TOOL_DIR="$PWD/.pebble-tooling"
              export UV_TOOL_BIN_DIR="$PWD/.pebble-tooling/bin"
              export PATH="$UV_TOOL_BIN_DIR:$PATH"

              if ! command -v pebble >/dev/null 2>&1; then
                echo "• Bootstrapping pebble-tool with uv (first run only)…"
                uv tool install --quiet pebble-tool \
                  || echo "  ! Automatic install failed — run: uv tool install pebble-tool"
              fi

              echo ""
              echo "Pebble watchface dev shell ready."
              echo "  pebble sdk install latest        # one-time SDK download (~hundreds of MB)"
              echo "  pnpm install                     # workspace dependencies"
              echo "  cd watchfaces/simple-time && pnpm dev   # build + run in the emulator"
              echo ""
            '';
          };
        }
      );

      formatter = forAllSystems (pkgs: pkgs.nixfmt-rfc-style);
    };
}
