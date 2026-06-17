#!/usr/bin/env node
// Builds the GitHub Pages site into `_site/`.
//
// It is intentionally dependency-free (Node built-ins only): the Pebble SDK and
// emulator can't run in CI, so we don't try to build the watchfaces here — we
// just scan their manifests and present them, and copy the hand-written doc
// pages from `docs/` alongside a generated index.
//
//   node scripts/build-site.mjs
//
// Output: `_site/index.html`, `_site/docs/*.html`, `_site/.nojekyll`.

import { cp, mkdir, readdir, readFile, rm, writeFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const OUT = join(ROOT, "_site");
const REPO = process.env.GITHUB_REPOSITORY ?? "rossng/pebble-watchfaces";
const REPO_URL = `https://github.com/${REPO}`;

const esc = (s = "") =>
  String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");

// ---------------------------------------------------------------- collect data

async function collectWatchfaces() {
  const dir = join(ROOT, "watchfaces");
  if (!existsSync(dir)) return [];
  const entries = await readdir(dir, { withFileTypes: true });
  const faces = [];
  for (const e of entries.sort((a, b) => a.name.localeCompare(b.name))) {
    if (!e.isDirectory()) continue;
    const pkgPath = join(dir, e.name, "package.json");
    if (!existsSync(pkgPath)) continue;
    const pkg = JSON.parse(await readFile(pkgPath, "utf8"));
    const peb = pkg.pebble ?? {};
    // Real emulator screenshots, if `pnpm screenshots` has been run:
    // watchfaces/<slug>/screenshots/<platform>.png
    const shots = {};
    const shotsDir = join(dir, e.name, "screenshots");
    if (existsSync(shotsDir)) {
      for (const f of await readdir(shotsDir)) {
        const m = /^(.+)\.png$/i.exec(f);
        if (m) shots[m[1]] = f;
      }
    }
    faces.push({
      slug: e.name,
      name: peb.displayName ?? pkg.name ?? e.name,
      description: pkg.description ?? "",
      author: pkg.author ?? "",
      uuid: peb.uuid ?? "",
      platforms: peb.targetPlatforms ?? [],
      isWatchface: peb.watchapp?.watchface !== false,
      sdkVersion: peb.sdkVersion,
      shots,
    });
  }
  return faces;
}

async function collectDocs() {
  const dir = join(ROOT, "docs");
  if (!existsSync(dir)) return [];
  const files = (await readdir(dir)).filter((f) => f.endsWith(".html")).sort();
  const docs = [];
  for (const file of files) {
    const html = await readFile(join(dir, file), "utf8");
    const title = html.match(/<title>([\s\S]*?)<\/title>/i)?.[1]?.trim();
    const desc = html
      .match(/<meta\s+name=["']description["']\s+content=["']([\s\S]*?)["']/i)?.[1]
      ?.trim();
    docs.push({ file, title: title ?? file, description: desc ?? "" });
  }
  return docs;
}

// ------------------------------------------------------------------- rendering

const isRoundPlatform = (p) => p === "chalk" || p === "gabbro";

// Prefer a real emulator screenshot; fall back to a stylised SVG placeholder.
function watchPreview(face) {
  const label = esc(face.name);

  // Use a captured screenshot when one exists — pick the first target platform
  // that has one (so a round watch shows its round screen).
  const shotPlatform =
    face.platforms.find((p) => face.shots?.[p]) ?? Object.keys(face.shots ?? {})[0];
  if (shotPlatform && face.shots[shotPlatform]) {
    const round = isRoundPlatform(shotPlatform);
    const src = `shots/${encodeURIComponent(face.slug)}/${esc(face.shots[shotPlatform])}`;
    return `<div class="preview shot ${round ? "round" : "square"}">
      <img src="${src}" alt="${label} on ${esc(shotPlatform)}" loading="lazy" />
    </div>`;
  }

  const round = face.platforms.some(isRoundPlatform);
  const screen = round
    ? `<circle cx="90" cy="90" r="66" fill="#0a0a0a" stroke="#0a0a0a" stroke-width="2"/>`
    : `<rect x="30" y="26" width="120" height="128" rx="6" fill="#0a0a0a" stroke="#0a0a0a" stroke-width="2"/>`;
  const bezel = round
    ? `<circle cx="90" cy="90" r="80" fill="#ff4b00" stroke="#0a0a0a" stroke-width="3"/>`
    : `<rect x="16" y="12" width="148" height="156" rx="20" fill="#ff4b00" stroke="#0a0a0a" stroke-width="3"/>`;
  return `<svg class="preview" viewBox="0 0 180 180" role="img" aria-label="${label} preview">
      ${bezel}
      ${screen}
      <text x="90" y="86" text-anchor="middle" fill="#ffffff" font-size="28" font-family="ui-monospace,Menlo,monospace" font-weight="700">10:09</text>
      <text x="90" y="112" text-anchor="middle" fill="#9aa3b2" font-size="11" font-family="system-ui,sans-serif">${label}</text>
    </svg>`;
}

function faceCard(face) {
  const chips = face.platforms.map((p) => `<span class="chip">${esc(p)}</span>`).join("");
  const kind = face.isWatchface ? "Watchface" : "Watchapp";
  const src = `${REPO_URL}/tree/main/watchfaces/${encodeURIComponent(face.slug)}`;
  return `<article class="card">
      ${watchPreview(face)}
      <div class="card-body">
        <div class="card-head">
          <h3>${esc(face.name)}</h3>
          <span class="kind">${kind}</span>
        </div>
        ${face.description ? `<p class="desc">${esc(face.description)}</p>` : ""}
        <div class="chips">${chips}</div>
        <div class="card-meta">
          ${face.author ? `<span>by ${esc(face.author)}</span>` : ""}
          ${face.uuid ? `<span class="uuid" title="${esc(face.uuid)}">${esc(face.uuid)}</span>` : ""}
        </div>
        <a class="src" href="${src}">View source ↗</a>
      </div>
    </article>`;
}

function docCard(doc) {
  return `<a class="doc" href="docs/${esc(doc.file)}">
      <h3>${esc(doc.title)}</h3>
      ${doc.description ? `<p>${esc(doc.description)}</p>` : ""}
      <span class="go">Read ↗</span>
    </a>`;
}

function page(faces, docs) {
  const faceSection = faces.length
    ? `<div class="grid">${faces.map(faceCard).join("\n")}</div>`
    : `<p class="empty">No watchfaces yet. Add one with <code>pnpm new</code>.</p>`;
  const docSection = docs.length ? `<div class="docs">${docs.map(docCard).join("\n")}</div>` : "";

  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>Pebble Watchfaces</title>
<meta name="description" content="Pebble Alloy watchfaces in TypeScript, plus a field guide to the Pebble ecosystem." />
<link rel="preconnect" href="https://fonts.googleapis.com" />
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin />
<link href="https://fonts.googleapis.com/css2?family=Pixelify+Sans:wght@400;500;600;700&family=Silkscreen:wght@400;700&display=swap" rel="stylesheet" />
<style>
  :root{
    --bg:#f3f2ee; --panel:#ffffff; --panel-2:#f0efe8; --ink:#0b0b0c; --ink-dim:#5c5c61;
    --line:#0b0b0c; --hair:#d9d8d2;
    --core:#ff4b00; --core-soft:#ffe1d3; --watch:#00a99a; --watch-soft:#cdf2ee;
    --shadow:4px 4px 0 var(--ink); --shadow-lg:6px 6px 0 var(--ink);
    --mono:ui-monospace,"SF Mono","JetBrains Mono",Menlo,Consolas,monospace;
    --pixel:"Pixelify Sans",system-ui,sans-serif;
    --label:"Silkscreen","Pixelify Sans",ui-monospace,monospace;
    --sans:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
  }
  *{box-sizing:border-box}
  body{margin:0;font-family:var(--sans);color:var(--ink);line-height:1.5;
    background-color:var(--bg);
    background-image:radial-gradient(var(--hair) 1.3px, transparent 1.3px);
    background-size:18px 18px;background-position:-9px -9px;
    -webkit-font-smoothing:antialiased;}
  a{color:var(--core);text-decoration:none;font-weight:600}
  a:hover{text-decoration:underline}
  .wrap{max-width:1080px;margin:0 auto;padding:0 20px 100px}
  header{padding:60px 20px 10px;max-width:1080px;margin:0 auto}
  .kicker{font-family:var(--label);font-size:11px;letter-spacing:.14em;text-transform:uppercase;color:var(--core);margin:0 0 14px}
  h1{font-family:var(--pixel);font-weight:600;font-size:clamp(38px,7vw,68px);line-height:.98;margin:0 0 16px;letter-spacing:0}
  .lede{max-width:620px;color:var(--ink-dim);font-size:17px;margin:0}
  .lede a{color:var(--ink);text-decoration:underline;text-decoration-thickness:2px;text-underline-offset:2px}
  section{margin:52px 0 0}
  h2{font-family:var(--pixel);font-weight:600;font-size:28px;margin:0 0 6px;display:flex;align-items:center;gap:10px}
  .section-lede{color:var(--ink-dim);margin:0 0 22px;font-size:14.5px;max-width:680px}

  .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:18px}
  .card{display:flex;gap:14px;border:2px solid var(--line);border-radius:12px;background:var(--panel);padding:14px;
    box-shadow:var(--shadow);transition:transform .1s ease,box-shadow .1s ease}
  .card:hover{transform:translate(-2px,-2px);box-shadow:var(--shadow-lg)}
  .preview{width:104px;height:104px;flex:0 0 104px}
  .preview.shot{display:flex;align-items:center;justify-content:center;overflow:hidden;
    background:#000;border:2px solid var(--line)}
  .preview.shot.square{border-radius:10px}
  .preview.shot.round{border-radius:50%}
  .preview.shot img{width:100%;height:100%;object-fit:contain;image-rendering:pixelated}
  .preview.shot.round img{border-radius:50%}
  .card-body{min-width:0;flex:1}
  .card-head{display:flex;align-items:center;gap:8px;justify-content:space-between}
  .card-head h3{margin:0;font-family:var(--pixel);font-weight:600;font-size:18px}
  .kind{font-family:var(--label);font-size:9px;text-transform:uppercase;letter-spacing:.04em;color:var(--ink);
    background:var(--watch);border:2px solid var(--line);border-radius:6px;padding:3px 6px;white-space:nowrap}
  .desc{margin:6px 0 8px;font-size:13px;color:var(--ink-dim)}
  .chips{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:8px}
  .chip{font-family:var(--mono);font-size:10.5px;font-weight:600;color:var(--ink);background:var(--core-soft);
    border:1.5px solid var(--line);border-radius:6px;padding:1px 7px}
  .card-meta{display:flex;flex-direction:column;gap:2px;font-size:11px;color:var(--ink-dim);margin-bottom:8px}
  .uuid{font-family:var(--mono);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
  .src{font-family:var(--mono);font-size:12px;color:var(--core)}
  .empty{color:var(--ink-dim)}
  .empty code{font-family:var(--mono);background:var(--panel);border:1.5px solid var(--line);padding:1px 6px;border-radius:5px}

  .docs{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:16px}
  .doc{border:2px solid var(--line);border-radius:12px;background:var(--panel);box-shadow:var(--shadow);
    padding:16px 18px;display:block;color:var(--ink);transition:transform .1s,box-shadow .1s}
  .doc:hover{transform:translate(-2px,-2px);box-shadow:var(--shadow-lg);text-decoration:none}
  .doc h3{margin:0 0 6px;font-family:var(--pixel);font-weight:600;font-size:18px}
  .doc p{margin:0 0 10px;font-size:13px;color:var(--ink-dim)}
  .doc .go{font-family:var(--label);font-size:10px;text-transform:uppercase;letter-spacing:.05em;color:var(--core)}

  footer{color:var(--ink-dim);font-size:12.5px;border-top:2px solid var(--line);padding-top:18px;margin-top:60px}
  footer code{font-family:var(--mono)}
</style>
</head>
<body>
<header>
  <p class="kicker">Pebble · Alloy · TypeScript</p>
  <h1>Pebble Watchfaces</h1>
  <p class="lede">TypeScript watchfaces for the revived <a href="https://repebble.com">Pebble</a>, built on <a href="https://developer.repebble.com/guides/alloy/">Alloy</a> — plus a field guide to the ecosystem.</p>
</header>
<div class="wrap">
  <section>
    <h2>⌚ Watchfaces</h2>
    <p class="section-lede">One Alloy project each, under <code style="font-family:var(--mono)">watchfaces/</code>.</p>
    ${faceSection}
  </section>
  ${
    docSection
      ? `<section>
    <h2>📚 Guides</h2>
    <p class="section-lede">Reference pages from <code style="font-family:var(--mono)">docs/</code>.</p>
    ${docSection}
  </section>`
      : ""
  }
  <footer>
    Built from <a href="${REPO_URL}">${esc(REPO)}</a> · deployed to GitHub Pages on every push to <code>main</code>.
  </footer>
</div>
</body>
</html>
`;
}

// ----------------------------------------------------------------------- build

async function main() {
  const [faces, docs] = await Promise.all([collectWatchfaces(), collectDocs()]);

  await rm(OUT, { recursive: true, force: true });
  await mkdir(OUT, { recursive: true });

  await writeFile(join(OUT, "index.html"), page(faces, docs));
  await writeFile(join(OUT, ".nojekyll"), "");

  if (docs.length) {
    await cp(join(ROOT, "docs"), join(OUT, "docs"), { recursive: true });
  }

  // Copy each watchface's screenshots into _site/shots/<slug>/.
  let shotCount = 0;
  for (const face of faces) {
    const names = Object.keys(face.shots);
    if (!names.length) continue;
    await cp(join(ROOT, "watchfaces", face.slug, "screenshots"), join(OUT, "shots", face.slug), {
      recursive: true,
    });
    shotCount += names.length;
  }

  console.log(
    `Built _site/ — ${faces.length} watchface(s), ${docs.length} doc page(s), ${shotCount} screenshot(s).`,
  );
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
