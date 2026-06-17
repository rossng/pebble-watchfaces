// Phone-side (PebbleKit JS) code. It runs on the paired phone, not the watch,
// and is where networking and location would live. This watchface is fully
// self-contained, so there's nothing to do here beyond acknowledging startup.
//
// Note: this file is bundled as plain JavaScript by the build (see wscript), so
// unlike the watch code in ../embeddedjs it is not TypeScript.
Pebble.addEventListener("ready", function () {
  console.log("simple-time: PebbleKit JS ready");
});
