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
| SELECT | Mode: HOLD / LATCH / STRUM / ARP |
| Circle pad up/down | Filter cutoff |
| Circle pad left/right | Pitch bend (arp speed in ARP mode) |
| START | Quit |

## Install (QR code, easiest)

In FBI: Remote Install > Scan QR Code, then scan `install-qr.png`. ChordBox shows up on the HOME Menu.

## Install (.3dsx)

1. The console needs custom firmware with the Homebrew Launcher (see 3ds.hacks.guide).
2. Audio needs the DSP firmware dump: run **DSP1** once, which creates `sdmc:/3ds/dspfirm.cdc`.
3. Put `chordbox.3dsx` in `sdmc:/3ds/` and launch it from the Homebrew Launcher.

## Build

GitHub Actions builds it on every push (devkitARM container). The `.3dsx` is attached to the **latest** release.

Locally, with devkitPro installed: `make`
