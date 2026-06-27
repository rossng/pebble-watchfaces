# AGENTS.md

Monorepo of **Pebble Alloy** watchfaces — TypeScript that runs on the watch in
Moddable's XS engine. pnpm workspaces; a Nix flake pins the toolchain. Human-facing
detail lives in [README.md](README.md); this file is the fast path for agents.

## Map

- `watchfaces/<name>/` — one self-contained Alloy project each; `src/embeddedjs/main.ts`
  is the watch code. **Use `watchfaces/simple-time/` as the template — it is already
  correct.** (Exception: `passing-trains/` is a native **C** face, not Alloy — it needs
  framebuffer-level scaling Alloy can't do; see its README and the colour-image gotchas.)
- `types/pebble-alloy.d.ts` — ambient editor types (Poco / `watch` / `screen`). The
  _real_ types come from the SDK at build time; these just keep the editor happy.
- `scripts/` — `build-site.mjs` (Pages site), `capture-screenshots.mjs`,
  `emu-check.sh` (install a build + report RUNNING/BOOTLOOP, no orphaned emulators).
  `tools/emu-tui/` — keyboard control panel for a running emulator.
- `flake.nix` — dev shell (`nix develop`). `.github/workflows/` — smoke / pages / screenshots.

## Write + test a watchface (the core loop)

Always run through **pnpm** — it puts the watchface's local `tsc` on `PATH`. A bare
`pebble build` fails with `tsc: command not found`.

```
cd watchfaces/simple-time
pnpm typecheck     # fast, editor-side (uses types/pebble-alloy.d.ts)
pnpm build         # the real test: Moddable build → build/simple-time.pbw
pnpm dev           # build + boot emulator + emu-tui (reuses a warm emulator)
pnpm reset         # `pebble kill && pebble wipe` — use if the emulator boot-loops
```

**`pnpm build` succeeding is the true signal** — it type-checks against the real SDK
typings and compiles XS bytecode; `pnpm typecheck` alone does not. Prereqs: the SDK
(`pebble sdk install latest`) and `pnpm install`; `nix develop` provides everything else.

To see the result, screenshot the running emulator and Read the PNG:
`pebble screenshot --no-open shot.png`.

### The watch API (Poco drawing + time events)

```ts
import Poco from "commodetto/Poco";
const render = new Poco(screen);
const font = new render.Font("Bitham-Bold", 42); // system fonts: Gothic-*, Bitham-*, Roboto-Condensed, Leco-Regular, Droid-Serif
const fg = render.makeColor(255, 255, 255); // r,g,b 0–255

// minutechange fires immediately on install (no separate startup paint needed);
// secondchange works but drains battery — use it only for a seconds display.
watch.addEventListener("minutechange", (e) => {
  const h = String(e.date.getHours()).padStart(2, "0");
  const m = String(e.date.getMinutes()).padStart(2, "0");
  const t = `${h}:${m}`;
  render.begin();
  render.fillRectangle(render.makeColor(0, 0, 0), 0, 0, render.width, render.height);
  render.drawText(
    t,
    font,
    fg,
    (render.width - render.getTextWidth(t, font)) / 2,
    (render.height - font.height) / 2,
  );
  render.end();
});
```

## Add a new watchface

Copy `watchfaces/simple-time/` to `watchfaces/<name>/`, then: give it a fresh
`pebble.uuid` (`uuidgen | tr 'A-Z' 'a-z'`) and set `name` / `pebble.displayName` /
`description`. Leave everything else. It joins the pnpm workspace automatically.
(`pebble new-project --alloy <name>` also works but emits plain JS and omits the
adaptations below — the template already has them, so copying is simpler.)

## Gotchas (these will bite — all verified in this repo's history)

- **TypeScript 6, not 5.** The current SDK generates a tsconfig with `target`/`lib`
  `es2025`, which only TS ≥ 6 understands. Each watchface pins `typescript@^6`.
  (Older write-ups say "use 5.x, not 6.x" — that predates the `es2025` switch.)
- **`skipLibCheck` is mandatory.** `src/embeddedjs/manifest.json` injects it under
  `platforms.pebble.typescript.tsconfig.compilerOptions`. The SDK's own `global.d.ts`
  imports an unresolvable `web/websocket`; without it, **no** `.ts` watchface compiles.
- **No `"type": "module"`** in a watchface `package.json`. It makes Node treat the
  generated pkjs webpack config (CommonJS) as ESM and the build dies on
  `require is not defined`. The watch code is ESM via `tsconfig` regardless.
- **`main.ts` is type-checked twice** — by the workspace `tsc` (sees
  `types/pebble-alloy.d.ts`) and by Moddable (sees the SDK typings). Only use symbols
  present in both: type event params inline (`(e: { date: Date })`); do **not**
  reference ambient type _names_ the SDK build won't have.
- **pkjs stays `.js`.** `src/pkjs/index.js` (phone side) is bundled as plain
  JavaScript, never TypeScript.
- **Emulator flash corrupts → boot loop.** Fix with `pnpm reset`. A cold boot right
  after a reset can exceed the install connect timeout (`libpebble2.TimeoutError`);
  just retry — the emulator is warm by then.
- **Time / clock-format controls don't reach JS faces.** A face's clock is
  `new Date()`, which on the emulator is host wall-clock. `pebble emu-set-time` and
  12h/24h format are firmware-only and have no effect on Alloy watchfaces (there is no
  clock-format JS API). Screenshots therefore show real time, not a pinned time.
- **Keep the Nix shell as `mkShellNoCC`.** Plain `mkShell` leaks `STRINGS`/`AR`/… into
  Moddable's `make`, which then fails with `No rule to make target 'strings'`.
- **Never grow the XS heap in `mdbl.c` — it's what makes a face "show in the picker
  but not launch".** The XS engine on Pebble runs in a _fixed_ heap (≈32 KB static,
  8 KB chunk by default — see `creation` in Moddable's
  `build/devices/pebble/manifest.json`). Passing an enlarged `.chunk`/`.static` to
  `moddable_createMachine` makes machine creation fail _silently_: the mod is never
  loaded (no `xsHost.c> Found mod` in `pebble logs`), the app shell exits immediately,
  and the watchface appears in the picker but bounces straight back when selected.
  Keep it `moddable_createMachine(NULL)`. (This — not archive size — was the real
  cause of passing-trains' "won't launch"; an earlier write-up blamed a flash cap.)
  Corollary: you cannot upscale into a screen-sized RAM bitmap, and `fillPattern`
  can't stretch — so colour artwork is drawn straight from flash at the size it was
  baked.
- **Colour images: `argb2222` only, drawn with `fillPattern`.** Pebble's blitter
  draws exactly one colour format — native `argb2222` (1 byte/px, 64 colours,
  uncompressed) — and only via `fillPattern`; `drawBitmap` `PBL_ASSERT`s on it and the
  watch **bootloops**. `parseBMP` is no help: it reads only standard `.bmp` and yields
  formats the blitter rejects. Every image is baked uncompressed into the `mc.xsa` mod
  archive, and cost grows with band-height², so colour artwork is budgeted by
  band-height × count — but a ~100 KB mod loads fine (the 1 MB `MAX_RESOURCES_SIZE`
  _is_ a red herring, but the older "~120 KB → bootloop" claim was a misdiagnosis of
  the heap bug above). The Alloy recipe for this — hand-built `.bm4` bitmaps as
  manifest `data`, wrapped in a `Bitmap` and blitted a slice at a time with
  `fillPattern` — is preserved in this repo's git history for `passing-trains`.
- **Need to _scale_ an image, or fill the full screen with colour? You must leave
  Alloy.** Moddable's Poco draws bitmaps **unscaled** (the only `scale()` is for
  vector Pebble Draw Commands), exposes no framebuffer, and you cannot add a custom
  native — the app SDK ships no XS headers and the only Alloy entry point is
  `moddable_createMachine`; the `native("xs_…")` resolver is firmware-sealed. So
  runtime upscaling is impossible in Alloy from every angle. The escape hatch is a
  plain **native C watchface** — see the **Native C watchfaces** section below;
  `watchfaces/passing-trains/` is the repo's one example.
