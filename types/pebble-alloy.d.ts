// Minimal ambient declarations for the Pebble Alloy embeddedjs runtime.
//
// The authoritative type definitions ship with the Moddable SDK (under
// `$MODDABLE/typings`) and are wired in automatically by `pebble build` via the
// `manifest_typings.json` include in each watchface's `src/embeddedjs/manifest.json`.
// These local declarations cover just the surface the example watchfaces use so
// the workspace type-checks in your editor without the full SDK installed.
//
// This file is intentionally a script (no top-level import/export) so its
// declarations are ambient/global.

/** An opaque color value produced by `Poco.makeColor`, packed for the display. */
type PocoColor = number;

interface PocoFont {
  /** Line height of the font in pixels. */
  readonly height: number;
}

/**
 * Commodetto's Poco renderer, bound to a pixel buffer. On Pebble the buffer is
 * the global `screen`. See https://developer.repebble.com/guides/alloy/poco-guide/
 */
declare class Poco {
  constructor(pixels: unknown);

  /** Display width in pixels. */
  readonly width: number;
  /** Display height in pixels. */
  readonly height: number;

  /** Construct a Pebble system font by name and size, e.g. `new poco.Font("Bitham-Bold", 42)`. */
  Font: new (name: string, size: number) => PocoFont;

  /** Pack an 8-bit-per-channel RGB triple into a display color. */
  makeColor(r: number, g: number, b: number): PocoColor;

  /** Begin a drawing pass, optionally clipped to a rectangle. */
  begin(x?: number, y?: number, width?: number, height?: number): void;
  /** End the drawing pass and flush to the display. */
  end(): void;

  fillRectangle(color: PocoColor, x: number, y: number, width: number, height: number): void;
  drawText(text: string, font: PocoFont, color: PocoColor, x: number, y: number): void;
  getTextWidth(text: string, font: PocoFont): number;
}

declare module "commodetto/Poco" {
  export default Poco;
}

/** The watch's framebuffer, passed to `new Poco(screen)`. */
declare const screen: unknown;

/** Time-tick events delivered to `watch` listeners. */
interface WatchTimeEvent {
  /** The current local time at the moment the event fired. */
  readonly date: Date;
}

interface Watch {
  /**
   * Subscribe to a time tick. The listener fires immediately on subscription,
   * so the watchface paints as soon as it launches. Prefer `"minutechange"`
   * unless you render seconds — `"secondchange"` is far more power-hungry.
   */
  addEventListener(
    type: "minutechange" | "secondchange" | "daychange",
    listener: (event: WatchTimeEvent) => void,
  ): void;
  removeEventListener(
    type: "minutechange" | "secondchange" | "daychange",
    listener: (event: WatchTimeEvent) => void,
  ): void;
}

/** The global Pebble watch object. */
declare const watch: Watch;
