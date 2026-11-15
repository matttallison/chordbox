// ChordBox - touch chord synth for Nintendo 2DS / 3DS (homebrew, libctru)
//
// Bottom screen: 8 chord pads (diatonic chords of the current key + 1 borrowed chord)
// Controls:
//   Touch pad ......... play chord
//   L (hold) .......... add 7th          R (hold) .... add 9th
//   X (hold) .......... sus4             Y ........... next sound
//   A ................. major / minor    B ........... next voicing
//   D-pad L/R ......... key down / up    D-pad U/D ... octave up / down
//   SELECT ............ play mode (HOLD / LATCH / STRUM / ARP)
//   Circle pad up/down  filter cutoff    Circle pad L/R pitch bend (arp speed in ARP)
//   START ............. quit

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SR          32000
#define BUF_SAMPLES 512
#define NBUFS       4
#define MAXV        16
#define MAXN        8
#define TWO_PI      6.28318530718f

// ---------------------------------------------------------------- theory
static const char* NOTE_NAMES[12] = {"C","C#","D","Eb","E","F","F#","G","Ab","A","Bb","B"};
static const int SC_MAJ[7] = {0,2,4,5,7,9,11};
static const int SC_MIN[7] = {0,2,3,5,7,8,10};
static const char* ROMAN[7] = {"I","II","III","IV","V","VI","VII"};
static const char* ROMAN_LC[7] = {"i","ii","iii","iv","v","vi","vii"};

typedef struct {
	int root;          // semitones above key
	int iv[6];         // intervals above chord root
	int n;
	char name[20];
	char roman[10];
} Chord;

static int sdeg(const int* sc, int d) { return sc[d % 7] + 12 * (d / 7); }

static void buildChord(Chord* c, int pad, int key, int minor, int add7, int add9, int sus) {
	const int* sc = minor ? SC_MIN : SC_MAJ;
	int third, fifth, sev, nine;
	if (pad < 7) {
		c->root = sc[pad];
		third = sdeg(sc, pad + 2) - c->root;
		fifth = sdeg(sc, pad + 4) - c->root;
		sev   = sdeg(sc, pad + 6) - c->root;
		nine  = sdeg(sc, pad + 8) - c->root;
	} else if (!minor) {   // bVII in major
		c->root = 10; third = 4; fifth = 7; sev = 10; nine = 14;
	} else {               // V (harmonic) in minor
		c->root = 7;  third = 4; fifth = 7; sev = 10; nine = 13;
	}

	// roman numeral (before sus)
	if (pad < 7) {
		if (third == 4) strcpy(c->roman, ROMAN[pad]); else strcpy(c->roman, ROMAN_LC[pad]);
		if (fifth == 6) strcat(c->roman, "o");
		if (fifth == 8) strcat(c->roman, "+");
	} else strcpy(c->roman, minor ? "V" : "bVII");

	int isMaj = (third == 4 && fifth == 7), isMin = (third == 3 && fifth == 7);
	int isDim = (third == 3 && fifth == 6), isAug = (third == 4 && fifth == 8);
	if (sus) third = 5;

	c->n = 0;
	c->iv[c->n++] = 0; c->iv[c->n++] = third; c->iv[c->n++] = fifth;
	if (add7) c->iv[c->n++] = sev;
	if (add9) c->iv[c->n++] = nine;

	// name
	char q[16] = "";
	if (add7) {
		if (sus)        strcpy(q, sev == 11 ? "maj7sus4" : "7sus4");
		else if (isMaj) strcpy(q, sev == 11 ? "maj7" : "7");
		else if (isMin) strcpy(q, sev == 11 ? "m(maj7)" : "m7");
		else if (isDim) strcpy(q, sev == 9 ? "dim7" : "m7b5");
		else if (isAug) strcpy(q, "+7");
		if (add9) {
			if (nine == 13) strcat(q, "b9");
			else if (!strcmp(q, "maj7")) strcpy(q, "maj9");
			else if (!strcmp(q, "7"))    strcpy(q, "9");
			else if (!strcmp(q, "m7"))   strcpy(q, "m9");
			else strcat(q, "(9)");
		}
	} else {
		if (sus) strcpy(q, "sus4");
		else if (isMin) strcpy(q, "m");
		else if (isDim) strcpy(q, "dim");
		else if (isAug) strcpy(q, "+");
		if (add9) strcat(q, nine == 13 ? "addb9" : "add9");
	}
	snprintf(c->name, sizeof(c->name), "%s%s", NOTE_NAMES[(key + c->root) % 12], q);
}

