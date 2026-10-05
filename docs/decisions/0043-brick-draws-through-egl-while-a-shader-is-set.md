# 0043. The Brick draws through an EGL window while a shader is set, and through fbdev otherwise

- **Status:** Accepted
- **Date:** 2026-10-05
- **Supersedes:** - (amends ADR-0013's "diatom presents via fbdev" and ADR-0041's "Desktop and Brick take None")
- **Superseded by:** -

## Context

- plorpos-reo.4 brings the GKD's shaders (ADR-0041) to the plain Brick. The
  user's condition: no added latency or cost with shaders off.
- ADR-0013: diatom presents via fbdev with a flip thread; the launcher's mali
  EGL window stays alive, idle, while resident diatom presents. The
  display-handoff spike (`docs/spikes/2026-08-24-display-handoff.md`) found
  that only CONCURRENT presentation wedges the panel (`FBIOPAN_DISPLAY`
  in-kernel, a power cycle); a sequential fbdev -> EGL handoff is fine, even
  with fb0 still mapped.
- Measured, option A (EGL pbuffer + `glReadPixels` into an fbdev page):
  readback of 1024x768 costs 9.6-11.2 ms a frame (~38 ms through PBOs);
  ready + readback 15-21 ms for the light chains. Not viable.
- Measured, the switch (`tools/glswitch`, clean run, plorpos-reo.4.1): the
  window comes up in 99-159 ms and goes down in 104-181 ms; GL holds 60.0 fps
  (swap ~1.0-1.3 ms); two EGL windows in two processes taking turns, with
  the idle one alive, do not wedge. Window creation pans fb0 pages 0 then 1
  on its own, before any draw.
- Measured, real cores (plorpos-reo.4.3): None A/B against v1.2's diatom,
  same state - Genesis 49.30 vs 49.37 fps, PS 59.10 vs 58.97, Arcade 59.42
  vs 59.41. All fifteen of the GKD's chains run at 98.2-99.8% of target on
  GBC, GBA, SNES, Genesis, PS and Arcade, and a probe counting fb0 pans
  shows no frame dropped (Real LCD, Fast Sharpen, PT + LCD 3x: same flip
  rate as None on PS and 60 Hz Genesis).
- `tools/shaderbench` on the Brick says five of those chains miss nearly
  every frame. Its source is noise, which this GPU cannot compress; game
  frames can. On the Brick, real cores are the test, not the bench.
- The in-game Shader pick sends `SETDISPLAY` while the launcher's menu is on
  glass, and the launcher waits for the reply.

## Options considered

### Option A - EGL pbuffer, read back into fbdev
Keeps ADR-0013's display ownership untouched: the GPU never presents. Costs
10+ ms of readback every frame (measured above). Rejected.

### Option B - an EGL window for every frame, shader or not
One present path. But None would pay GL's per-frame cost and lose the flip
thread's timing, which breaks the user's condition. Rejected.

### Option C - fbdev for None, an EGL window only while a chain is set
None keeps today's path, byte for byte. A chain costs one switch (~100-180
ms) when it is picked or a game with one starts. Two display paths in one
port.

### Option D - compile in `shader_set`, as the GKD does
Errors would reach the launcher as `ERROR code=bad_shader`. But `shader_set`
runs while the launcher's menu is on glass: bringing a window up then would
make diatom a second presenter, the wedge class. Rejected.

## Decision

Option C, with the switch done at the next present:

- `diatom_port_shader_set` only records the chain (and checks each file is
  readable). `shader_reconcile()`, at the top of the next
  `diatom_port_present`, brings the window up (flip queue drained first),
  sets the chain, or takes the window down for None. There is never a
  moment with two presenters from diatom.
- A chain that fails to compile falls back to None, with one log line
  (`shader: <error>; drawing None`). The launcher sees the chain accepted.
- The window stays up across pause: `diatom_port_present_stop` finishes the
  GPU's work, waits 34 ms for the last flip to land, re-reads the front page
  from `yoffset`, then parks as before. Tearing it down at every pause would
  add ~150 ms to menu open and ~130 ms to Continue (the 100 ms budget).
- The flip thread stays alive, idle, while GL draws.
- The launcher sends the shader before `RUN` (beside `SETQUIET`), so the
  first frame has it. Sent after, `RUN`'s three warmup frames drew the old
  state and a relaunch flashed black.
- `gkdgl_shutdown()` forgets its texture sizes. A second window in one
  process otherwise sampled a never-allocated texture: black.

## Consequences

- None costs nothing new (A/B above). A chain costs GL's per-frame upload
  and passes, measured within budget on every system tested.
- On the Brick a compile error is a log line, not an `ERROR` reply. A bad
  file still fails in `shader_set` (readability).
- Screenshots with a chain read the GL frame back (`gkdgl_read`), shader
  included; notices and the level bar draw over GL.
- Display experiments on the Brick go through `tools/brick-run.sh`, which
  now freezes every supervisor pid and refuses unless the freeze held. It
  once froze nothing (two `launch.sh` pids), and three spike runs were
  two-presenter runs that nobody saw.
- The 34 ms in the pause path is a measured guess, proven over 5+
  pause/resume cycles, not under heavy GPU load.

## Revisit if

- A pause/resume with a chain shows a stale or torn frame (the 34 ms wait).
- A game whose shader is None starts after one with a chain and the ~100-180
  ms window teardown before its first frame is noticed (the window stays up
  between games today).
- A chain is measured below 92.5% of target on a real core; then the Brick
  list can differ from the GKD's.
- A firmware update gives the Brick a KMS path, or the disp2 layer ioctls
  become usable for two clients (ADR-0013's own revisit).
