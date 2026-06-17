# Experimental Nix package for `pebble-tool` (the Pebble / Alloy SDK CLI).
#
# STATUS: starting point — NOT yet verified to build. Before `nix build
# .#pebble-tool` will work you must confirm, against the current release on
# https://pypi.org/project/pebble-tool/ :
#   1. `version`
#   2. the source `hash` (replace `lib.fakeHash`)
#   3. the full Python dependency list in `dependencies`
#
# Even once it builds, the package only provides the *tool*. The actual SDK
# (QEMU emulator images + ARM toolchain) is fetched at runtime by
# `pebble sdk install latest` into ~/.pebble-sdk — an inherently impure step that
# is not, and realistically cannot be, captured by this derivation. For day-to-day
# development prefer the flake dev shell, which bootstraps pebble-tool with uv and
# provides the native libraries the emulator needs.
{
  lib,
  python3Packages,
  fetchPypi,
}:
python3Packages.buildPythonApplication rec {
  pname = "pebble-tool";
  version = "6.0.1"; # TODO: confirm latest on PyPI

  pyproject = true;

  src = fetchPypi {
    inherit pname version;
    # TODO: nix-prefetch-url --unpack \
    #   https://pypi.io/packages/source/p/pebble-tool/pebble_tool-${version}.tar.gz
    hash = lib.fakeHash;
  };

  build-system = [ python3Packages.setuptools ];

  # TODO: confirm against pebble-tool's pyproject.toml / setup.py. These are the
  # usual suspects for the classic tool; the Alloy-era release may differ.
  dependencies = with python3Packages; [
    requests
    pyserial
    websocket-client
    pillow
    pygments
    six
  ];

  # Tests expect a downloaded SDK and a connected emulator.
  doCheck = false;

  meta = {
    description = "Command-line tool for building and running Pebble (Alloy) apps";
    homepage = "https://developer.repebble.com/sdk/";
    license = lib.licenses.mit;
    mainProgram = "pebble";
  };
}
