/**
 * Simple Time — a minimal Pebble Alloy watchface.
 *
 * Draws the current time, centered, as white `HH:MM` on a black background and
 * redraws once a minute. Runs on the watch in the Moddable XS engine.
 */

import Poco from "commodetto/Poco";

// One renderer bound to the display framebuffer, reused for every frame.
const render = new Poco(screen);

// Pebble system fonts and colors are cheap to keep around — build them once.
const timeFont = new render.Font("Bitham-Bold", 42);
const background = render.makeColor(0, 0, 0);
const foreground = render.makeColor(255, 255, 255);

function pad(value: number): string {
  return String(value).padStart(2, "0");
}

function draw(event: { date: Date }): void {
  const now = event.date;
  const time = `${pad(now.getHours())}:${pad(now.getMinutes())}`;

  render.begin();
  render.fillRectangle(background, 0, 0, render.width, render.height);

  const textWidth = render.getTextWidth(time, timeFont);
  render.drawText(
    time,
    timeFont,
    foreground,
    (render.width - textWidth) / 2,
    (render.height - timeFont.height) / 2,
  );

  render.end();
}

// `minutechange` also fires immediately on subscription, so there's no separate
// startup paint to wire up.
watch.addEventListener("minutechange", draw);
