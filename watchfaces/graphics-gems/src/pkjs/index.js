// Phone-side (PebbleKit JS) code: opens the settings page and forwards the
// chosen values to the watch over AppMessage.
//
// The settings page is hosted on the project's GitHub Pages site — the Pebble
// phone app's config webview needs a real https URL (a data: URI just hangs on
// "loading"). Current settings are passed in via the URL hash so the form can
// pre-fill; the page posts the new values back through pebblejs://close. Bundled
// as plain JavaScript (see wscript), so this is not TypeScript.

var CONFIG_URL = "https://www.rossng.eu/pebble-watchfaces/graphics-gems/settings.html";

// Last-saved settings, kept on the phone so the form re-opens pre-filled.
function loadSettings() {
  var s = {};
  try {
    s = JSON.parse(localStorage.getItem("gg_settings") || "{}");
  } catch {
    s = {};
  }
  return {
    RENDER_MODE: s.RENDER_MODE != null ? s.RENDER_MODE : 3,
    ROTATION_MODE: s.ROTATION_MODE != null ? s.ROTATION_MODE : 0,
    MODEL_SEL: s.MODEL_SEL != null ? s.MODEL_SEL : 255,
    TEXT_LAYOUT: s.TEXT_LAYOUT != null ? s.TEXT_LAYOUT : 0,
    TRANSLUCENT_TEXT: s.TRANSLUCENT_TEXT != null ? s.TRANSLUCENT_TEXT : 1,
    JUMBLE: s.JUMBLE != null ? s.JUMBLE : 0,
  };
}

Pebble.addEventListener("ready", function () {
  console.log("graphics-gems: PebbleKit JS ready");
});

Pebble.addEventListener("showConfiguration", function () {
  Pebble.openURL(CONFIG_URL + "#" + encodeURIComponent(JSON.stringify(loadSettings())));
});

Pebble.addEventListener("webviewclosed", function (e) {
  if (!e || !e.response) return;
  var o;
  try {
    o = JSON.parse(decodeURIComponent(e.response));
  } catch {
    return;
  }
  localStorage.setItem("gg_settings", JSON.stringify(o));
  Pebble.sendAppMessage(
    o,
    function () {},
    function () {},
  );
});
