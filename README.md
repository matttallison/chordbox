# PhotoTape — every photo is a synth patch

Nintendo 2DS / 3DS homebrew. Snap a photo and it becomes an instrument:

| From the photo | Becomes |
|---|---|
| The whole image | a **tape** (SPECTRAL = brightness as a sound spectrum, WAVE = a drone through the photo's wavetables, SCAN = pixels read as a waveform) |
| Each column | one of 64 **synth wavetables** |
| Dominant hue | **root note** |
| Brightness + warmth | **scale** (Phrygian … Lydian) |
| Red / green / blue, contrast, edges | **kick / snare / hat** tuning, decay and click |
| The most detailed column | **PERC** — a hit sliced straight off the tape |
| Contrast / saturation / brightness | synth **envelope + filter** |

## Install (QR code)

In FBI: **Remote Install > Scan QR Code**, scan `install-qr.png`. PhotoTape shows up on the HOME Menu.

Direct links: [phototape.cia](https://github.com/matttallison/chordbox/releases/download/phototape/phototape.cia) · [phototape.3dsx](https://github.com/matttallison/chordbox/releases/download/phototape/phototape.3dsx)

## Pages

Tap the tabs or press **SELECT**. **START** = play/stop the sequencer anywhere. Hold **SELECT + START** to quit.

**CAM** — live camera on top. **A** snap (saves `P####.bmp` + patch), **X** outer/inner camera, **Y** tape mode, **B** save patch, **D-pad ←/→** or `<<` `>>` browse saved photos (each one reloads its own patch: pattern, sliders, sample), **D-pad ↑/↓** shift root.

**TAPE** — tape looper. Drag across the strip to set the loop, tap to jump. Sliders: SPEED, WOW, DRIVE, HISS, TONE, LEVEL. **A** run/stop (with tape-stop slowdown), **B** reverse, **Y** sync loop to 1/2/4 bars, **X** full loop.

**GRAIN** — granular sampler. Touch/slide the strip to play grains from that spot. Sliders: SIZE, DENS, SPRAY, PITCH, REV, LEVEL. **A/R** latch the cloud, **X** source = tape or mic sample, circle pad drifts the position.

**SEQ** — 7 tracks × 16 steps: KICK SNARE HAT PERC SYNTH GRAIN SAMPLE. Tap/drag the grid; the lane underneath sets velocity (drums) or note (melodic, in the photo's scale). **L/R** track, **A** mute, **B** audition, **X** generate this track from the photo, **Y** clear, circle pad = track tune / decay, **D-pad ↑/↓** tempo, **←/→** swing.

**MIC** — sampler. **A** record (up to 6 s), **B** play. Drag the waveform to set start/end (or D-pad / L R ZL ZR to nudge). TRIM, NORM, REVERSE, FADE, UNDO, **>TAPE** (sample becomes the tape for looping/grains/perc), SAVE WAV (`S####.wav`), GRAIN source, mic GAIN.

**KEYS** — 16 pads in the photo's scale playing SYNTH, GRAIN or SAMPLE, plus 4 drum pads. **D-pad ↑/↓** octave. Hold **R** while the sequencer runs to record what you play into the pattern. Face buttons: A kick, B snare, X hat, L perc.

Files: `sdmc:/3ds/phototape/`.
