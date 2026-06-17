# AGENTS.md

Monorepo of **Pebble Alloy** watchfaces — TypeScript that runs on the watch in
Moddable's XS engine. pnpm workspaces; a Nix flake pins the toolchain. Human-facing
detail lives in [README.md](README.md); this file is the fast path for agents.

## Map

- `watchfaces/<name>/` — one self-contained Alloy project each; `src/embeddedjs/main.ts`
  is the watch code. **Use `watchfaces/simple-time/` as the template — it is already
  correct.**
- `types/pebble-alloy.d.ts` — ambient editor types (Poco / `watch` / `screen`). The
  _real_ types come from the SDK at build time; these just keep the editor happy.
- `scripts/` — `build-site.mjs` (Pages site), `capture-screenshots.mjs`.
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

## Other tasks

- `pnpm screenshots` (root) — capture emulator PNGs into `watchfaces/*/screenshots/`
  (committed; the site embeds them). `pnpm build:site` — regenerate `_site/`.
- `pnpm lint` / `pnpm format` / `pnpm typecheck` (root, across the workspace).
- Committed: source and `watchfaces/*/screenshots/*.png`. Gitignored: `build/`,
  `*.pbw`, `_site/`, `package-lock.json`.

## Before calling it done

Run `pnpm build` in the watchface (it must produce a `.pbw`) — not just `pnpm
typecheck`. Then `pnpm lint` and `pnpm format` at the root.
