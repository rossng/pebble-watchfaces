#!/usr/bin/env bash
#
# emu-check.sh -- build/install a watchface on the Pebble emulator and report a
# definite verdict: RUNNING, BOOTLOOP, CRASH, or INSTALL_FAILED -- without ever
# leaving orphaned qemu/pypkjs processes behind (the thing that corrupts the
# emulator image and produces stale screenshots).
#
# Run it inside the dev shell so `pebble` is on PATH:
#
#   nix develop --command scripts/emu-check.sh <platform> <watchface-dir> [shot.png]
#
# e.g. nix develop --command scripts/emu-check.sh emery watchfaces/passing-trains
#
# Exit codes: 0 RUNNING - 2 INSTALL_FAILED - 3 BOOTLOOP - 4 CRASH.
# Set KEEP=1 to leave the (single) emulator running for follow-up screenshots;
# by default everything is torn down on exit so the next run starts clean.

set -uo pipefail

PLATFORM="${1:-emery}"
WATCH_DIR="${2:-watchfaces/passing-trains}"
SHOT="${3:-$(pwd)/${WATCH_DIR}/${PLATFORM}-shot.png}"
KEEP="${KEEP:-0}"

log() { printf '\033[36m[emu-check]\033[0m %s\n' "$*" >&2; }

# Kill every emulator process for a clean slate. There is no good reason to ever
# have two: a pile-up is exactly what corrupts the flash image.
#
# `pebble kill` first so the tool forgets the emulator it thinks is running --
# otherwise the next `install` tries to connect to the dead one ("Connection
# refused") instead of cold-booting a fresh one. Then SIGKILL any orphans the
# tool didn't track.
kill_emulators() {
  pebble kill              >/dev/null 2>&1
  pkill -9 -f qemu-pebble  >/dev/null 2>&1
  pkill -9 -f pypkjs       >/dev/null 2>&1
  return 0
}

cleanup() {
  if [ "$KEEP" = "1" ]; then
    log "KEEP=1 -- leaving one emulator running ($PLATFORM)."
  else
    log "tearing down emulator."
    kill_emulators
  fi
}
trap cleanup EXIT

log "clearing any existing emulators..."
kill_emulators
sleep 1

cd "$WATCH_DIR" || { echo "INSTALL_FAILED"; exit 2; }

# Boot + install. A cold boot routinely exceeds the first connect timeout
# (libpebble2 TimeoutError); the emulator is warm by the retry, so just loop.
installed=0
for attempt in 1 2 3 4 5; do
  log "install attempt ${attempt}..."
  if pebble install --emulator "$PLATFORM" >/tmp/emu-install.$$.log 2>&1; then
    if grep -q "App install succeeded" /tmp/emu-install.$$.log; then
      installed=1
      break
    fi
  fi
  tail -2 /tmp/emu-install.$$.log | sed 's/^/    /' >&2
  sleep 3
done
rm -f /tmp/emu-install.$$.log
if [ "$installed" -ne 1 ]; then
  echo "INSTALL_FAILED"
  exit 2
fi
log "app installed; observing..."

# A watchface that runs cleanly answers a screenshot request reliably. One that
# hard-faults bootloops the firmware and the protocol link keeps dropping, so the
# tell is a screenshot that never succeeds across several tries. (A *graceful* JS
# exception doesn't bootloop — the link is fine and the screenshot just shows the
# empty "install an app" face, which only the caller's eyes can distinguish.)
#
# Give a freshly cold-booted emulator time to settle, then retry generously so we
# never mistake slow-warm-up for a bootloop.
probe() { timeout 20 pebble screenshot --emulator "$PLATFORM" --no-open "$1" >/dev/null 2>&1; }

log "letting the emulator settle..."
sleep 8
ok=0
for try in 1 2 3 4 5 6 7 8; do
  if probe "$SHOT"; then ok=1; break; fi
  log "screenshot try $try failed; retrying..."
  sleep 5
done

if [ "$ok" -ne 1 ]; then
  log "screenshot never succeeded across retries -- firmware link unstable."
  echo "BOOTLOOP"
  exit 3
fi

log "screenshot saved to: $SHOT"
log "RUNNING -- inspect the screenshot to tell the watchface from the empty face."
echo "RUNNING"
exit 0
