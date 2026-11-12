# 0049. On the Miyoo Mini Flip, retime the panel to the core and lock every core to it

- **Status:** Accepted
- **Date:** 2026-11-12
- **Supersedes:** [0048](0048-the-vsync-lock-is-not-on-by-default-beyond-pokedaisy.md), on the Miyoo Mini Flip only
- **Superseded by:** -

## Context

The Flip's panel refreshes at 59.657 Hz. Its cores report 59.18 (Neo Geo) to 60.25 Hz (RACE), and 50 for PAL. A core
that runs free drops or repeats a frame every 1/|core - panel| seconds: every 2-3 s for the common 60.0-60.1 Hz cores,
which the owner sees as a hitch while scrolling. Measured (frames replaced before they reached the glass): Sonic 2
(picodrive) 22 a minute, Super Mario Bros. (FCEUmm) 36 in 80 s, DKC (snes9x2010) 60-70 in 187 s.

The first fix (plorpos-f1p.66) locked three listed cores to the panel. Locked, a game runs at the panel's rate, not its
own (FCEUmm 59.65 instead of 60.10, 0.75 % slow), and the lock was kept off the rest because heavy cores lost speed
under it (DKC 58-59 fps locked against 60.08 free) and RACE overflowed audio. The owner wanted every core paced, no
per-core list and no per-game rule, and no core slower than it runs free.

Found on the device (TortOS plorpos-f1p.68, from mhal.ko and measured with the vsync interrupt count): the panel is MIPI
DSI in video mode, and its refresh follows the frame the DSI host sends. Three register writes change it live, with a
steady picture through the switch:

| Register | Where | Stock |
|---|---|---|
| DSI horizontal front porch (bytes, even steps: the fine step) | `0x1f345258`, 32-bit only | 160 |
| Timing generator vertical total (lines) | bank `0x1129` reg `0x12` = `0x1f225248`, 16-bit, bits 12:0 | 580 |
| DSI vertical front porch (lines), changed by the same amount as the total | `0x1f345228`, 32-bit only | 10 |

`Hz = 59.657 * 580 / V * 2850 / (2850 - d)` for V lines and d bytes taken off the porch, within 0.015 Hz of the meter
from 50.00 to 60.31 Hz. Changing either vertical register alone rolls the picture or does nothing; the LPLL does not set
this panel's rate. A switch takes 3-14 us and drops nothing: DKC through two switches, 0 audio frames dropped.

## Options considered

### Option A - retime the panel to each game's reported fps, cores free
The panel follows the core, so a free core's frames meet the panel one to one. Generic: it needs only the fps every core
reports. Costs three undocumented register writes (one unit tested, from driver disassembly) and a stock reset at game
end and resident start. Leaves heavy cores and frame pacing to the free-running loop.

### Option B - lock every core to the stock panel (as other ports do)
No register work, but every game runs at 59.657 Hz: up to 1 % off its true speed, audio resampled by the same, and heavy
cores lose their frameskip (DKC 58-59 fps). Rejected.

### Option C - retime, then lock every core, with automatic ways out (chosen)
With the panel at the core's rate, the lock holds the game at its own speed (the resampler ratio is about 1.0), shows
every frame and costs a light core nothing. What still breaks a lock is detected, not listed: a core that cannot keep
the panel, and a core whose audio overflows (RACE, whose `retro_run` sometimes emulates two frames).

### Option D - keep a per-core lock list on the retimed panel
Works for the three listed cores, leaves the rest juddering against nothing in particular, and keeps a list to curate.
Rejected by the owner's rule.

## Decision

On the Flip only (`port/miyoo.c`):

- **Retime at game start.** `diatom_port_content_fps(fps)`, a new optional port hook called after
  `retro_get_system_av_info`, picks the setting (V in 571..699, d in 0..12 even) nearest the reported fps; if none is
  within 0.05 Hz, stock. `diatom_port_content_fps(0)` at game end, and stock at resident start, so a killed game cannot
  leave its rate behind; the launcher and Muse always run at stock. The flip thread's period estimate is seeded with
  the new rate and accepts 50 Hz gaps. A unit whose registers do not read as a state this code could have written is
  left alone. Off: `DIATOM_RETIME=0` or `TortOS/diatom.noretime`.
- **Every core runs locked** (the core list is gone), with three automatic exits:
  - more than 2 of 120 frames slip 1.5 panel periods, or 120 frames take 2 % longer than 120 periods (a core slow
    without slipping) -> stand down, retry after 10 s, doubling to 5 min, never shorter again within the game;
  - more than 1 ms of audio dropped since the lock began, checked every frame -> free for the rest of the game.
  Off: `DIATOM_VSYNC=0` or `TortOS/diatom.novsync`.

## Consequences

Measured through the launcher on the Flip (frames never shown counts the `flip:` lines after the launch window):

| Game (core) | Core fps | Panel set / measured | Result |
|---|---|---|---|
| DKC (snes9x2010) | 60.0985 | 60.094 / 60.091 | locked 60.02 fps, 0 audio dropped, 0 never shown (free stock: 60-70 never shown) |
| DKC (snes9x2005_plus) | 59.9227 | 59.928 / 59.927 | locked 59.93, 0 dropped, 0 never shown |
| SMB3 (FCEUmm) | 60.0998 | 60.094 / 60.092 | locked 60.10 (was 59.65 under the old lock), 0 dropped |
| Dank Tomb (fake-08) | 60.00 | 60.009 / 60.006 | locked 60.01, 0 dropped |
| 1943 Kai (Beetle PCE Fast) | 59.82 | 59.825 / 59.823 | locked 59.83 |
| Neo Geo, CPS1, gpSP, RACE, PAL picodrive | 59.18 .. 60.25, 50 | within 0.01 | right rate, 0 never shown |
| Baseball Stars (RACE) | 60.25 | 60.240 | lets go after 37 frames (129 audio frames dropped), then 60.28 free; locked without the exit: 60.56 fps, 7188 dropped in 73 s |
| Super Mario RPG, SA-1 (snes9x2005_plus) | 59.9227 | | 56.78 fps locked before the slow-window exit, 59.63 after (free about the same) |
| Yoshi's Island, Super FX 2 (snes9x2005_plus) | 59.9227 | | 59.82 locked vs 59.83 free over 40 s |

- Every game the Flip runs is paced, with no per-core or per-game table. Locked cores keep their true speed.
- A lid sleep (the Flip has no suspend; the backlight goes off) keeps the retimed timing.
- Harder: three undocumented registers, found on one unit. The fallback on an unknown reading is stock timing.
- A heavy game that lets go of the lock on audio pays 1-3 ms of audio once (Star Fox 153 frames, Yoshi's Island 60,
  RACE 129); the same games free drop 0. The locked catch-up run (an extra `retro_run` when a frame slipped) is what
  overflows; changing it touches every locked core and was not done.
- A core that changes rate mid-game (`SET_SYSTEM_AV_INFO`) keeps the rate it started with, as the loop does.
- Other ports are unchanged; 0048 still holds there.

## Revisit if

Another Flip unit reads non-stock timing at resident start (the `[warn] panel: no retime` line); a played heavy scene
loses speed locked against free; the audio exit fires on a core that runs clean free; or a port gains panel registers
it can retime the same way.