enum { V_CLOSE, V_INV1, V_INV2, V_OPEN, V_COUNT };
static const char* VOICING_NAMES[V_COUNT] = {"CLOSE", "INV 1", "INV 2", "OPEN+BASS"};

static int cmpInt(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }

static int voiceChord(const Chord* c, int key, int oct, int voicing, int* out) {
	int base = 48 + key + 12 * oct;
	if (key > 5) base -= 12;
	int n = c->n;
	for (int i = 0; i < n; i++) out[i] = base + c->root + c->iv[i];
	qsort(out, n, sizeof(int), cmpInt);
	switch (voicing) {
	case V_INV1: out[0] += 12; break;
	case V_INV2: out[0] += 12; out[1] += 12; break;
	case V_OPEN: out[1] += 12; out[n++] = base + c->root - 12; break;
	}
	qsort(out, n, sizeof(int), cmpInt);
	return n;
}

// ---------------------------------------------------------------- synth
enum { OSC_SAW, OSC_SQUARE, OSC_ORGAN, OSC_TRI };

typedef struct {
	const char* name;
	int osc, nosc;
	float detune;     // cents spread
	float a, d, s, r; // seconds / level
	float cutoff;     // Hz at circle-pad centre
	float gain;
	float duty;
} Patch;

static const Patch PATCHES[] = {
	{"SAW PAD",     OSC_SAW,    2,  8.0f, 0.06f, 0.40f, 0.80f, 0.70f, 2600.0f, 0.16f, 0.5f},
	{"STRINGS",     OSC_SAW,    3, 14.0f, 0.45f, 0.30f, 0.90f, 1.20f, 1900.0f, 0.15f, 0.5f},
	{"CHIP SQUARE", OSC_SQUARE, 1,  0.0f, 0.002f,0.10f, 0.85f, 0.06f, 15000.0f,0.09f, 0.25f},
	{"ORGAN",       OSC_ORGAN,  1,  0.0f, 0.01f, 0.05f, 1.00f, 0.08f, 6000.0f, 0.13f, 0.5f},
	{"E.PIANO",     OSC_TRI,    2,  4.0f, 0.003f,1.40f, 0.00f, 0.40f, 3500.0f, 0.22f, 0.5f},
	{"BRASS",       OSC_SAW,    2,  6.0f, 0.03f, 0.25f, 0.70f, 0.20f, 1400.0f, 0.17f, 0.5f},
};
#define NPATCH (int)(sizeof(PATCHES)/sizeof(PATCHES[0]))

typedef struct {
	int active, stage;    // stage: 0 off 1 att 2 dec 3 sus 4 rel
	int note, delay;
	float freq, env, relRate;
	float ph[3];
	float pan;
	unsigned age;
} Voice;

static Voice voices[MAXV];
static unsigned voiceAge = 0;
static int patchIdx = 0;
static float sinTab[1024];
static float fltL1, fltL2, fltR1, fltR2;
static float bendSemis = 0.0f, cutoffMul = 1.0f;
static float envBrass = 0.0f, fltA = 0.3f;

static float mtof(int n) { return 440.0f * powf(2.0f, (n - 69) / 12.0f); }

static void noteOn(int note, int delaySamples) {
	int best = -1; unsigned oldest = 0xffffffff;
	for (int i = 0; i < MAXV; i++) if (!voices[i].active) { best = i; break; }
	if (best < 0) {
		// steal oldest releasing voice, else oldest overall
		for (int i = 0; i < MAXV; i++)
			if (voices[i].stage == 4 && voices[i].age < oldest) { oldest = voices[i].age; best = i; }
		if (best < 0) for (int i = 0; i < MAXV; i++)
			if (voices[i].age < oldest) { oldest = voices[i].age; best = i; }
	}
	Voice* v = &voices[best];
	memset(v, 0, sizeof(*v));
	v->active = 1; v->stage = delaySamples > 0 ? 0 : 1; v->delay = delaySamples;
	v->note = note; v->freq = mtof(note);
	v->ph[0] = 0.0f; v->ph[1] = 0.33f; v->ph[2] = 0.71f;
	v->pan = 0.5f + 0.35f * sinf(note * 1.7f);
	v->age = voiceAge++;
}

static void releaseVoice(Voice* v) {
	if (!v->active) return;
	if (v->stage == 0) { v->active = 0; return; } // strum note not yet started
	if (v->stage == 4) return;
	v->stage = 4;
	float r = PATCHES[patchIdx].r;
	v->relRate = v->env / (r * SR + 1.0f);
}

