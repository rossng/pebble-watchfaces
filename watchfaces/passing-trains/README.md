# Passing Trains

A Pebble watchface that fills the screen with a random Dutch (NS) train — scaled to
the full height of the display, sitting on the floor like a real train — and slowly
pans along it. The time sits in a bottom bar styled after an NS station departure
board: white time on NS blue with a yellow accent strip and a yellow "platform" chip
showing the date.

- On launch it picks a random train and holds at its front for the first minute.
- Every minute the train drifts a few pixels, so over an hour or two the whole
  length of it rolls by.
- A subtle **type label** (Koploper, Buffel, Sprinter (Nieuwe Generatie), …) floats
  in the sky above each train, pinned to it so it scrolls past and slides away with
  the train.
- When a train has fully passed (plus a short sky gap) another random train rolls in.
  The pool is **10 distinct NS train types**, one short variant of each.
- A **tap / wrist-shake** fast-forwards: the current train scrolls clean off in a
  couple of seconds and stops at the head of the next one.

> **Note — this is a native C watchface, not an Alloy/TypeScript one** like the rest
> of the repo. It has to be: filling the full screen height means upscaling the train
> at draw time, and only C gives you the framebuffer access to do that (the Alloy
> path can't — see [How it draws](#how-it-draws)). Use `watchfaces/simple-time/` as
> the template for a _normal_ watchface; this one is the exception.

Train artwork comes from [`rossng/ns-treinen`](https://github.com/rossng/ns-treinen),
pulled in as the `ns-treinen/` git submodule.

```
ns-treinen/                  # submodule: upstream train PNGs
scripts/prepare-images.mjs   # bakes a curated set into small Pebble PNG resources
resources/images/*.png       # generated: posterised, floor-anchored trains (committed)
src/c/
  main.c                     # the watchface (scaled blit + drift + tap + time bar)
  trains.h                   # generated: resource ids + type names the watch picks from
package.json                 # native project; resources.media is generated
```

## Rebuilding the train set

```bash
pnpm prepare:images   # re-bake resources from the submodule (needs ImageMagick)
pnpm build            # produces build/passing-trains.pbw
```

Edit `BODY_HEIGHT` / `CANVAS_HEIGHT` and the `TRAINS` list at the top of
[`scripts/prepare-images.mjs`](scripts/prepare-images.mjs) to retune. For each train
the script measures where the solid body starts (below the thin pantograph), scales
so every train's **body** is `BODY_HEIGHT` — so the trains look a consistent height
regardless of roof gear — and bakes it bottom-anchored on a `CANVAS_HEIGHT` canvas,
the pantograph sticking up into the sky above. It then regenerates
`resources/images/*.png`, `src/c/trains.h`, and the `resources.media` block in
`package.json`.

**Adding trains is cheap; adding _long_ trains is not.** Only two trains are resident
in RAM at a time (current + next), each held **uncompressed** as a `gbitmap`, so the
pool size is bounded by flash (PNGs are ~3 KB each — 256 KB to spare), but the **two
widest must fit the ~125 KB heap together**. A `gbitmap` is `width × CANVAS_HEIGHT`
bytes and width grows with the train's real length, so the very long international /
loco-hauled stock (ICE, Thalys, ICR, the 8-car intercity sets) is left out — one of
those alone is 100 KB+ and blows the budget. The current 10 types top out at a
~81 KB worst-case pair (`prepare:images` prints each train's gbitmap size). To add
more long trains, lower `BODY_HEIGHT`/`CANVAS_HEIGHT` together (smaller gbitmaps,
blockier upscale) — they're at 34 px / 48 px, a ~4× upscale.

## Publishing to the appstore

This face targets `emery` + `gabbro` only — the **new** Pebble hardware (Pebble
Time 2 / Pebble Round 2) — so it publishes to the revived
[Pebble appstore](https://developer.repebble.com/), not the classic Rebble store
(it ships no `aplite`/`basalt`/`chalk`/`diorite` binaries, so classic Pebbles can't
run it anyway).

```bash
pebble login            # auth is tied to your GitHub account
pnpm publish:appstore   # = pebble build && pebble publish
```

`pebble publish` uploads `build/passing-trains.pbw` to the developer portal and
auto-generates the promo screenshots/GIFs for every target platform. (Web
alternative: upload the `.pbw` at
[developer.repebble.com/dashboard/submit](https://developer.repebble.com/dashboard/submit).)

Before each submission: bump `version` in [`package.json`](package.json) (release
notes are per-version), and remember the listing is public — the train artwork is
NS (Dutch Railways) livery from the [`ns-treinen`](https://github.com/rossng/ns-treinen)
submodule, so confirm there's no trademark/branding concern first.

## How it draws

The full-height trick is a hand-written scaled blit. A train scaled to ~200 px tall
would be ~1500 px wide — far too big to hold in RAM and impossible to bake into
flash. Instead each train lives in flash as a small (48 px-tall) colour PNG, which
the SDK loads into an 8-bit `gbitmap`. Every frame, `update_proc`:

1. captures the framebuffer with `graphics_capture_frame_buffer` (8-bit `GColor8` on
   colour Pebble — **the same byte layout as the source bitmap**, `(a<<6)|(r<<4)|(g<<2)|b`),
2. for each on-screen pixel, nearest-neighbour-samples the small source
   (`sx = (pan + x)/scale`, `sy = y/scale`) and copies the byte straight in,
3. uses each row's `GBitmapDataRowInfo` `min_x`/`max_x` so the round (gabbro) display
   is masked correctly,
4. releases the framebuffer and draws the time bar normally.

No scaled image is ever materialised — only the on-screen pixels exist, so RAM stays
tiny regardless of display size. The trains are laid out as a notional
`[current | gap | next]` strip; `pan` (in source pixels) advances each minute and a
tap glides it to the next train's head, after which `next` is promoted to `current`.

This is why the face is C and not Alloy: Moddable's Poco can't scale a bitmap and
gives no framebuffer access, and the XS engine's fixed ~32 KB heap couldn't hold an
upscaled buffer anyway. See the repo [AGENTS.md](../../AGENTS.md) for that history.

See the [repository README](../../README.md) for environment setup and the emulator;
`scripts/emu-check.sh` (repo root) installs a build and reports RUNNING / BOOTLOOP
without leaving orphaned emulators behind.
