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
| SELECT | Mode: HOLD / LATCH / STRUM / ARP / EXPRESS / HICHORD |
| Circle pad up/down | Filter cutoff |
| Circle pad left/right | Pitch bend (arp speed in ARP mode) |
| START | Quit |

## HICHORD mode (default at startup)

Plays like a HiChord. The 2DS controls map like this:

| HiChord | 2DS |
|---|---|
| Chord buttons 1-7 | Top row of touch pads |
| Joystick | Circle pad: hold a chord, push to change it, let go to snap back |
| Joystick click (chord lock) | L: locks the pushed change onto that chord button. Press L with the stick centred to unlock |
| Inversion control | R: Root / 1st / 2nd |
| Gray menu (key & settings) | X |
| Yellow menu (sounds & effects) | Y |
| Red menu (modes & tempo) | A |
| Mode + button 1-6 | Hold A + tap pad: Play, Strum, Lead, Drone, Arp, Repeat |
| Sound + button 1-7 | Hold Y + tap pad: Saw, Sine, FM E.Piano, FM Piano, Strings, Clarinet, FM Wow |

With a menu open, the D-pad moves up/down and changes values left/right. B closes the menu, and it also closes itself after 5 seconds. With no menu open, the D-pad changes key (left/right) and octave (up/down).

**Joystick maps** (Yellow > JOYSTICK): Default, Extended, Chromatic, Borrow, OG. The top screen shows a compass with the current map's 8 directions and lights up the one you're pushing.

**Modes:**
- Play: polyphonic, sounds while held.
- Strum: rolls root, 3rd, 5th, then extras at Slow 200 ms, Med 80 ms or Fast 40 ms. The bass sounds with the first note.
- Lead: root note only. The stick moves up/down an octave and left/right a semitone.
- Drone: rings until you pick another chord.
- Arp: Up, Down, Up/Dn, Dn/Up or Rand patterns, Arp Only or Chord+Arp layers, rates from 1/1 to 1/32.
- Repeat: pulses the chord in time with the BPM.

**Settings:**
- Bass: Off, Root (2 octaves down), or Slash (the 5th; the 3rd on minor chords; the 4th or 2nd on sus chords).
- Voice leading: on or off.
- Voices: 8, 4, 2 or 1.
- ADSR presets: Patch, Keys, Pad, Pluck or Sustain.
- Scales: 10.

**Strum pad:** strings below the chord pads, tuned to the current chord including any joystick change. Swipe across them, and play higher on the strings for louder notes.

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
