# Graphics Gems

A watchface that renders the classics of computer-graphics research — the **Utah
teapot**, the **Stanford bunny**, an **icosphere** and a **(2,3) torus knot** —
with a from-scratch software rasterizer, the time floating front-and-centre in
the 3D scene.

|          emery (rect)           |          gabbro (round)           |
| :-----------------------------: | :-------------------------------: |
| ![emery](screenshots/emery.png) | ![gabbro](screenshots/gabbro.png) |

This is a **native C** face, not Alloy. Alloy's Poco renderer has no framebuffer
access, so 3D is impossible there (the same reason `passing-trains` is native C —
see the colour-image notes in the repo's [AGENTS.md](../../AGENTS.md)). Here we
capture the framebuffer and write `argb2222` bytes directly.

## What it does

- **The time floats in the scene.** Big white digits sit on a plane just in
  front of the model's centre, inserted into the same back-to-front draw order as
  the triangles — so the frontmost slice of the model draws _over_ them and weaves
  through. How far each model sits behind that plane is tuned per model
  (`MODEL_TEXT_FRONT` in `main.c`, picked from each model's reach distribution —
  see the `stats` mode of `scripts/preview-orient.mjs`) so every model z-fights
  the digits a little as it turns. Lay the time out near-full-width `HH:MM` or
  stack `HH` over `MM` for even larger digits; it's white and slightly
  translucent (a screen-door peeks the model back through ~25% of the glyphs).
- **Big models.** Each model is sized per-model (`MODEL_FILL`) so the teapot,
  bunny and knot spill past the screen edges in some orientations, while the
  near-spherical icosphere stays fully visible.
- **A travelling colour gradient.** The mesh is painted with a two-colour
  gradient indexed along a fixed model-space axis; the bands travel around the
  mesh and the two anchor hues drift over time, so the colours keep changing.
  Lighting modulates it per face.
- **Tumble + shake.** In _continuous_ mode it turns at a gentle constant speed
  about a slowly-wandering axis. A tap/shake spins it up ~5× in a fresh random
  direction, then eases back to normal. In _tap_ (static) mode it sits still — no
  animation, to save battery — until a shake spins it, after which it settles back
  to a standstill in a new orientation. Each model starts in a flattering pose
  (`MODEL_START`, chosen with `scripts/preview-orient.mjs`).
- **Jumble on shake** (optional). When enabled, every shake also reshuffles the
  model, render mode and gradient colours.

## Settings (phone config page)

Open the watchface's settings in the Pebble phone app (the app must declare the
`configurable` capability in `package.json` for the gear to appear). The form is
a static page — `settings.html` — hosted on the project's GitHub Pages site;
`src/pkjs/index.js` points `Pebble.openURL` at it, passing the current settings
in the URL hash so it pre-fills, and reads the new values back on close. It's
hosted rather than served as a `data:` URI because the phone app's config webview
won't load `data:` URIs (it just hangs on "loading"). Values are sent over
AppMessage and persisted on the watch. The site build copies each face's
`settings.html` to `_site/<slug>/settings.html`.

| Setting          | Options                                                  |
| ---------------- | -------------------------------------------------------- |
| Render mode      | Wireframe · Unlit · Lambert · Phong                      |
| Rotation         | Continuous tumble · Spin on tap/shake                    |
| Model            | Cycle (hourly) · Teapot · Bunny · Icosphere · Torus knot |
| Time layout      | HH:MM (wide) · Stacked (largest)                         |
| Translucent time | on / off                                                 |
| Jumble on shake  | on / off — a shake reshuffles model / mode / colours     |

## How the renderer works (`src/c/render.c`)

A textbook fixed-function pipeline, sized to a watch:

1. **Transform + project.** Each vertex is rotated, pushed to `z = -CAM_DIST`,
   and perspective-projected. Vertices are baked as `int16` fixed-point.
2. **Cull + colour + sort.** Back faces are dropped by the sign of the view-space
   normal · view direction. Each survivor gets its gradient colour (from its
   model-space centroid along the gradient axis, offset by the travelling phase)
   modulated by **per-face (flat)** lighting — Lambert diffuse plus, in Phong
   mode, a Blinn-Phong specular highlight. They're then depth-sorted
   (**painter's algorithm** — there's no room for a full z-buffer in the ~125 KB
   app heap, and it gives the text-piercing for free).
3. **Fill.** Triangles are scanline-filled straight into the captured
   framebuffer, clipped per row to the (possibly round) display via each row's
   `min_x`/`max_x`.

**Dithered colour.** The displays only show `argb2222` (64 colours, 4 levels per
channel), so a flat-quantised tint turns into rainbow speckle. Each channel is
instead **ordered-dithered** (4×4 Bayer) between its two nearest levels, so the
gradient reads smoothly.

**No libm in the render path** (`src/c/fastmath.h`). emery/gabbro are soft-float
ARMv7-M (no FPU), and the render runs deep in the system's call stack — where
libm's `sqrtf`/`cosf`/`powf` overflow the small app stack and hard-fault inside
`__ieee754_*`. So all of it is inlined and call-free: a fast inverse sqrt, a
Bhaskara `sin`/`cos`, integer-power specular, a triangle-wave gradient. That both
fixes the fault and keeps each frame cheap enough for the FPU-less core (~12 fps;
the heaviest model, the knot, is ~75 ms/frame).

Honest limits of the platform: Phong is per-_face_, not per-pixel — at this
triangle budget, flat shading with ordered dithering is the right trade, and it
leaves a clean path to per-pixel shading later. And `graphics_draw_text` has no
alpha, so the time's translucency is a screen-door (re-rasterising the faces
behind the text on a sparse checker) rather than true blending.

## Models (`scripts/bake-models.mjs` → `src/c/models.h`)

`pnpm bake:models` regenerates the model header. The teapot and bunny are loaded
from vendored OBJs (`models/`, via Git LFS) and **decimated by vertex
clustering** to a few-hundred-triangle budget; the icosphere and torus knot are
generated procedurally. Every model is recentred and scaled to a unit bounding
sphere. The arrays are `const`, so they live in flash — but note Pebble loads the
whole app image into RAM, so the static footprint still has to clear the 16-bit
virtual-size limit (~64 KB); that's why vertices are `int16`, not `float`. Don't
hand-edit `models.h`; tweak the budgets at the top of the script and re-run.

The big time uses a bundled geometric font: **Poppins ExtraBold** (OFL),
rasterised by the build at two sizes (`resources/fonts/`, via Git LFS), with a
`characterRegex` that keeps only the digits and colon so the resources stay tiny.

Model provenance: the Utah teapot (Martin Newell, 1975) and the Stanford bunny
(Stanford 3D Scanning Repository, Greg Turk & Marc Levoy, 1994) are long-standing
public graphics-research test models, here decimated from the OBJs in
[alecjacobson/common-3d-test-models](https://github.com/alecjacobson/common-3d-test-models).

## Build / test

```
cd watchfaces/graphics-gems
pnpm bake:models   # regenerate src/c/models.h (only when changing models)
pnpm build         # native C → build/graphics-gems.pbw (emery + gabbro)
pnpm dev           # build + boot emulator + emu-tui
```

There's no `tsc`/`typecheck` — `src/c/**/*.c` is compiled by the stock `wscript`.
Verify a build with `scripts/emu-check.sh emery watchfaces/graphics-gems` (the
round `gabbro` emulator is flakier to cold-boot; warm it with one install and
retry).