static void releaseAll(void) { for (int i = 0; i < MAXV; i++) releaseVoice(&voices[i]); }

static inline float polyblep(float t, float dt) {
	if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
	if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
	return 0.0f;
}

static inline float sinLook(float p) { return sinTab[(int)(p * 1024.0f) & 1023]; }

static inline float oscSample(int type, float p, float dt, float duty) {
	switch (type) {
	case OSC_SAW: return (2.0f * p - 1.0f) - polyblep(p, dt);
	case OSC_SQUARE: {
		float s = p < duty ? 1.0f : -1.0f;
		s += polyblep(p, dt);
		float p2 = p - duty; if (p2 < 0.0f) p2 += 1.0f;
		s -= polyblep(p2, dt);
		return s;
	}
	case OSC_ORGAN: {
		float p2 = p * 2.0f, p3 = p * 3.0f, p4 = p * 4.0f;
		return 0.6f * sinLook(p) + 0.45f * sinLook(p2) + 0.3f * sinLook(p3) + 0.15f * sinLook(p4);
	}
	default: { // tri + sine (e-piano-ish)
		float t = 4.0f * fabsf(p - 0.5f) - 1.0f;
		return 0.5f * t + 0.7f * sinLook(p) + 0.12f * sinLook(p * 3.0f);
	}
	}
}

// arp state
static int arpOn = 0, arpNotes[MAXN * 2], arpN = 0, arpIdx = 0, arpCounter = 0, arpVoiceNote = -1;
static float arpRate = 0.125f; // seconds per step

static void arpStep(void) {
	if (arpN == 0) return;
	for (int i = 0; i < MAXV; i++)
		if (voices[i].active && voices[i].note == arpVoiceNote && voices[i].stage != 4) releaseVoice(&voices[i]);
	arpVoiceNote = arpNotes[arpIdx % arpN];
	arpIdx++;
	noteOn(arpVoiceNote, 0);
}

static void fillAudio(s16* out, int n) {
	const Patch* P = &PATCHES[patchIdx];
	float bend = powf(2.0f, bendSemis / 12.0f);
	float aInc = 1.0f / (P->a * SR + 1.0f);
	float dDec = (1.0f - P->s) / (P->d * SR + 1.0f);
	float fc = P->cutoff * cutoffMul;
	if (fc > 15000.0f) fc = 15000.0f;
	if (fc < 80.0f) fc = 80.0f;

	float det[3];
	if (P->nosc == 1) { det[0] = 1.0f; }
	else if (P->nosc == 2) { det[0] = powf(2.0f, -P->detune / 1200.0f); det[1] = powf(2.0f, P->detune / 1200.0f); }
	else { det[0] = powf(2.0f, -P->detune / 1200.0f); det[1] = 1.0f; det[2] = powf(2.0f, P->detune / 1200.0f); }
	float oscNorm = 1.0f / sqrtf((float)P->nosc);

	int arpSamples = (int)(arpRate * SR);

	for (int s = 0; s < n; s++) {
		if (arpOn && arpN > 0) {
			if (--arpCounter <= 0) { arpCounter = arpSamples; arpStep(); }
		}

		// brass-style filter envelope follows overall activity
		float L = 0.0f, R = 0.0f;
		int anyAttack = 0;
		for (int i = 0; i < MAXV; i++) {
			Voice* v = &voices[i];
			if (!v->active) continue;
			if (v->stage == 0) { if (--v->delay <= 0) v->stage = 1; else continue; }
			switch (v->stage) {
			case 1: v->env += aInc; anyAttack = 1; if (v->env >= 1.0f) { v->env = 1.0f; v->stage = 2; } break;
			case 2: v->env -= dDec; if (v->env <= P->s) { v->env = P->s; v->stage = 3; } break;
			case 4: v->env -= v->relRate; if (v->env <= 0.0f) { v->active = 0; continue; } break;
			}
			if (v->stage == 3 && P->s <= 0.0001f) { v->active = 0; continue; }
			float f = v->freq * bend;
			float sum = 0.0f;
			for (int o = 0; o < P->nosc; o++) {
				float dt = f * det[o] / SR;
				v->ph[o] += dt; if (v->ph[o] >= 1.0f) v->ph[o] -= 1.0f;
				sum += oscSample(P->osc, v->ph[o], dt, P->duty);
			}
			float x = sum * oscNorm * v->env * P->gain;
			L += x * (1.0f - v->pan);
			R += x * v->pan;
		}

		if ((s & 15) == 0) { // update filter coefficient every 16 samples
			float fcx = fc;
			if (P->osc == OSC_SAW && P->a < 0.05f) { // brass: filter opens on attack
				envBrass += ((anyAttack ? 1.0f : 0.0f) - envBrass) * 0.008f;
				fcx = fc * (1.0f + 2.5f * envBrass);
			}
			if (fcx > 15000.0f) fcx = 15000.0f;
			fltA = 1.0f - expf(-TWO_PI * fcx / SR);
		}
		float a = fltA;
		fltL1 += a * (L - fltL1); fltL2 += a * (fltL1 - fltL2);
		fltR1 += a * (R - fltR1); fltR2 += a * (fltR1 - fltR2);
		L = fltL2 * 1.6f; R = fltR2 * 1.6f;

		// soft clip
		L = L * (27.0f + L * L) / (27.0f + 9.0f * L * L);
		R = R * (27.0f + R * R) / (27.0f + 9.0f * R * R);
		if (L > 1.0f) L = 1.0f;
		if (L < -1.0f) L = -1.0f;
		if (R > 1.0f) R = 1.0f;
		if (R < -1.0f) R = -1.0f;
		out[s * 2]     = (s16)(L * 32000.0f);
		out[s * 2 + 1] = (s16)(R * 32000.0f);
	}
}

