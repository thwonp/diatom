# 0047. The microphone is the port's, and the core opens it

- **Status:** Accepted
- **Date:** 2026-10-25
- **Supersedes:** -
- **Superseded by:** -

## Context

- The RG Rotate has a working internal mic (TortOS plorpos-3yv.75, seq 138):
  the default PipeWire source, stereo 48 kHz, whose LEFT channel is an unfed
  ADC pinned at the rail and whose RIGHT channel is the mic. A mono request
  averages the two.
- No core got mic input: Diatom refused `RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE`.
  melonDS DS then logs "Microphone interface not available; substituting
  silence". DSperate's libretro wrapper had no mic path at all.
- melonDS DS turns the mic on as a game starts but reads it only while the game
  listens: Pokemon Pearl's title read 0 samples in 25 s (measured).
- Opening an SDL capture device on the Rotate costs 285 ms cold, 40-90 ms warm
  (measured). An open capture device keeps the codec's ADC powered, and one
  held across a system suspend stalls (plorpos-3yv.75.1).

## Options considered

### Option A - the libretro interface over a port capture API
`diatom_port_mic_open/close/run/read`; the env answers the interface only
where `caps.mic` is set. The port captures, picks the mic's channel, and
converts to the rate the core asked for. Costs a port API and stubs on the
ports with no mic.

### Option B - a blow button
melonDS DS's Blow input on a mapped button. Cheaper, but no real mic, and
DSperate has no equivalent.

## Decision

Option A. One mic. The handle is a token that stays valid until the core
closes it; the port owns the device. It is opened at the FIRST READ, not when
the core turns the mic on, and let go at every pause and game end (the same
place the Rotate lets go of its sink, port/gkd.c audio_release) - the next read
takes it back. A read returns at most ~100 ms of input: a backlog after a
pause is dropped. Per-stretch summaries (samples, dropped, peak, rms) are
logged when the device goes, never per frame.

## Consequences

- A DS game that listens pays one open (40-285 ms) the first time it does,
  and again after each pause. A game that never listens never opens it.
- Nothing is open across a system suspend, so .75.1 cannot happen in a game.
- Ports without a mic refuse the interface; their cores substitute silence
  as before.
- Verified with the stub core (`STUBCORE_MIC=1`) on the Rotate: 10 s silent
  room rms 33-118, the same with a 1 kHz tone from the speaker rms ~5000,
  0 dropped, 59.73 fps unchanged.
