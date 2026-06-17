#!/usr/bin/env node
// A tiny keyboard-driven control panel for a running Pebble emulator.
//
// It captures single keypresses and shells out to the matching `pebble emu-*`
// command, targeting an already-booted emulator. `pnpm dev` boots the emulator
// (via `pebble install --emulator …`) and then launches this so you can drive
// the watch — buttons, accel taps, screenshots — without juggling a second terminal.
import { spawn } from "node:child_process";
import readline from "node:readline";
import process from "node:process";

const argv = process.argv.slice(2);
const flag = (name) => {
  const i = argv.indexOf(name);
  return i >= 0 ? argv[i + 1] : undefined;
};
const emulator = flag("--emulator") ?? process.env.PEBBLE_EMULATOR ?? "emery";

const out = process.stdout;
const C = { dim: "\x1b[2m", red: "\x1b[31m", green: "\x1b[32m", bold: "\x1b[1m", reset: "\x1b[0m" };

const MAX_LOG = 10;
const log = [];
let running = 0;

function note(text, ok = true) {
  const stamp = new Date().toTimeString().slice(0, 8);
  log.push({ stamp, text, ok });
  while (log.length > MAX_LOG) log.shift();
  render();
}

// Run a pebble subcommand against the running emulator, logging its result.
function pebble(label, ...cmd) {
  running++;
  render();
  const child = spawn("pebble", [...cmd, "--emulator", emulator], {
    stdio: ["ignore", "ignore", "pipe"],
  });
  let err = "";
  child.stderr.on("data", (d) => (err += d));
  child.on("error", (e) => {
    running--;
    note(`${label} — could not run pebble: ${e.message}`, false);
  });
  child.on("close", (code) => {
    running--;
    if (code === 0) note(`${label}`, true);
    else note(`${label} — exit ${code} ${err.trim().split("\n").pop() ?? ""}`.trim(), false);
  });
}

function quit() {
  out.write("\x1b[2J\x1b[H\x1b[?25h");
  if (process.stdin.isTTY) process.stdin.setRawMode(false);
  process.exit(0);
}

// Each action lists the keys that fire it (matched against key.name or the
// raw character) plus the hint shown on screen.
const ACTIONS = [
  {
    hint: "↑ k",
    keys: ["up", "k"],
    label: "Up button",
    run: () => pebble("up", "emu-button", "click", "up"),
  },
  {
    hint: "↓ j",
    keys: ["down", "j"],
    label: "Down button",
    run: () => pebble("down", "emu-button", "click", "down"),
  },
  {
    hint: "⏎ s",
    keys: ["return", "s"],
    label: "Select button",
    run: () => pebble("select", "emu-button", "click", "select"),
  },
  {
    hint: "⌫ b",
    keys: ["backspace", "delete", "b"],
    label: "Back button",
    run: () => pebble("back", "emu-button", "click", "back"),
  },
  {
    hint: "a",
    keys: ["a"],
    label: "Accelerometer tap",
    run: () => pebble("tap", "emu-tap", "--direction", "z+"),
  },
  {
    hint: "p",
    keys: ["p"],
    label: "Screenshot (→ cwd)",
    run: () => pebble("screenshot", "screenshot", "--no-open"),
  },
  { hint: "q", keys: ["q"], label: "Quit", run: quit },
];

function render() {
  const rows = [];
  const status = running ? `${C.dim}· ${running} running${C.reset}` : "";
  rows.push(
    `  ${C.bold}Pebble emulator control${C.reset}  ${C.dim}[${emulator}]${C.reset}  ${status}`,
  );
  rows.push("");
  for (const a of ACTIONS) rows.push(`   ${C.bold}${a.hint.padEnd(5)}${C.reset} ${a.label}`);
  rows.push("");
  rows.push(`   ${C.dim}─ activity ─${C.reset}`);
  for (const m of log) {
    const mark = m.ok ? `${C.green}✓${C.reset}` : `${C.red}✗${C.reset}`;
    const text = m.ok ? m.text : `${C.red}${m.text}${C.reset}`;
    rows.push(`   ${C.dim}${m.stamp}${C.reset} ${mark} ${text}`);
  }
  out.write("\x1b[2J\x1b[H" + rows.join("\n") + "\n");
}

function onKey(str, key) {
  key = key ?? {};
  if (key.ctrl && key.name === "c") return quit();

  for (const a of ACTIONS) {
    if (a.keys.includes(key.name) || a.keys.includes(str)) {
      a.run();
      return;
    }
  }
}

if (!process.stdin.isTTY) {
  console.error("emu-tui needs an interactive terminal (TTY).");
  process.exit(1);
}
readline.emitKeypressEvents(process.stdin);
process.stdin.setRawMode(true);
process.stdin.resume();
process.stdin.on("keypress", onKey);
out.write("\x1b[?25l"); // hide cursor
process.on("exit", () => out.write("\x1b[?25h"));
note(`ready — controlling “${emulator}” emulator`);