// ---------------------------------------------------------------- bottom screen drawing
static u8* fb;
static u8* fontGfx;

static inline void px(int x, int y, u8 r, u8 g, u8 b) {
	if (x < 0 || x >= 320 || y < 0 || y >= 240) return;
	u8* p = fb + (x * 240 + (239 - y)) * 3;
	p[0] = b; p[1] = g; p[2] = r;
}

static void rect(int x0, int y0, int w, int h, u8 r, u8 g, u8 b) {
	for (int x = x0; x < x0 + w; x++)
		for (int y = y0; y < y0 + h; y++) px(x, y, r, g, b);
}

static void drawChar(int x, int y, char ch, int sc, u8 r, u8 g, u8 b) {
	const u8* gl = fontGfx + ((u8)ch) * 8;
	for (int row = 0; row < 8; row++)
		for (int col = 0; col < 8; col++)
			if (gl[row] & (0x80 >> col)) rect(x + col * sc, y + row * sc, sc, sc, r, g, b);
}

static void drawText(int x, int y, const char* s, int sc, u8 r, u8 g, u8 b) {
	for (; *s; s++, x += 8 * sc) drawChar(x, y, *s, sc, r, g, b);
}

static void drawTextC(int cx, int y, const char* s, int sc, u8 r, u8 g, u8 b) {
	int w = strlen(s) * 8 * sc;
	drawText(cx - w / 2, y, s, sc, r, g, b);
}

// pad colours (warm palette)
static const u8 PADCOL[8][3] = {
	{230,110, 40},{200, 70, 60},{170, 60,110},{110, 70,160},
	{ 50,120,170},{ 40,150,120},{120,150, 50},{210,170, 40}
};

#define PAD_W 80
#define PAD_H 100

static void drawPads(int key, int minor, int add7, int add9, int sus, int active, const char* status) {
	rect(0, 0, 320, 240, 16, 14, 18);
	for (int i = 0; i < 8; i++) {
		Chord c; buildChord(&c, i, key, minor, add7, add9, sus);
		int x = (i % 4) * PAD_W, y = (i / 4) * PAD_H;
		const u8* col = PADCOL[i];
		float k = (i == active) ? 1.0f : 0.38f;
		rect(x + 3, y + 3, PAD_W - 6, PAD_H - 6, col[0] * k, col[1] * k, col[2] * k);
		if (i == active) { // outline
			rect(x + 3, y + 3, PAD_W - 6, 2, 255, 255, 255);
			rect(x + 3, y + PAD_H - 5, PAD_W - 6, 2, 255, 255, 255);
			rect(x + 3, y + 3, 2, PAD_H - 6, 255, 255, 255);
			rect(x + PAD_W - 5, y + 3, 2, PAD_H - 6, 255, 255, 255);
		}
		drawTextC(x + PAD_W / 2, y + 14, c.roman, 1, 255, 240, 220);
		int sc = strlen(c.name) <= 4 ? 2 : 1;
		drawTextC(x + PAD_W / 2, y + (sc == 2 ? 44 : 48), c.name, sc, 255, 255, 255);
	}
	rect(0, 200, 320, 40, 30, 26, 34);
	drawText(6, 206, status, 1, 255, 200, 120);
	drawText(6, 222, "L:7  R:9  X:sus4  Y:sound  B:voicing", 1, 170, 160, 180);
}

