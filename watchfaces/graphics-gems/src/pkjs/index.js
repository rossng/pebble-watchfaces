// Phone-side (PebbleKit JS) code: it serves the settings page and forwards the
// chosen values to the watch over AppMessage.
//
// Rather than pull in the Clay dependency, the config page is a small
// self-contained HTML document handed to the phone app as a data: URI. The same
// round-trip Clay does — show a form, read the values back on close — just
// without the extra package in the workspace. Bundled as plain JavaScript (see
// wscript), so this is not TypeScript.

var MODELS = ["Teapot", "Bunny", "Icosphere", "Torus knot"];

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
    TRANSLUCENT_TEXT: s.TRANSLUCENT_TEXT != null ? s.TRANSLUCENT_TEXT : 1,
    TEXT_LAYOUT: s.TEXT_LAYOUT != null ? s.TEXT_LAYOUT : 0,
    JUMBLE: s.JUMBLE != null ? s.JUMBLE : 0,
  };
}

function opt(value, label, current) {
  return (
    '<option value="' +
    value +
    '"' +
    (value === current ? " selected" : "") +
    ">" +
    label +
    "</option>"
  );
}

function configPage(s) {
  var modelOpts = opt(255, "Cycle (change hourly)", s.MODEL_SEL);
  for (var i = 0; i < MODELS.length; i++) modelOpts += opt(i, MODELS[i], s.MODEL_SEL);

  var html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>" +
    "<meta name='viewport' content='width=device-width,initial-scale=1'>" +
    "<title>Graphics Gems</title><style>" +
    "body{font-family:-apple-system,Helvetica,Arial,sans-serif;margin:0;background:#111;color:#eee}" +
    "header{background:#000;padding:22px 18px;font-size:22px;font-weight:600;border-bottom:1px solid #333}" +
    ".row{padding:16px 18px;border-bottom:1px solid #222}" +
    "label{display:block;font-size:13px;color:#9ad;text-transform:uppercase;letter-spacing:.05em;margin-bottom:8px}" +
    "select{width:100%;padding:12px;font-size:17px;background:#1c1c1c;color:#fff;border:1px solid #333;border-radius:8px}" +
    ".tog{display:flex;align-items:center;justify-content:space-between}" +
    ".tog label{margin:0}" +
    "input[type=checkbox]{width:26px;height:26px}" +
    "button{width:100%;padding:16px;font-size:18px;font-weight:600;background:#2a7;color:#000;border:0;border-radius:10px;margin:18px 0}" +
    ".wrap{padding:0 18px}" +
    "</style></head><body>" +
    "<header>Graphics Gems</header>" +
    "<div class='row'><label>Render mode</label><select id='render'>" +
    opt(0, "Wireframe", s.RENDER_MODE) +
    opt(1, "Unlit (solid)", s.RENDER_MODE) +
    opt(2, "Lambert (diffuse)", s.RENDER_MODE) +
    opt(3, "Phong (specular)", s.RENDER_MODE) +
    "</select></div>" +
    "<div class='row'><label>Rotation</label><select id='rotation'>" +
    opt(0, "Continuous tumble", s.ROTATION_MODE) +
    opt(1, "Spin on tap / shake", s.ROTATION_MODE) +
    "</select></div>" +
    "<div class='row'><label>Model</label><select id='model'>" +
    modelOpts +
    "</select></div>" +
    "<div class='row'><label>Time layout</label><select id='layout'>" +
    opt(0, "HH:MM (wide)", s.TEXT_LAYOUT) +
    opt(1, "Stacked (largest)", s.TEXT_LAYOUT) +
    "</select></div>" +
    "<div class='row tog'><label>Translucent time</label>" +
    "<input type='checkbox' id='translucent'" +
    (s.TRANSLUCENT_TEXT ? " checked" : "") +
    "></div>" +
    "<div class='row tog'><label>Jumble on shake</label>" +
    "<input type='checkbox' id='jumble'" +
    (s.JUMBLE ? " checked" : "") +
    "></div>" +
    "<div class='wrap'><button id='save'>Save</button></div>" +
    "<script>" +
    "document.getElementById('save').addEventListener('click',function(){" +
    "var o={RENDER_MODE:+document.getElementById('render').value," +
    "ROTATION_MODE:+document.getElementById('rotation').value," +
    "MODEL_SEL:+document.getElementById('model').value," +
    "TEXT_LAYOUT:+document.getElementById('layout').value," +
    "TRANSLUCENT_TEXT:document.getElementById('translucent').checked?1:0," +
    "JUMBLE:document.getElementById('jumble').checked?1:0};" +
    "location.href='pebblejs://close#'+encodeURIComponent(JSON.stringify(o));" +
    "});" +
    "</script></body></html>";
  return "data:text/html;charset=utf-8," + encodeURIComponent(html);
}

Pebble.addEventListener("ready", function () {
  console.log("graphics-gems: PebbleKit JS ready");
});

Pebble.addEventListener("showConfiguration", function () {
  Pebble.openURL(configPage(loadSettings()));
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