- **Verifying an image face? Use `scripts/emu-check.sh <platform> <dir>`.** It cleans
  up orphaned `qemu`/`pypkjs` (the pile-up that corrupts the flash image), installs
  with cold-boot retries, and reports `RUNNING` / `BOOTLOOP` — far more reliable than
  eyeballing a screenshot that may just be slow to warm up. A timed-out screenshot
  across all retries means a bootloop (an oversized archive, or a hard fault in a
  native-C face); a clean "install an app" face means a _graceful_ JS exception instead.

## Native C watchfaces (the escape hatch)

`watchfaces/passing-trains/` is the repo's one non-Alloy face: plain Pebble C, because
filling the screen with a _scaled_ image needs framebuffer access Alloy can't give (see
the colour-image gotchas). If you need the same, the reusable lessons:

- **Project shape.** Drop `projectType: moddable` / `enableMultiJS` from `package.json`;
  declare art under `pebble.resources.media` (`{type:"bitmap", memoryFormat:"8Bit"}`).
  `src/c/**/*.c` is auto-compiled by the stock `wscript` — no `tsc`, no `typecheck`
  script, pkjs optional. `pebble build` works the same (no Moddable prebuild). Build
  through `pnpm` still, for consistency.
- **Direct pixels.** `graphics_capture_frame_buffer(ctx)` → per row
  `gbitmap_get_data_row_info(fb, y)` whose `min_x`/`max_x` mask the round (gabbro)
  display for free. On colour Pebble a framebuffer byte is `GColor8` = `argb2222`
  (`(a<<6)|(r<<4)|(g<<2)|b`), **identical to an `8Bit` source bitmap**, so blits are a
  raw byte copy and you can scale/composite by hand. `graphics_release_frame_buffer`,
  then draw text/shapes the normal (clip-aware) way.
