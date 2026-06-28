// Phone-side (PebbleKit JS): opens the settings page and forwards the chosen
// values to the watch over AppMessage.
//
// The settings page is hosted on the project's GitHub Pages site — the Pebble
// phone app's config webview needs a real https URL (a data: URI just hangs on
// "loading"). Current settings are passed in via the URL hash so the form can
// pre-fill; the page posts the new values back through pebblejs://close.
//
// Bundled by the SDK's (old) webpack/acorn via enableMultiJS, so keep this to
// ES5 (e.g. `catch (e)`, not the ES2019 optional catch binding).

/* eslint-disable no-unused-vars */

var CONFIG_URL = "https://www.rossng.eu/pebble-watchfaces/glass-clock/settings.html";

// Last-saved settings, kept on the phone so the form re-opens pre-filled.
function loadSettings() {
  var s = {};
  try {
    s = JSON.parse(localStorage.getItem("glassclock_settings") || "{}");
  } catch (e) {
    s = {};
  }
  return {
    CEL_MODE: s.CEL_MODE != null ? s.CEL_MODE : 3,
    EDGE_STYLE: s.EDGE_STYLE != null ? s.EDGE_STYLE : 2,
    TURN: s.TURN != null ? s.TURN : 1,
    PATTERN: s.PATTERN != null ? s.PATTERN : 1,
    MOOD: s.MOOD != null ? s.MOOD : 0,
    TRANSLUCENCY: s.TRANSLUCENCY != null ? s.TRANSLUCENCY : 1,
  };
}

Pebble.addEventListener("ready", function () {
  console.log("glass-clock: PebbleKit JS ready");
});

Pebble.addEventListener("showConfiguration", function () {
  Pebble.openURL(CONFIG_URL + "#" + encodeURIComponent(JSON.stringify(loadSettings())));
});

Pebble.addEventListener("webviewclosed", function (e) {
  if (!e || !e.response) return;
  var o;
  try {
    o = JSON.parse(decodeURIComponent(e.response));
  } catch (err) {
    return;
  }
  localStorage.setItem("glassclock_settings", JSON.stringify(o));
  Pebble.sendAppMessage(o);
});
