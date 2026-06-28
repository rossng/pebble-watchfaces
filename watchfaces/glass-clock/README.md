# Glass Clock

A native Pebble C watchface that **raytraces the time as chunky 3D glass digits**.
The hours sit over the minutes (`HH` / `MM`) as four 7-segment numbers modelled as
real glass geometry — extruded, chamfered, turned slightly so you see their
thickness — with refraction, reflection, randomised coloured lighting and
cel-shaded outlines.

It renders **progressively**: every minute the picture restarts as grainy noise and
develops toward a coherent image over the following ~60 seconds (Monte-Carlo
accumulation), then the next minute kicks off a fresh one — new colours, lighting,
background pattern and viewing angle each time.

![emery](screenshots/emery.png) ![gabbro](screenshots/gabbro.png)

## How it works

Pebble's Alloy/Poco layer can't do any of this (no framebuffer, unscaled bitmaps
only), so this is a from-scratch software raytracer, à la `graphics-gems`:

- **Geometry** — each digit is a signed distance field: 2D rounded-box segments
  (smin-blended for liquid-glass joints) extruded to a chunky slab, with a runtime
  choice of sharp / bevelled / chamfered edges. The four digits are turned around
  the up axis so the side walls and facets read as 3D.
- **Glass shading** — primary rays sphere-trace to the surface; Fresnel splits each
  hit into a reflection and a two-surface refraction (refract in, march the
  interior, Beer-Lambert tint over the path, refract out). The environment is
  procedural — a dark gradient, a randomised pattern (stripes / checker / weave) and
  a few coloured light blobs — so reflection/refraction are just `env(dir)`, no scene
  geometry to intersect. The glass refracts that pattern, which is what makes it
  read as glass rather than a flat colour.
- **Contrast** — legibility isn't left to the glass colours. A coverage stencil of
  the digits (a cheap primary-ray pass, recomputed each minute) drives the blit:
  digit pixels get a brightness floor, the background is pushed dark. So the time is
  high-contrast from frame 0, whatever the raytrace produces.
- **Cel outlines** — a separate edge pass detects silhouettes and facet creases
  (normal / depth discontinuities) and draws toon lines along them, contrasting with
  whatever's behind (white over dark, black over light).
- **Progressive accumulation** — jittered samples (subpixel AA + glossy roughness)
  accumulate into an internal-resolution buffer that's upscaled and ordered-dithered
  into the 64-colour `argb2222` display. The work is chunked across `app_timer`
  bursts, and the (heavy) full-screen blit is throttled, so the watch stays
  responsive.

There is **no 2D text and no fonts** — the digits _are_ the glass.

## Settings

Configurable from the phone (Pebble app → Glass Clock → Settings):

- **Cel outline** — None / White / Black / Auto-contrast
- **Digit edges** — Sharp / Bevelled / Chamfered
- **3D turn** — Flat-on / Oblique
- **Translucency** — Solid / Medium / High
- **Background pattern** — Off / On
- **Colour mood** — Surprise me / Neon / Jewel

## Build

```
cd watchfaces/glass-clock
pnpm build                       # -> build/glass-clock.pbw
pebble install --emulator emery  # or gabbro (round)
```

Targets the colour platforms **emery** (rectangular) and **gabbro** (round). Colour is
required — there's no mono fallback.
