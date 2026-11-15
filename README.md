# ChordBox

Touch chord synth for Nintendo 2DS / 3DS (homebrew).

Bottom screen has 8 chord pads: the 7 chords of the current key, plus one borrowed chord (bVII in major, V in minor).

## Controls

| Input | Action |
|---|---|
| Touch pad | Play chord |
| L (hold) | Add 7th |
| R (hold) | Add 9th |
| X (hold) | sus4 |
| Y | Next sound (Saw Pad, Strings, Chip Square, Organ, E.Piano, Brass) |
| A | Major / minor key |
| B | Next voicing (Close, Inv 1, Inv 2, Open + Bass) |
| D-pad left/right | Key down / up |
| D-pad up/down | Octave |
| SELECT | Mode: HOLD / LATCH / STRUM / ARP / EXPRESS |
| Circle pad up/down | Filter cutoff |
| Circle pad left/right | Pitch bend (arp speed in ARP mode) |
| START | Quit |

## EXPRESS mode

One touch plays a fully voiced chord from a set grouped by feel: Gospel, Neo Soul, Jazz, House, Dark, Lo-Fi (12 chords each).

| Input | Action |
|---|---|
| Touch height in pad | Dynamics: top = loud and bright, bottom = soft and dark |
| Slide up/down while holding | Swell / fade live |
| Touch position across pad | Strum: left edge = block chord, right edge = slow strum |
| Slide left/right while holding | Pitch bend |
| Circle pad left/right | Inversion (-2 to +2) |
| L (hold) | Down-strum |
| R (hold) | Humanize (random timing, velocity, octave on inner notes) |
| X (hold) | Sustain pedal |
| A / B | Next / previous chord set |
| D-pad | Transpose / octave |

The top screen shows a keyboard that lights up every note that's sounding, in all modes.

## Install (QR code, easiest)

In FBI: Remote Install > Scan QR Code, then scan `install-qr.png`. ChordBox shows up on the HOME Menu.

## Install (.3dsx)

1. The console needs custom firmware with the Homebrew Launcher (see 3ds.hacks.guide).
2. Audio needs the DSP firmware dump: run **DSP1** once, which creates `sdmc:/3ds/dspfirm.cdc`.
3. Put `chordbox.3dsx` in `sdmc:/3ds/` and launch it from the Homebrew Launcher.

## Build

GitHub Actions builds it on every push (devkitARM container). The `.3dsx` is attached to the **latest** release.

Locally, with devkitPro installed: `make`
