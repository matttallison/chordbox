# PadBox

Mini MPC-style sampling drum machine for Nintendo 2DS / 3DS (homebrew). It's a sibling of ChordBox and PhotoTape.

- 16 pads × 4 banks (A–D). Two kits are built in: **A** is boom bap and **B** is 808. Banks C and D are empty for your own samples.
- The sequencer has 16 sequences of 1–8 bars. It records in real time with quantize, MPC swing (50–75%), note repeat, count-in, metronome, and undo/redo.
- **STEP** page: a 16-step grid editor.
- **SMPL** page: records from the mic into any pad, with a threshold trigger and normalize.
- **CHOP** page: slices a break into 2–16 pieces (equal, or auto at the transients), sends the slices to a bank's pads, and can set the BPM from the loop length.
- **PROG** page: per-pad level, pan, tune/fine, start/end, reverse, attack, decay, one-shot/note-on mode, mute groups (hats are choked by default), and a resonant low-pass filter.
- 16 LEVELS: spreads the selected sound chromatically across all 16 pads.
- Master VINTAGE modes: CLEAN, MPC60 (12-bit), SP-1200 (12-bit at 26 kHz), LOFI (8-bit). The circle pad works as a DJ filter.
- Loads WAV files from the SD card (8/16/24/32-bit, mono or stereo, any sample rate). Saves pads as WAVs and saves whole projects to 16 slots.

## Install (QR code)

In FBI: **Remote Install > Scan QR Code**, then scan `install-qr.png`. PadBox shows up on the HOME Menu.

Direct links: [padbox.cia](https://github.com/matttallison/chordbox/releases/download/padbox/padbox.cia) · [padbox.3dsx](https://github.com/matttallison/chordbox/releases/download/padbox/padbox.3dsx)

Audio needs the DSP dump: run **DSP1** once, which creates `sdmc:/3ds/dspfirm.cdc`.

## Screens

The **bottom screen** has the 16 pads. Pad 1 is bottom-left, like on an MPC. The strip along the top holds the page tabs, the **bank** button (A–D), **BTN** (button pads) and **16L** (16 levels).
The **top screen** is the LCD for the current page.

## Controls (everywhere)

| Input | Action |
|---|---|
| Tap pad | Play it. With FULL LEVEL off, higher on the pad = louder |
| START | Play / stop |
| SELECT | Next page (SELECT+START quits) |
| A | REC on/off (overdub). With COUNT IN on, you get 1 bar of clicks first |
| X | Undo / redo the last record pass, erase or clear |
| B (hold) + pad | Erase that pad from the sequence |
| L + B | Clear the whole sequence (X undoes it) |
| R (hold) + pad | Note repeat at the QUANT rate, swung. Records when REC is on |
| Y | FULL LEVEL on/off |
| L + pad | Select a pad without playing it |
| L + Y | 16 LEVELS on/off |
| L + X | Tap tempo |
| L + A | Metronome on/off |
| L + D-pad up/down | Bank |
| D-pad up/down | Move between fields |
| D-pad left/right | Change the value (hold L for bigger steps) |
| Circle pad | Master DJ filter: down = low-pass, up = high-pass |

**BTN mode** (tap BTN) lets you finger-drum with your thumbs, because the touchscreen only takes one touch at a time. D-pad ← ↓ → ↑ play pads 1–4, Y B A X play pads 5–8, L plays pad 9, R is still note repeat, SELECT is REC, and START is play/stop. Tap BTN again to leave BTN mode.

## Pages

**MAIN**: SEQ, BPM, BARS, QUANT (off, 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32), SWING, METRO (off, on, rec only), COUNT IN, VINTAGE, VOLUME, FULL LVL, and DOUBLE (copies the sequence to double its length). The bottom of the LCD shows an overview of the whole sequence with a playhead.

**PROG**: settings for the selected pad: SAMPLE (cycles through every loaded sample), LEVEL, PAN, TUNE, FINE, START, END, REVERSE, ATTACK, DECAY (OFF = play the whole sample), MODE (one shot / note on), MUTE GRP, CUTOFF, RESO. The waveform shows the start and end points.

**STEP**: the pads become steps 1–16 of the bar, reading from top-left. Tap a step to toggle it for the selected pad, at the VEL value. Fields: BAR, PAD, VEL. L + pad picks the pad. The top screen shows all 16 pads of the bank as a grid.

**SMPL**: press **A** to record and **A** again to stop. The take goes into the selected pad. Fields: GAIN, THRESH (waits for sound before it starts recording), MAX LEN (1–30 s), NORMALIZE.

**CHOP**: the source is the selected pad. Fields: SLICES, MODE (EQUAL/AUTO), SENS, SLICE, NUDGE (moves the selected slice's start), TO BANK, LOOP BARS. Tap the pads to audition slices. **A** sends the slices to pads 1–N of the TO BANK bank. **X** sets the BPM so that the loop is LOOP BARS long.

**DISK**: browse the SD card with D-pad up/down. **A** opens a folder or loads a WAV into the selected pad. **B** saves the selected pad (its trimmed region) as a WAV in `samples/`. D-pad left/right picks a project slot. **X** saves the project and **Y** loads it (press twice to confirm).

Files are in `sdmc:/3ds/padbox/`. Put your WAVs in `samples/`. Projects are saved in `projects/P01` to `P16`.

## Build

GitHub Actions builds it on every push to the `padbox` branch (devkitARM container) and attaches the `.cia` and `.3dsx` to the **padbox** release.
