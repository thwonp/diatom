# 0041. Send the shader chain itself over SETDISPLAY; the launcher owns the list and its names

- **Status:** Accepted
- **Date:** 2026-10-02
- **Supersedes:** -
- **Superseded by:** -

## Context

- plorpos-gkd.72 brings NextUI's GLSL shaders to the GKD: a Shader row in the
  launcher's in-game menu, picked per system, persisted by the launcher like
  the display mode. The GKD presents through diatom's own GLES pipeline
  (`port/gkd_gl.c`), which already builds a chain of up to three passes all
  or nothing (`gkdgl_set_chain`).
- A list entry is a few passes (file, sampling filter, scale) and a final
  filter. NextUI's presets are such lists; a single shader is a list of one.
- The launcher draws the menu, so it needs the entries' names. It already
  owns per-system settings (`display.<TAG>`) and sends paths to diatom
  (ADR-0016).
- Measured on the GKD (2026-10-02, Advance Wars, mgba): applying a two-pass
  chain mid-game takes up to 89 ms (two compiles); a refused one 1-3 ms.
  RSS settles at +14 MB after the first chains and does not grow over 40
  switches.
- NextUI draws every pass over a quad already in clip space with an identity
  `MVPMatrix`, and its `stock.glsl` ignores the matrix. A quad that needed
  the matrix to land put that shader in one corner of the screen (found by
  this ADR's device check).

## Options considered

### Option A - the launcher sends the chain
`SETDISPLAY shader=<path>:<nearest|linear>:<scale>[,...]  final=<nearest|linear>`;
`shader=none` is None. diatom parses, the port compiles, `DISPLAY` says the
spec back. The list (names, presets, per-shader settings) lives in the
launcher's config. diatom gains no file, no flag and no query.

### Option B - diatom owns a list file, the launcher sends a name
One parser, in diatom. But the launcher then needs a `SHADERS` query to draw
its menu, diatom needs a `--shaders <dir>` flag (a `launch.sh` change), and
two repositories must agree where the file lives. Nothing the user sees is
better for it.

## Decision

Option A. The spec grammar: up to `DIATOM_SHADER_MAX_PASSES` (3) passes
joined by commas, each `path:filter:scale`, split from the right; scale 0 is
the display rect, 1-4 a multiple of the pass's input. `final=` is how the
last copy scales when the last pass has a scale. Either field alone keeps
the other. The shader is the last thing validated and the first applied:
any refusal (`ERROR code=bad_shader`, one line, compile logs flattened)
changes nothing, mode and filter included. The chain stays across RUN, like
the mode. `--shader <spec>` does the same for a one-shot.

Ports get `diatom_port_shader_set`; only the GKD's draws. Desktop and Brick
take None and refuse the rest, which keeps the host's half - grammar,
all-or-nothing, the reply - testable on the desktop (`test/stateplane.py`).

Passes draw as NextUI's do: clip-space quad, identity `MVPMatrix`. The
orientation flip is in `TexCoord` (one strip for a texture target, one for
the screen).

Two more of NextUI's assumptions, found on the device (plorpos-gkd.72.6):

- Every pass is told the FRAME's size (`TextureSize`, `InputSize`), not its
  own input's - NextUI's presets are all `srctype`/`scaletype` source. Told
  its input's, lcd3x after pixellate drew one grid cell per screen pixel and
  showed nothing.
- `mediump` is read as `highp`. The shaders declare their size uniforms
  mediump, which Mali runs as 16-bit floats: `scanline.glsl` overflowed
  (pi x 1600 x 240) and drew black, and above 1024 such a float cannot name
  every pixel. The GKD's output is 1600x1440; NextUI's targets are smaller.

## Consequences

- The launcher holds every entry's settings; changing one is a launcher
  config edit, with no diatom release.
- `DISPLAY` lines grow by the spec (absolute paths, up to ~300 bytes for a
  three-pass chain). The launcher reads `rect=` by name, so the appended
  fields do not disturb it.
- A switch costs a compile each time (up to ~90 ms for two passes, measured);
  it happens in the in-game menu, while the game is paused. Caching compiled
  programs is the fix if that ever shows.
- A scale above 4 is refused; a chain that is still too large for the GPU
  fails as a black picture, not an error.

## Revisit if

- A shader switch is measured over 100 ms on the device (the UI latency
  budget), or the launcher wants to switch shaders while the game runs.
- A port other than the GKD gets a GPU path.
- A shader needs its `#pragma parameter` values set (today: defaults only).
