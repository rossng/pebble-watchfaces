# emu-tui

A tiny, dependency-free keyboard control panel for a **running** Pebble
emulator. It captures single keypresses and shells out to the matching
`pebble emu-*` command, so you can drive the watch without a second terminal.

It does **not** boot an emulator — point it at one that is already running
(that's what `pebble install --emulator …` does). The watchface `pnpm dev`
script wires this up for you: build → install (boots the emulator) → `emu-tui`.

## Usage

```bash
emu-tui                    # targets the "emery" emulator
emu-tui --emulator gabbro  # or set PEBBLE_EMULATOR
```

## Keys

| Key       | Action                                |
| --------- | ------------------------------------- |
| `↑` / `k` | Up button                             |
| `↓` / `j` | Down button                           |
| `⏎` / `s` | Select button                         |
| `⌫` / `b` | Back button                           |
| `a`       | Accelerometer tap                     |
| `p`       | Screenshot (saved to the current dir) |
| `q`       | Quit                                  |

Each keypress runs the corresponding `pebble` subcommand against the target
emulator and reports success/failure in the activity log.

> Set-time and 12h/24h-format controls were removed: `pebble emu-set-time` and
> `emu-time-format` only reach the firmware's native rendering, but Alloy/JS
> watchfaces read `new Date()` (host wall-clock on the emulator) and have no API
> for the clock-format preference — so neither had any visible effect here.
