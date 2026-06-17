# Simple Time

A minimal [Pebble Alloy](https://developer.repebble.com/guides/alloy/) watchface
that displays the current time as centered `HH:MM`.

```
src/
  embeddedjs/
    main.ts        # watch code (TypeScript, runs on the Pebble in Moddable's XS engine)
    manifest.json  # Moddable manifest — modules, TS typings, skipLibCheck
  c/mdbl.c         # native entry point that boots the XS machine (don't edit)
  pkjs/index.js    # phone-side PebbleKit JS (plain JS — bundled as-is)
wscript            # waf build script
package.json       # Pebble manifest (the `pebble` block) + scripts + typescript dep
tsconfig.json      # editor/CI type-checking (extends the workspace base config)
```

Drawn with the Poco renderer: white `HH:MM` centered on black, redrawn on each
`minutechange` (battery-friendly — see `secondchange` only if you show seconds).

From this directory (inside the dev shell, with the SDK installed):

```bash
pnpm build              # pebble build
pnpm emulator           # pebble install --emulator emery
pnpm dev                # build + run in the emulator
pnpm typecheck          # tsc, no emit
```

See the [repository README](../../README.md) for environment setup, the emulator,
and deploying to a physical watch.
