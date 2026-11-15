// ChordBox - touch chord synth for Nintendo 2DS / 3DS (homebrew, libctru)
//
// Bottom screen: 8 chord pads (diatonic chords of the current key + 1 borrowed chord)
// Controls:
//   Touch pad ......... play chord
//   L (hold) .......... add 7th          R (hold) .... add 9th
//   X (hold) .......... sus4             Y ........... next sound
//   A ................. major / minor    B ........... next voicing
//   D-pad L/R ......... key down / up    D-pad U/D ... octave up / down
//   SELECT ............ play mode (HOLD / LATCH / STRUM / ARP / EXPRESS)
//   Circle pad up/down  filter cutoff    Circle pad L/R pitch bend (arp speed in ARP)
//   START ............. quit
//
// EXPRESS mode (one-touch voiced chords from sets grouped by feel):
//   12 pads per set; A / B = next / previous set (Gospel, Neo Soul, Jazz, House, Dark, Lo-Fi)
//   Touch height in pad = dynamics (top loud/bright), slide up/down while holding to swell
//   Touch position across pad = strum (left = block chord, right = slow strum)
//   Slide left/right while holding = pitch bend   Circle pad L/R = inversion
//   L (hold) = down-strum   R (hold) = humanize   X (hold) = sustain pedal
// Top screen shows a keyboard lighting up the notes that are sounding.

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
	float pan, vel;
	unsigned age;
} Voice;

static Voice voices[MAXV];
static unsigned voiceAge = 0;
static int patchIdx = 0;
static float sinTab[1024];
static float fltL1, fltL2, fltR1, fltR2;
static float bendSemis = 0.0f, cutoffMul = 1.0f;
static float envBrass = 0.0f, fltA = 0.3f;
static float exprGain = 1.0f, exprTarget = 1.0f; // live dynamics (EXPRESS mode)
static int exprFilter = 0;                         // dynamics also open the filter

static float mtof(int n) { return 440.0f * powf(2.0f, (n - 69) / 12.0f); }