- **RAM is the budget, not flash.** A `gbitmap` is uncompressed in RAM
  (`width × height` bytes); PNG resources are compressed in flash (~KB) but decoded on
  load (the decode needs a transient buffer). App heap is ~125 KB (emery/gabbro). So
  what caps you is _how many big bitmaps are resident at once_, not how many you ship —
  keep large images small and load lazily. Verify the worst case at runtime with
  `heap_bytes_free()`, and NULL-check every `gbitmap_create_with_resource` (NULL = OOM;
  guard it and draw a fallback rather than dereferencing → hard fault).
- **`tick_timer_service_subscribe` does NOT fire on subscribe** (unlike Alloy's
  `minutechange`); the first tick lands on the next minute boundary, often seconds after
  launch. To hold an initial state for the first minute, skip the first tick.
- **Generated resources.** A Node + ImageMagick `scripts/prepare-images.mjs` bakes the
  art and emits _both_ `package.json`'s `resources.media` and a generated `trains.h`
  (resource ids, display names, per-item tweaks). Don't hand-edit generated files;
  re-run the script. Tune one or two constants at the top (e.g. body height) rather
  than the pipeline.
- **Debugging "shows in the picker but won't launch".** In Alloy it's the XS heap
  (the `mdbl.c` gotcha); in native C it's usually a hard fault — `APP_LOG` + a NULL
  check on each resource load pinpoints it. The emulator's cold-boot flakiness bites
  hardest here: the first install after a wipe (or after switching platforms) routinely
  times out — warm the emulator with one install, wait, then retry in a loop.

## Other tasks

- `pnpm screenshots` (root) — capture emulator PNGs into `watchfaces/*/screenshots/`
  (committed; the site embeds them). `pnpm build:site` — regenerate `_site/`. A
  top-level `appStoreUrl` in a face's `package.json` renders a **Pebble appstore**
  button on its site card (set it once the face is published).
- `pnpm lint` / `pnpm format` / `pnpm typecheck` (root, across the workspace).
- Committed: source and `watchfaces/*/screenshots/*.png`. Gitignored: `build/`,
  `*.pbw`, `_site/`, `package-lock.json`.
- **Binary resources use Git LFS** (`*.png` and friends, via `.gitattributes`). A fresh
  clone needs `git lfs install && git lfs pull` to fetch real bytes (otherwise you get
  pointer files); `nix develop` provides `git-lfs`.

## Before calling it done

Run `pnpm build` in the watchface (it must produce a `.pbw`) — not just `pnpm
typecheck`. Then `pnpm lint` and `pnpm format` at the root.