// ---------------------------------------------------------------- main
static const char* MODE_NAMES[4] = {"HOLD", "LATCH", "STRUM", "ARP"};

int main(void) {
	gfxInitDefault();
	PrintConsole top;
	consoleInit(GFX_TOP, &top);
	gfxSetDoubleBuffering(GFX_BOTTOM, false);
	fontGfx = consoleGetDefault()->font.gfx;

	for (int i = 0; i < 1024; i++) sinTab[i] = sinf(TWO_PI * i / 1024.0f);

	int audioOK = R_SUCCEEDED(ndspInit());
	ndspWaveBuf wbuf[NBUFS];
	s16* audioMem = NULL;
	if (audioOK) {
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		ndspChnReset(0);
		ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
		ndspChnSetRate(0, SR);
		ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
		float mix[12]; memset(mix, 0, sizeof(mix)); mix[0] = mix[1] = 1.0f;
		ndspChnSetMix(0, mix);
		audioMem = (s16*)linearAlloc(BUF_SAMPLES * 2 * sizeof(s16) * NBUFS);
		memset(wbuf, 0, sizeof(wbuf));
		for (int i = 0; i < NBUFS; i++) {
			wbuf[i].data_vaddr = audioMem + i * BUF_SAMPLES * 2;
			wbuf[i].nsamples = BUF_SAMPLES;
			memset((void*)wbuf[i].data_vaddr, 0, BUF_SAMPLES * 4);
			DSP_FlushDataCache(wbuf[i].data_vaddr, BUF_SAMPLES * 4);
			ndspChnWaveBufAdd(0, &wbuf[i]);
		}
	}

	int key = 0, minor = 0, oct = 0, voicing = V_CLOSE, mode = 0;
	int activePad = -1, prevTouchPad = -1;
	int prev7 = 0, prev9 = 0, prevSus = 0;
	int dirty = 1, topDirty = 1;
	Chord cur; memset(&cur, 0, sizeof(cur)); strcpy(cur.name, "-");
	int curNotes[MAXN], curN = 0;

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown(), kHeld = hidKeysHeld();
		if (kDown & KEY_START) break;

		int add7 = !!(kHeld & KEY_L), add9 = !!(kHeld & KEY_R), sus = !!(kHeld & KEY_X);

		if (kDown & KEY_DRIGHT) { key = (key + 1) % 12; dirty = topDirty = 1; }
		if (kDown & KEY_DLEFT)  { key = (key + 11) % 12; dirty = topDirty = 1; }
		if (kDown & KEY_DUP)    { if (oct < 2) oct++; topDirty = 1; }
		if (kDown & KEY_DDOWN)  { if (oct > -2) oct--; topDirty = 1; }
		if (kDown & KEY_A)      { minor = !minor; dirty = topDirty = 1; }
		if (kDown & KEY_B)      { voicing = (voicing + 1) % V_COUNT; topDirty = 1; }
		if (kDown & KEY_Y)      { patchIdx = (patchIdx + 1) % NPATCH; dirty = topDirty = 1; }
		if (kDown & KEY_SELECT) {
			mode = (mode + 1) % 4; releaseAll(); arpOn = 0; arpN = 0;
			activePad = -1; dirty = topDirty = 1;
		}

		circlePosition cp; hidCircleRead(&cp);
		float cy = cp.dy / 156.0f, cx = cp.dx / 156.0f;
		if (fabsf(cy) < 0.08f) cy = 0;
		if (fabsf(cx) < 0.08f) cx = 0;
		cutoffMul = powf(2.0f, cy * 2.5f);
		if (mode == 3) { bendSemis = 0; arpRate = 0.125f * powf(2.0f, -cx * 1.5f); }
		else bendSemis = cx * 2.0f;

		// touch
		int touchPad = -1;
		if (kHeld & KEY_TOUCH) {
			touchPosition t; hidTouchRead(&t);
			if (t.py < 200) touchPad = (t.py / PAD_H) * 4 + (t.px / PAD_W);
		}
		int newTouch = (touchPad >= 0 && touchPad != prevTouchPad);
		int modsChanged = (add7 != prev7 || add9 != prev9 || sus != prevSus);
		if (modsChanged) dirty = 1;

		int trigger = -1, stop = 0;
		if (mode == 0 || mode == 2) {          // HOLD / STRUM: sound while touching
			if (newTouch) trigger = touchPad;
			else if (touchPad >= 0 && modsChanged) trigger = touchPad;
			else if (touchPad < 0 && prevTouchPad >= 0) stop = 1;
		} else {                               // LATCH / ARP: tap to start, tap same pad to stop
			if (newTouch && prevTouchPad < 0) {
				if (touchPad == activePad) stop = 1; else trigger = touchPad;
			} else if (newTouch) trigger = touchPad;
			else if (activePad >= 0 && modsChanged) trigger = activePad;
		}

		if (stop) {
			releaseAll(); arpOn = 0; arpN = 0; activePad = -1; dirty = topDirty = 1;
		}
		if (trigger >= 0) {
			buildChord(&cur, trigger, key, minor, add7, add9, sus);
			curN = voiceChord(&cur, key, oct, voicing, curNotes);
			releaseAll();
			if (mode == 3) {
				arpN = 0;
				for (int i = 0; i < curN; i++) arpNotes[arpN++] = curNotes[i];
				for (int i = 0; i < curN; i++) arpNotes[arpN++] = curNotes[i] + 12;
				if (!arpOn) { arpIdx = 0; arpCounter = 0; }
				arpOn = 1;
			} else {
				int strum = (mode == 2) ? (int)(0.035f * SR) : 0;
				for (int i = 0; i < curN; i++) noteOn(curNotes[i], i * strum);
			}
			activePad = trigger; dirty = topDirty = 1;
		}
		prevTouchPad = touchPad; prev7 = add7; prev9 = add9; prevSus = sus;

		// audio
		if (audioOK) {
			for (int i = 0; i < NBUFS; i++) {
				if (wbuf[i].status == NDSP_WBUF_DONE) {
					fillAudio((s16*)wbuf[i].data_vaddr, BUF_SAMPLES);
					DSP_FlushDataCache(wbuf[i].data_vaddr, BUF_SAMPLES * 4);
					ndspChnWaveBufAdd(0, &wbuf[i]);
				}
			}
		}

		// top screen
		if (topDirty) {
			consoleSelect(&top);
			printf("\x1b[2J\x1b[1;1H");
			printf("\x1b[33m  ChordBox\x1b[0m  - touch chord synth\n\n");
			if (!audioOK) printf("\x1b[31m  No audio: dump DSP firmware first\n  (run DSP1 to create sdmc:/3ds/dspfirm.cdc)\x1b[0m\n\n");
			printf("  Key ....... %s %s\n", NOTE_NAMES[key], minor ? "minor" : "major");
			printf("  Octave .... %+d\n", oct);
			printf("  Sound ..... %s\n", PATCHES[patchIdx].name);
			printf("  Voicing ... %s\n", VOICING_NAMES[voicing]);
			printf("  Mode ...... %s\n\n", MODE_NAMES[mode]);
			printf("  Chord:  \x1b[36m%s\x1b[0m  (%s)\n", activePad >= 0 ? cur.name : "-", activePad >= 0 ? cur.roman : "");
			printf("  Notes: ");
			if (activePad >= 0) for (int i = 0; i < curN; i++) printf(" %s%d", NOTE_NAMES[curNotes[i] % 12], curNotes[i] / 12 - 1);
			printf("\n\n");
			printf("\x1b[90m  Pad ........ play chord\n");
			printf("  L / R ...... add 7th / 9th (hold)\n");
			printf("  X .......... sus4 (hold)\n");
			printf("  Y .......... next sound\n");
			printf("  A .......... major / minor key\n");
			printf("  B .......... next voicing\n");
			printf("  D-pad ...... key L/R, octave U/D\n");
			printf("  SELECT ..... HOLD/LATCH/STRUM/ARP\n");
			printf("  Circle pad . U/D filter, L/R bend\n");
			printf("               (L/R = arp speed in ARP)\n");
			printf("  START ...... quit\x1b[0m\n");
			topDirty = 0;
		}

		// bottom screen
		if (dirty) {
			fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
			char status[48];
			snprintf(status, sizeof(status), "%s %s  %s  %s", NOTE_NAMES[key], minor ? "MIN" : "MAJ",
				PATCHES[patchIdx].name, MODE_NAMES[mode]);
			drawPads(key, minor, add7, add9, sus, activePad, status);
			dirty = 0;
		}

		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}

	if (audioOK) { ndspChnWaveBufClear(0); ndspExit(); linearFree(audioMem); }
	gfxExit();
	return 0;
}