static void noteOn(int note, int delaySamples, float vel) {
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
	v->vel = vel;
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
	noteOn(arpVoiceNote, 0, 1.0f);
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
		exprGain += (exprTarget - exprGain) * 0.0015f;
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
			float x = sum * oscNorm * v->env * P->gain * v->vel * exprGain;
			L += x * (1.0f - v->pan);
			R += x * v->pan;
		}

		if ((s & 15) == 0) { // update filter coefficient every 16 samples
			float fcx = fc;
			if (P->osc == OSC_SAW && P->a < 0.05f) { // brass: filter opens on attack
				envBrass += ((anyAttack ? 1.0f : 0.0f) - envBrass) * 0.008f;
				fcx = fc * (1.0f + 2.5f * envBrass);
			}
			if (exprFilter) fcx *= 0.3f + 1.1f * exprGain;
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


// ---------------------------------------------------------------- EXPRESS chord sets
// Fully voiced chords grouped by feel (written in C, transposed by key/octave).
// notes[] are semitones from C3 (MIDI 48); notes below 0 are the bass.
typedef struct {
	int root;          // pitch class of chord root in C
	const char* q;     // quality suffix
	int slash;         // pitch class of slash bass, -1 for none
	int n;
	int notes[6];
} XChord;

typedef struct { const char* name; XChord c[12]; } XSet;

static const XSet XSETS[] = {
	{"GOSPEL", {
		{0, "maj9", -1, 5, {-12,4,11,14,19}},  {2, "m11", -1, 5, {-10,5,12,14,19}},
		{4, "m7", -1, 5, {-8,7,11,14,19}},      {5, "maj7#11", -1, 5, {-7,4,9,11,16}},
		{7, "13", -1, 5, {-5,5,9,16,19}},       {9, "m9", -1, 5, {-3,7,11,12,16}},
		{4, "7#9", -1, 4, {-8,8,14,19}},        {9, "7b13", -1, 5, {-3,7,13,17,21}},
		{2, "m9", -1, 5, {-10,5,9,12,16}},      {7, "7sus4", -1, 5, {-5,5,12,14,19}},
		{5, "m6", -1, 5, {-7,8,12,14,17}},      {0, "add9", 7, 6, {-5,4,7,12,14,19}}}},
	{"NEO SOUL", {
		{3, "maj9", -1, 5, {-9,7,10,14,17}},    {2, "m9", -1, 5, {-10,5,9,12,16}},
		{7, "m11", -1, 5, {-5,10,12,14,17}},    {0, "m9", -1, 5, {-12,3,7,10,14}},
		{5, "13", -1, 5, {-7,3,9,14,19}},       {10, "maj7", -1, 5, {-14,9,14,17,21}},
		{8, "maj7#11", -1, 5, {-16,7,12,14,19}},{5, "m9", -1, 5, {-7,8,12,15,19}},
		{10, "13", -1, 5, {-14,8,14,19,24}},    {3, "m9", -1, 5, {-9,6,10,13,17}},
		{1, "9", -1, 5, {-11,5,11,15,20}},      {0, "m11", -1, 5, {-12,3,10,14,17}}}},
	{"JAZZ", {
		{2, "m9", -1, 5, {-10,5,12,16,21}},     {7, "13", -1, 5, {-5,5,11,16,21}},
		{0, "6/9", -1, 5, {-12,4,9,14,19}},     {5, "maj7", -1, 5, {-7,4,9,12,16}},
		{11, "m7b5", -1, 5, {-13,9,14,17,21}},  {4, "7b9", -1, 5, {-8,8,14,17,20}},
		{9, "m9", -1, 5, {-3,7,11,12,16}},      {2, "9", -1, 5, {-10,6,12,16,21}},
		{7, "m9", -1, 5, {-5,10,14,17,21}},     {0, "7alt", -1, 5, {-12,4,10,15,20}},
		{5, "maj9", -1, 5, {-7,4,9,12,19}},     {3, "dim7", -1, 5, {-9,6,9,12,15}}}},
	{"HOUSE", {
		{0, "m9", -1, 5, {0,3,7,10,14}},        {3, "maj9", -1, 5, {3,7,10,14,17}},
		{5, "m9", -1, 5, {5,8,12,15,19}},       {7, "m7", -1, 4, {7,10,14,17}},
		{8, "maj9", -1, 5, {-4,0,3,7,10}},      {10, "sus2", -1, 4, {-2,0,5,10}},
		{2, "m7b5", -1, 4, {2,5,8,12}},         {7, "7sus4", -1, 4, {-5,0,2,5}},
		{0, "m11", -1, 5, {0,5,7,10,15}},       {5, "m7", -1, 4, {5,8,12,15}},
		{8, "6/9", -1, 5, {-4,0,5,10,15}},      {3, "maj7", 7, 4, {-5,3,7,10}}}},
	{"DARK", {
		{9, "m(add9)", -1, 5, {-3,9,12,16,23}}, {5, "maj7#11", -1, 5, {-7,4,9,11,16}},
		{2, "m9", -1, 5, {-10,5,9,12,16}},      {4, "m7", -1, 5, {-8,7,11,14,19}},
		{9, "m", 7, 4, {-5,9,12,16}},           {5, "maj9", -1, 5, {-7,7,9,12,16}},
		{4, "7b9", -1, 5, {-8,8,14,17,20}},     {10, "maj7", -1, 5, {-14,9,14,17,21}},
		{2, "m6", -1, 5, {-10,5,9,11,14}},      {11, "dim", -1, 4, {-13,5,11,14}},
		{4, "sus4", -1, 4, {-8,9,11,16}},       {7, "sus2", -1, 4, {-5,7,9,14}}}},
	{"LO-FI", {
		{5, "maj9", -1, 5, {-7,4,7,9,12}},      {4, "m9", -1, 5, {-8,7,11,14,18}},
		{2, "m11", -1, 5, {-10,5,7,12,16}},     {0, "maj9", -1, 5, {-12,4,7,11,14}},
		{9, "m11", -1, 5, {-3,7,12,14,19}},     {10, "maj9", -1, 5, {-14,9,12,14,17}},
		{7, "13", -1, 5, {-5,5,9,11,16}},       {4, "7#9", -1, 4, {-8,8,14,19}},
		{8, "maj7", -1, 4, {-16,7,12,15}},      {1, "maj7#11", -1, 5, {-11,5,7,12,17}},
		{3, "maj9", -1, 5, {-9,7,10,14,17}},    {7, "m9", -1, 5, {-5,10,14,17,21}}}},
};
#define NXSET (int)(sizeof(XSETS)/sizeof(XSETS[0]))

static void xChordName(const XChord* c, int key, char* out, int len) {
	if (c->slash >= 0)
		snprintf(out, len, "%s%s/%s", NOTE_NAMES[(c->root + key) % 12], c->q, NOTE_NAMES[(c->slash + key) % 12]);
	else
		snprintf(out, len, "%s%s", NOTE_NAMES[(c->root + key) % 12], c->q);
}

// inversion: >0 moves lowest upper note up an octave, <0 moves highest down (bass stays put)
static int xVoice(const XChord* c, int key, int oct, int inv, int* out) {
	int base = 48 + key + 12 * oct;
	if (key > 5) base -= 12;
	int n = c->n;
	for (int i = 0; i < n; i++) out[i] = base + c->notes[i];
	qsort(out, n, sizeof(int), cmpInt);
	int first = (n > 1 && out[1] - out[0] >= 7) ? 1 : 0; // keep bass note fixed
	for (int k = 0; k < inv; k++) { out[first] += 12; qsort(out + first, n - first, sizeof(int), cmpInt); }
	for (int k = 0; k > inv; k--) { out[n - 1] -= 12; qsort(out + first, n - first, sizeof(int), cmpInt); }
	return n;
}

// ---------------------------------------------------------------- drawing (both screens)
static u8* fb;
static int fbW = 320;
static u8* fontGfx;

static inline void px(int x, int y, u8 r, u8 g, u8 b) {
	if (x < 0 || x >= fbW || y < 0 || y >= 240) return;
	u8* p = fb + (x * 240 + (239 - y)) * 3;
	p[0] = b; p[1] = g; p[2] = r;
}

static void rect(int x0, int y0, int w, int h, u8 r, u8 g, u8 b) {
	for (int x = x0; x < x0 + w; x++)
		for (int y = y0; y < y0 + h; y++) px(x, y, r, g, b);
}

static void frame(int x, int y, int w, int h, u8 r, u8 g, u8 b) {
	rect(x, y, w, 2, r, g, b); rect(x, y + h - 2, w, 2, r, g, b);
	rect(x, y, 2, h, r, g, b); rect(x + w - 2, y, 2, h, r, g, b);
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

static const u8 PADCOL[12][3] = {
	{230,110, 40},{200, 70, 60},{170, 60,110},{110, 70,160},
	{ 50,120,170},{ 40,150,120},{120,150, 50},{210,170, 40},
	{220, 90, 90},{150, 90,190},{ 60,160,180},{190,130, 60}
};

enum { M_HOLD, M_LATCH, M_STRUM, M_ARP, M_EXPRESS, M_COUNT };
static const char* MODE_NAMES[M_COUNT] = {"HOLD", "LATCH", "STRUM", "ARP", "EXPRESS"};

// pad geometry: classic = 4x2 (80x100), express = 4x3 (80x66)
static int padRows(int mode) { return mode == M_EXPRESS ? 3 : 2; }
static int padH(int mode) { return 200 / padRows(mode); }

static void drawBottom(int mode, int key, int minor, int add7, int add9, int sus, int xset,
                       int active, const char* status, const char* hint) {
	fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320;
	rect(0, 0, 320, 240, 16, 14, 18);
	int rows = padRows(mode), ph = padH(mode);
	for (int i = 0; i < rows * 4; i++) {
		int x = (i % 4) * 80, y = (i / 4) * ph;
		const u8* col = PADCOL[i % 12];
		float k = (i == active) ? 1.0f : 0.38f;
		rect(x + 3, y + 3, 74, ph - 6, col[0] * k, col[1] * k, col[2] * k);
		if (i == active) frame(x + 3, y + 3, 74, ph - 6, 255, 255, 255);
		char name[24];
		if (mode == M_EXPRESS) {
			xChordName(&XSETS[xset].c[i], key, name, sizeof(name));
			int sc = strlen(name) <= 4 ? 2 : 1;
			drawTextC(x + 40, y + ph / 2 - 4 * sc, name, sc, 255, 255, 255);
			// strum-zone ticks along the bottom edge: left = block, right = slow strum
			for (int t = 0; t < 5; t++) rect(x + 10 + t * 14, y + ph - 10, 2, 2 + t, 255, 230, 200);
		} else {
			Chord c; buildChord(&c, i, key, minor, add7, add9, sus);
			drawTextC(x + 40, y + 14, c.roman, 1, 255, 240, 220);
			int sc = strlen(c.name) <= 4 ? 2 : 1;
			drawTextC(x + 40, y + (sc == 2 ? 44 : 48), c.name, sc, 255, 255, 255);
		}
	}
	rect(0, 200, 320, 40, 30, 26, 34);
	drawText(6, 206, status, 1, 255, 200, 120);
	drawText(6, 222, hint, 1, 170, 160, 180);
}

// ---------------------------------------------------------------- top screen + keyboard
#define KB_LO 24   // C1
#define KB_HI 96   // C7
#define KB_Y  168
#define KB_H  66
#define KW    9
#define KB_X  7

static int isBlack(int n) { int p = n % 12; return p == 1 || p == 3 || p == 6 || p == 8 || p == 10; }
static int whiteIndex(int n) { // number of white keys from KB_LO up to n
	int c = 0; for (int i = KB_LO; i < n; i++) if (!isBlack(i)) c++; return c;
}

static void drawKeyboard(const u8* on, int keyPc) {
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	rect(0, KB_Y - 4, 400, KB_H + 8, 16, 14, 18);
	// white keys
	for (int n = KB_LO; n <= KB_HI; n++) {
		if (isBlack(n)) continue;
		int x = KB_X + whiteIndex(n) * KW;
		if (on[n]) rect(x, KB_Y, KW - 1, KB_H, 255, 140, 50);
		else rect(x, KB_Y, KW - 1, KB_H, 235, 230, 222);
		if (n % 12 == keyPc && !on[n]) rect(x + 2, KB_Y + KB_H - 6, KW - 5, 3, 120, 110, 140); // key root marker
	}
	// black keys
	for (int n = KB_LO; n <= KB_HI; n++) {
		if (!isBlack(n)) continue;
		int x = KB_X + whiteIndex(n) * KW - 3;
		if (on[n]) rect(x, KB_Y, 6, KB_H * 6 / 10, 255, 170, 60);
		else rect(x, KB_Y, 6, KB_H * 6 / 10, 28, 24, 30);
	}
	// octave labels
	for (int n = KB_LO; n <= KB_HI; n += 12) {
		char lab[4]; snprintf(lab, sizeof(lab), "C%d", n / 12 - 1);
		drawText(KB_X + whiteIndex(n) * KW, KB_Y - 12 + 0, lab, 1, 120, 110, 140);
	}
}

static void drawTopInfo(int audioOK, int mode, int key, int minor, int oct, int voicing, int xset,
                        int xinv, const char* chordName, const char* roman, const int* notes, int nn) {
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	rect(0, 0, 400, KB_Y - 14, 16, 14, 18);
	drawText(8, 6, "ChordBox", 2, 255, 160, 60);
	drawText(400 - 8 - strlen(MODE_NAMES[mode]) * 16, 6, MODE_NAMES[mode], 2, 120, 200, 220);

	// big chord name
	int sc = strlen(chordName) <= 8 ? 4 : 3;
	drawTextC(200, 40, chordName, sc, 255, 255, 255);
	if (roman[0]) drawTextC(200, 40 + 8 * sc + 4, roman, 1, 255, 210, 150);

	// note names
	char line[80] = ""; int len = 0;
	for (int i = 0; i < nn && len < 70; i++)
		len += snprintf(line + len, sizeof(line) - len, "%s%d ", NOTE_NAMES[notes[i] % 12], notes[i] / 12 - 1);
	drawTextC(200, 92, line, 1, 200, 190, 210);

	// settings row
	char row[64];
	if (mode == M_EXPRESS)
		snprintf(row, sizeof(row), "SET %s  TRANSPOSE %+d  OCT %+d  INV %+d", XSETS[xset].name, key > 5 ? key - 12 : key, oct, xinv);
	else
		snprintf(row, sizeof(row), "KEY %s %s  OCT %+d  %s", NOTE_NAMES[key], minor ? "MIN" : "MAJ", oct,
			VOICING_NAMES[voicing]);
	drawTextC(200, 112, row, 1, 255, 200, 120);
	snprintf(row, sizeof(row), "SOUND: %s", PATCHES[patchIdx].name);
	drawTextC(200, 126, row, 1, 255, 200, 120);
	if (!audioOK) drawTextC(200, 140, "NO AUDIO - run DSP1 once (dspfirm.cdc)", 1, 255, 80, 80);
}

// ---------------------------------------------------------------- main
int main(void) {
	gfxInitDefault();
	gfxSetDoubleBuffering(GFX_TOP, false);
	gfxSetDoubleBuffering(GFX_BOTTOM, false);
	fontGfx = consoleGetDefault()->font.gfx;
	srand(osGetTime());

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

	int key = 0, minor = 0, oct = 0, voicing = V_CLOSE, mode = M_HOLD;
	int xset = 0, xinv = 0;
	int activePad = -1, prevTouchPad = -1;
	int prev7 = 0, prev9 = 0, prevSus = 0;
	int dirty = 1, topDirty = 1;
	char chordName[24] = "-", roman[12] = "";
	int curNotes[MAXN], curN = 0;
	// express state
	int xPad = -1, xTouchX0 = 0, xSustain = 0;
	float xU = 0.0f, xV = 0.0f;
	u8 kbOn[128], kbPrev[128];
	memset(kbPrev, 0xff, sizeof(kbPrev));

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown(), kHeld = hidKeysHeld(), kUp = hidKeysUp();
		if (kDown & KEY_START) break;
		int express = (mode == M_EXPRESS);

		int add7 = 0, add9 = 0, sus = 0;
		if (!express) { add7 = !!(kHeld & KEY_L); add9 = !!(kHeld & KEY_R); sus = !!(kHeld & KEY_X); }

		if (kDown & KEY_DRIGHT) { key = (key + 1) % 12; dirty = topDirty = 1; }
		if (kDown & KEY_DLEFT)  { key = (key + 11) % 12; dirty = topDirty = 1; }
		if (kDown & KEY_DUP)    { if (oct < 2) oct++; topDirty = 1; }
		if (kDown & KEY_DDOWN)  { if (oct > -2) oct--; topDirty = 1; }
		if (kDown & KEY_Y)      { patchIdx = (patchIdx + 1) % NPATCH; dirty = topDirty = 1; }
		if (express) {
			if (kDown & KEY_A) { xset = (xset + 1) % NXSET; dirty = topDirty = 1; }
			if (kDown & KEY_B) { xset = (xset + NXSET - 1) % NXSET; dirty = topDirty = 1; }
		} else {
			if (kDown & KEY_A) { minor = !minor; dirty = topDirty = 1; }
			if (kDown & KEY_B) { voicing = (voicing + 1) % V_COUNT; topDirty = 1; }
		}
		if (kDown & KEY_SELECT) {
			mode = (mode + 1) % M_COUNT; releaseAll(); arpOn = 0; arpN = 0;
			activePad = -1; xPad = -1; curN = 0; strcpy(chordName, "-"); roman[0] = 0;
			bendSemis = 0; exprTarget = exprGain = 1.0f;
			exprFilter = (mode == M_EXPRESS);
			express = (mode == M_EXPRESS);
			dirty = topDirty = 1;
		}

		circlePosition cp; hidCircleRead(&cp);
		float cy = cp.dy / 156.0f, cx = cp.dx / 156.0f;
		if (fabsf(cy) < 0.08f) cy = 0;
		if (fabsf(cx) < 0.08f) cx = 0;
		cutoffMul = powf(2.0f, cy * 2.5f);

		// touch
		touchPosition t; int touching = !!(kHeld & KEY_TOUCH);
		if (touching) hidTouchRead(&t);
		int ph = padH(mode);
		int touchPad = -1;
		if (touching && t.py < 200) touchPad = (t.py / ph) * 4 + (t.px / 80);

		if (express) {
			// circle pad left/right = inversion
			int inv = (int)roundf(cx * 2.4f);
			if (inv > 2) inv = 2;
			if (inv < -2) inv = -2;
			int invChanged = (inv != xinv);
			if (invChanged) { xinv = inv; topDirty = 1; }

			int retrig = -1;
			if (touchPad >= 0 && prevTouchPad < 0) {          // touch down
				xPad = touchPad; xTouchX0 = t.px;
				int x0 = (xPad % 4) * 80, y0 = (xPad / 4) * ph;
				xU = (t.px - x0) / 80.0f; xV = 1.0f - (t.py - y0) / (float)ph;
				exprTarget = exprGain = 0.3f + 0.7f * xV;
				bendSemis = 0;
				retrig = xPad;
			} else if (touching && xPad >= 0) {               // slide: dynamics + bend
				int y0 = (xPad / 4) * ph;
				float v = 1.0f - (t.py - y0) / (float)ph;
				if (v < 0) v = 0;
				if (v > 1) v = 1;
				exprTarget = 0.3f + 0.7f * v;
				float b = (t.px - xTouchX0) / 80.0f;
				if (b > 2) b = 2;
				if (b < -2) b = -2;
				bendSemis = b;
				if (invChanged) retrig = xPad;
			} else if (!touching && xPad >= 0) {              // lift
				if (kHeld & KEY_X) xSustain = 1;               // X = sustain pedal
				else releaseAll();
				xPad = -1; bendSemis = 0; dirty = 1;
			}
			if (xSustain && (kUp & KEY_X) && !touching) { releaseAll(); xSustain = 0; }

			if (retrig >= 0) {
				const XChord* xc = &XSETS[xset].c[retrig];
				curN = xVoice(xc, key, oct, xinv, curNotes);
				xChordName(xc, key, chordName, sizeof(chordName));
				snprintf(roman, sizeof(roman), "%s", XSETS[xset].name);
				releaseAll(); xSustain = 0;
				int down = !!(kHeld & KEY_L), human = !!(kHeld & KEY_R);
				int strumStep = (int)(xU * xU * 0.09f * SR);
				int topIdx = curN - 1;
				int order[MAXN];
				for (int i = 0; i < curN; i++) order[i] = down ? curN - 1 - i : i;
				for (int k = 0; k < curN; k++) {
					int i = order[k];
					int note = curNotes[i];
					float vel = 0.85f;
					if (i == topIdx) vel = 1.0f;            // melody note on top sings out
					if (i == 0) vel = 0.8f;
					int delay = k * strumStep;
					if (human) {
						vel *= 0.8f + 0.4f * (rand() / (float)RAND_MAX);
						delay += rand() % (int)(0.015f * SR);
						if (i > 0 && i < topIdx && rand() % 3 == 0) { note += 12; curNotes[i] = note; }
					}
					noteOn(note, delay, vel);
				}
				activePad = retrig; dirty = topDirty = 1;
			}
		} else {
			bendSemis = (mode == M_ARP) ? 0 : cx * 2.0f;
			if (mode == M_ARP) arpRate = 0.125f * powf(2.0f, -cx * 1.5f);

			int newTouch = (touchPad >= 0 && touchPad != prevTouchPad);
			int modsChanged = (add7 != prev7 || add9 != prev9 || sus != prevSus);
			if (modsChanged) dirty = 1;

			int trigger = -1, stop = 0;
			if (mode == M_HOLD || mode == M_STRUM) {
				if (newTouch) trigger = touchPad;
				else if (touchPad >= 0 && modsChanged) trigger = touchPad;
				else if (touchPad < 0 && prevTouchPad >= 0) stop = 1;
			} else {
				if (newTouch && prevTouchPad < 0) {
					if (touchPad == activePad) stop = 1; else trigger = touchPad;
				} else if (newTouch) trigger = touchPad;
				else if (activePad >= 0 && modsChanged) trigger = activePad;
			}

			if (stop) {
				releaseAll(); arpOn = 0; arpN = 0; activePad = -1; dirty = topDirty = 1;
			}
			if (trigger >= 0) {
				Chord cur;
				buildChord(&cur, trigger, key, minor, add7, add9, sus);
				curN = voiceChord(&cur, key, oct, voicing, curNotes);
				strcpy(chordName, cur.name); strcpy(roman, cur.roman);
				releaseAll();
				if (mode == M_ARP) {
					arpN = 0;
					for (int i = 0; i < curN; i++) arpNotes[arpN++] = curNotes[i];
					for (int i = 0; i < curN; i++) arpNotes[arpN++] = curNotes[i] + 12;
					if (!arpOn) { arpIdx = 0; arpCounter = 0; }
					arpOn = 1;
				} else {
					int strum = (mode == M_STRUM) ? (int)(0.035f * SR) : 0;
					for (int i = 0; i < curN; i++) noteOn(curNotes[i], i * strum, 1.0f);
				}
				activePad = trigger; dirty = topDirty = 1;
			}
		}
		if (express && xPad < 0 && activePad >= 0 && !xSustain) { activePad = -1; dirty = 1; }
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
			drawTopInfo(audioOK, mode, key, minor, oct, voicing, xset, xinv, chordName,
				roman, curNotes, curN);
			memset(kbPrev, 0xff, sizeof(kbPrev)); // force keyboard redraw
			topDirty = 0;
		}
		memset(kbOn, 0, sizeof(kbOn));
		for (int i = 0; i < MAXV; i++) {
			Voice* v = &voices[i];
			if (v->active && v->stage >= 1 && v->stage <= 3 && v->note >= 0 && v->note < 128) kbOn[v->note] = 1;
		}
		if (memcmp(kbOn, kbPrev, sizeof(kbOn))) {
			drawKeyboard(kbOn, key);
			memcpy(kbPrev, kbOn, sizeof(kbOn));
		}

		// bottom screen
		if (dirty) {
			char status[48], hint[48];
			if (express) {
				snprintf(status, sizeof(status), "%s  %+d  %s", XSETS[xset].name, key > 5 ? key - 12 : key, PATCHES[patchIdx].name);
				strcpy(hint, "UP:loud RIGHT:strum L:dn R:hum X:sus");
			} else {
				snprintf(status, sizeof(status), "%s %s  %s  %s", NOTE_NAMES[key], minor ? "MIN" : "MAJ",
					PATCHES[patchIdx].name, MODE_NAMES[mode]);
				strcpy(hint, "L:7  R:9  X:sus4  Y:sound  B:voicing");
			}
			drawBottom(mode, key, minor, add7, add9, sus, xset, activePad, status, hint);
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
