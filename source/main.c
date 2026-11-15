// ChordBox - touch chord synth for Nintendo 2DS / 3DS (homebrew, libctru)
//
// Bottom screen: 8 chord pads (diatonic chords of the current key + 1 borrowed chord)
// Controls:
//   Touch pad ......... play chord
//   L (hold) .......... add 7th          R (hold) .... add 9th
//   X (hold) .......... sus4             Y ........... next sound
//   A ................. major / minor    B ........... next voicing
//   D-pad L/R ......... key down / up    D-pad U/D ... octave up / down
//   SELECT ............ play mode (HOLD / LATCH / STRUM / ARP / EXPRESS / JOYSTICK)
//   Circle pad up/down  filter cutoff    Circle pad L/R pitch bend (arp speed in ARP)
//   START ............. quit
//
// EXPRESS mode (one-touch voiced chords from sets grouped by feel):
//   12 pads per set; A / B = next / previous set (Gospel, Neo Soul, Jazz, House, Dark, Lo-Fi)
//   Touch height in pad = dynamics (top loud/bright), slide up/down while holding to swell
//   Touch position across pad = strum (left = block chord, right = slow strum)
//   Slide left/right while holding = pitch bend   Circle pad L/R = inversion
//   L (hold) = down-strum   R (hold) = humanize   X (hold) = sustain pedal
//
// JOYSTICK mode (HiChord-style):
//   Top row = 7 chord pads (diatonic to key + scale), hold to play; slide between pads to change
//   Circle pad = 8-way chord modifier while holding (maps: Default / Extended / Chromatic / Borrow)
//   Strum plate = swipe across 16 strings tuned to the current chord (tap for single notes)
//   L = lock current modifier   R = next stick map   A = next scale (10)   B = bass OFF/ROOT/SLASH
//   X = pad strum speed OFF / SLOW / MED / FAST     Y = sound            D-pad = key / octave
// Top screen shows a keyboard lighting up the notes that are sounding.

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SR          32000
#define BUF_SAMPLES 512
#define NBUFS       4
#define MAXV        24
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
enum { OSC_SAW, OSC_SQUARE, OSC_ORGAN, OSC_TRI, OSC_SINE, OSC_FM };

typedef struct {
	const char* name;
	int osc, nosc;
	float detune;     // cents spread
	float a, d, s, r; // seconds / level
	float cutoff;     // Hz at circle-pad centre
	float gain;
	float duty;
	float fmRatio, fmIndex, fmDecay; // FM: modulator ratio, index, index decay secs (<0 = index swells in)
	int brass;                       // filter opens on attack
} Patch;

static const Patch PATCHES[] = {
	// the first 7 match the HiChord quick-select order: Saw, Sine, FM E.Piano, FM Piano, Strings, Clarinet, FM Wow
	{"SAW",         OSC_SAW,    1,  0.0f, 0.005f,0.30f, 0.80f, 0.25f, 6000.0f, 0.20f, 0.5f},
	{"SINE",        OSC_SINE,   1,  0.0f, 0.005f,0.20f, 0.90f, 0.30f, 15000.0f,0.30f, 0.5f},
	{"FM E.PIANO",  OSC_FM,     1,  0.0f, 0.002f,1.60f, 0.15f, 0.50f, 9000.0f, 0.26f, 0.5f, 1.0f, 1.8f, 0.6f},
	{"FM PIANO",    OSC_FM,     1,  0.0f, 0.002f,2.20f, 0.00f, 0.40f, 12000.0f,0.26f, 0.5f, 1.0f, 2.8f, 0.25f},
	{"STRINGS",     OSC_SAW,    3, 14.0f, 0.45f, 0.30f, 0.90f, 1.20f, 1900.0f, 0.15f, 0.5f},
	{"CLARINET",    OSC_SQUARE, 1,  0.0f, 0.04f, 0.10f, 0.90f, 0.12f, 2200.0f, 0.14f, 0.5f},
	{"FM WOW",      OSC_FM,     1,  0.0f, 0.06f, 0.40f, 0.80f, 0.40f, 6000.0f, 0.24f, 0.5f, 2.0f, 3.0f, -0.5f},
	{"SAW PAD",     OSC_SAW,    2,  8.0f, 0.06f, 0.40f, 0.80f, 0.70f, 2600.0f, 0.16f, 0.5f},
	{"CHIP SQUARE", OSC_SQUARE, 1,  0.0f, 0.002f,0.10f, 0.85f, 0.06f, 15000.0f,0.09f, 0.25f},
	{"ORGAN",       OSC_ORGAN,  1,  0.0f, 0.01f, 0.05f, 1.00f, 0.08f, 6000.0f, 0.13f, 0.5f},
	{"E.PIANO",     OSC_TRI,    2,  4.0f, 0.003f,1.40f, 0.00f, 0.40f, 3500.0f, 0.22f, 0.5f},
	{"BRASS",       OSC_SAW,    2,  6.0f, 0.03f, 0.25f, 0.70f, 0.20f, 1400.0f, 0.17f, 0.5f, 0, 0, 0, 1},
};
#define NPATCH (int)(sizeof(PATCHES)/sizeof(PATCHES[0]))

typedef struct {
	int active, stage;    // stage: 0 off 1 att 2 dec 3 sus 4 rel
	int note, delay;
	float freq, env, relRate;
	float ph[3];
	float pan, vel;
	int tag, gate;
	float fmEnv;       // tag: 1 chord, 2 strum string; gate: samples until auto-release
	unsigned age;
} Voice;

static Voice voices[MAXV];
static unsigned voiceAge = 0;
static int patchIdx = 0;
static float sinTab[1024];
static float fltL1, fltL2, fltR1, fltR2;
static float bendSemis = 0.0f, cutoffMul = 1.0f;
static float envBrass = 0.0f, fltA = 0.3f;
// ADSR presets (HiChord-style): 0 = patch's own envelope
static const char* ADSR_NAMES[5] = {"PATCH", "KEYS", "PAD", "PLUCK", "SUSTAIN"};
static const float ADSR_P[5][4] = {{0,0,0,0}, {0.004f,1.2f,0.35f,0.9f}, {0.35f,0.6f,0.9f,1.4f},
                                   {0.002f,0.35f,0.0f,0.25f}, {0.01f,0.3f,0.9f,6.0f}};
static int adsrSel = 0, adsrEnabled = 1;
static float relCap = 0.0f; // >0 caps release time (Repeat mode)
static void effEnv(const Patch* P, float* a, float* d, float* s, float* r) {
	if (adsrEnabled && adsrSel > 0) { *a = ADSR_P[adsrSel][0]; *d = ADSR_P[adsrSel][1]; *s = ADSR_P[adsrSel][2]; *r = ADSR_P[adsrSel][3]; }
	else { *a = P->a; *d = P->d; *s = P->s; *r = P->r; }
	if (relCap > 0.0f && *r > relCap) *r = relCap;
}
static float exprGain = 1.0f, exprTarget = 1.0f; // live dynamics (EXPRESS mode)
static int exprFilter = 0;                         // dynamics also open the filter

static float mtof(int n) { return 440.0f * powf(2.0f, (n - 69) / 12.0f); }

static Voice* noteOn(int note, int delaySamples, float vel) {
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
	v->tag = 1;
	v->fmEnv = PATCHES[patchIdx].fmDecay >= 0.0f ? 1.0f : 0.0f;
	v->age = voiceAge++;
	return v;
}

static void releaseVoice(Voice* v) {
	if (!v->active) return;
	if (v->stage == 0) { v->active = 0; return; } // strum note not yet started
	if (v->stage == 4) return;
	v->stage = 4;
	float a_, d_, s_, r;
	effEnv(&PATCHES[patchIdx], &a_, &d_, &s_, &r);
	v->relRate = v->env / (r * SR + 1.0f);
}

static void releaseAll(void) { for (int i = 0; i < MAXV; i++) releaseVoice(&voices[i]); }
static void releaseTag(int tag) { for (int i = 0; i < MAXV; i++) if (voices[i].tag == tag) releaseVoice(&voices[i]); }

static inline float polyblep(float t, float dt) {
	if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
	if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
	return 0.0f;
}

static inline float sinLook(float p) { return sinTab[(int)(p * 1024.0f) & 1023]; }

static inline float oscSample(int type, float p, float dt, float duty) {
	switch (type) {
	case OSC_SAW: return (2.0f * p - 1.0f) - polyblep(p, dt);
	case OSC_SINE: return sinLook(p);
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
static int arpOn = 0, arpNotes[MAXN * 4], arpN = 0, arpIdx = 0, arpCounter = 0, arpVoiceNote = -1, arpRand = 0;
static float arpRate = 0.125f; // seconds per step
static float arpGate = 0.0f;   // seconds; 0 = hold until next step
// repeat (chord pulse) state
static int repOn = 0, repNotes[MAXN], repN = 0, repCounter = 0;
static float repRate = 0.25f, repGate = 0.1f;

static void arpStep(void) {
	if (arpN == 0) return;
	for (int i = 0; i < MAXV; i++)
		if (voices[i].active && voices[i].tag == 3 && voices[i].stage != 4) releaseVoice(&voices[i]);
	arpVoiceNote = arpRand ? arpNotes[rand() % arpN] : arpNotes[arpIdx % arpN];
	arpIdx++;
	Voice* v = noteOn(arpVoiceNote, 0, 1.0f);
	v->tag = 3;
	if (arpGate > 0.0f) v->gate = (int)(arpGate * SR);
}

static void fillAudio(s16* out, int n) {
	const Patch* P = &PATCHES[patchIdx];
	float bend = powf(2.0f, bendSemis / 12.0f);
	float EA, ED, ES, ER;
	effEnv(P, &EA, &ED, &ES, &ER);
	float aInc = 1.0f / (EA * SR + 1.0f);
	float dDec = (1.0f - ES) / (ED * SR + 1.0f);
	float fmMul = (P->osc == OSC_FM) ? expf(-1.0f / (fabsf(P->fmDecay) * SR + 1.0f)) : 1.0f;
	int repSamples = (int)(repRate * SR);
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
		if (repOn && repN > 0 && --repCounter <= 0) {
			repCounter = repSamples;
			for (int i = 0; i < MAXV; i++) if (voices[i].tag == 1) releaseVoice(&voices[i]);
			for (int i = 0; i < repN; i++) { Voice* v = noteOn(repNotes[i], 0, 1.0f); v->gate = (int)(repGate * SR); }
		}

		// brass-style filter envelope follows overall activity
		float L = 0.0f, R = 0.0f;
		int anyAttack = 0;
		for (int i = 0; i < MAXV; i++) {
			Voice* v = &voices[i];
			if (!v->active) continue;
			if (v->stage == 0) { if (--v->delay <= 0) v->stage = 1; else continue; }
			if (v->gate > 0 && --v->gate == 0) releaseVoice(v);
			switch (v->stage) {
			case 1: v->env += aInc; anyAttack = 1; if (v->env >= 1.0f) { v->env = 1.0f; v->stage = 2; } break;
			case 2: v->env -= dDec; if (v->env <= ES) { v->env = ES; v->stage = 3; } break;
			case 4: v->env -= v->relRate; if (v->env <= 0.0f) { v->active = 0; continue; } break;
			}
			if (v->stage == 3 && ES <= 0.0001f) { v->active = 0; continue; }
			float f = v->freq * bend;
			float sum = 0.0f;
			if (P->osc == OSC_FM) {
				float dt = f / SR;
				v->ph[0] += dt; if (v->ph[0] >= 1.0f) v->ph[0] -= 1.0f;
				v->ph[1] += dt * P->fmRatio; if (v->ph[1] >= 1.0f) v->ph[1] -= 1.0f;
				if (P->fmDecay >= 0.0f) v->fmEnv *= fmMul; else v->fmEnv = 1.0f - (1.0f - v->fmEnv) * fmMul;
				float idx = P->fmIndex * (0.12f + 0.88f * v->fmEnv);
				sum = sinLook(v->ph[0] + idx * 0.159f * sinLook(v->ph[1]));
			} else for (int o = 0; o < P->nosc; o++) {
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
			if (P->brass) { // brass: filter opens on attack
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

// ---------------------------------------------------------------- JOYSTICK mode (HiChord-style)
// 7 diatonic chord pads + circle pad as an 8-way chord-modifier joystick + strum plate.
static const char* SCALE_NAMES[10] = {"MAJOR","MINOR","HARM MIN","MEL MIN","MAJ PENT","MIN PENT","BLUES","DORIAN","MIXOLYD","LYDIAN"};
static const int JSC[10][7] = {
	{0,2,4,5,7,9,11}, {0,2,3,5,7,8,10}, {0,2,3,5,7,8,11}, {0,2,3,5,7,9,11},
	{0,2,4,5,7,9,11}, {0,2,3,5,7,8,10}, {0,2,3,5,7,8,10}, // pentatonics/blues use parent scale for chords
	{0,2,3,5,7,9,10}, {0,2,4,5,7,9,10}, {0,2,4,6,7,9,11}
};
enum { MAP_DEFAULT, MAP_EXTENDED, MAP_CHROMATIC, MAP_BORROW, MAP_OG, MAP_COUNT };
static const char* MAP_NAMES[MAP_COUNT] = {"DEFAULT", "EXTENDED", "CHROMATIC", "BORROW", "OG"};
// directions: 0 up, 1 up-right, 2 right, 3 down-right, 4 down, 5 down-left, 6 left, 7 up-left
static const char* DIR_LABELS[MAP_COUNT][8] = {
	{"MAJ/MIN", "DOM7", "MAJ7/M7", "9TH", "SUS4", "6/SUS2", "DIM/MIN", "AUG"},
	{"MAJ/MIN", "DOM9", "ADD11", "MIN11", "7#9", "ADD9", "7SUS4", "HALF-DIM"},
	{"MMAJ7", "7ALT", "+1 SEMI", "HALF-DIM", "MAJ13", "6/9", "-1 SEMI", "7B9"},
	{"SEC DOM", "BACKDOOR", "PASS DIM7", "HALF-DIM", "PARALLEL", "BORROW 7", "FLAT MAJ", "TRITONE"},
	{"MAJ/MIN", "DOM7", "MAJ7/M7", "9TH (OG)", "SUS4", "6/SUS2", "DIM/MIN", "AUG"},
};
// short labels for the on-screen joystick compass (5 chars max)
static const char* SHORT_DIR[MAP_COUNT][8] = {
	{"MAJ/m", "DOM7", "M7/m7", "9TH", "SUS4", "6/S2", "DIM", "AUG"},
	{"MAJ/m", "DOM9", "ADD11", "MIN11", "7#9", "ADD9", "7SUS4", "m7b5"},
	{"mMAJ7", "7ALT", "+1", "m7b5", "MAJ13", "6/9", "-1", "7b9"},
	{"V/x", "BKDR", "PDIM7", "m7b5", "PARAL", "BOR7", "bMAJ", "TRITN"},
	{"MAJ/m", "DOM7", "M7/m7", "9 OG", "SUS4", "6/S2", "DIM", "AUG"},
};
static const char* BASS_NAMES[3] = {"OFF", "ROOT", "SLASH"};
static const char* STRUM_NAMES[4] = {"OFF", "SLOW", "MED", "FAST"};
static const float STRUM_SEC[4] = {0.0f, 0.200f, 0.080f, 0.040f};

typedef struct { int root; int iv[7]; int n; } JChord;

static void jSet(JChord* c, int n, const int* iv) { c->n = n; for (int i = 0; i < n; i++) c->iv[i] = iv[i]; }
static int jHas(const JChord* c, int x) { for (int i = 0; i < c->n; i++) if (c->iv[i] == x) return 1; return 0; }
static void jAdd(JChord* c, int x) { if (!jHas(c, x) && c->n < 7) c->iv[c->n++] = x; }
static void jRep(JChord* c, int from, int to) { for (int i = 0; i < c->n; i++) if (c->iv[i] == from) c->iv[i] = to; }

static int isMajorScale(int sc) { return JSC[sc][2] == 4; }

static void jTriad(JChord* c, int sc, int deg, int with7) {
	const int* s = JSC[sc];
	c->root = s[deg];
	int t = sdeg(s, deg + 2) - c->root, f = sdeg(s, deg + 4) - c->root;
	c->n = 0; c->iv[c->n++] = 0; c->iv[c->n++] = t; c->iv[c->n++] = f;
	if (with7) c->iv[c->n++] = sdeg(s, deg + 6) - c->root;
}

static void joyBuild(JChord* c, int sc, int deg, int map, int dir) {
	jTriad(c, sc, deg, 0);
	int t = c->iv[1], f = c->iv[2];
	int dim = (t == 3 && f == 6), maj = (t == 4);
	if (dir < 0) return;
	switch (map) {
	case MAP_DEFAULT:
	case MAP_OG:
		switch (dir) {
		case 0: jRep(c, t, t == 4 ? 3 : 4); break;
		case 1: jAdd(c, 10); break;
		case 2: jAdd(c, dim ? 10 : (maj ? 11 : 10)); break;
		case 3: if (dim) jAdd(c, 10); else { jAdd(c, sdeg(JSC[sc], deg + 6) - c->root); jAdd(c, 14); if (map == MAP_OG) jAdd(c, t + 12); } break;
		case 4: jRep(c, t, 5); break;
		case 5: if (maj) jAdd(c, 9); else jRep(c, t, 2); break;
		case 6: if (dim) jRep(c, 6, 7); else { jRep(c, t, 3); jRep(c, f, 6); } break;
		case 7: jRep(c, t, 4); jRep(c, f, 8); break;
		}
		break;
	case MAP_EXTENDED:
		switch (dir) {
		case 0: jRep(c, t, t == 4 ? 3 : 4); break;
		case 1: jSet(c, 5, (int[]){0,4,7,10,14}); break;
		case 2: jAdd(c, 17); break;
		case 3: jSet(c, 6, (int[]){0,3,7,10,14,17}); break;
		case 4: jSet(c, 5, (int[]){0,4,7,10,15}); break;
		case 5: jAdd(c, 14); break;
		case 6: jSet(c, 4, (int[]){0,5,7,10}); break;
		case 7: jSet(c, 4, (int[]){0,3,6,10}); break;
		}
		break;
	case MAP_CHROMATIC:
		switch (dir) {
		case 0: jSet(c, 4, (int[]){0,3,7,11}); break;
		case 1: jSet(c, 5, (int[]){0,4,10,15,20}); break;
		case 2: c->root += 1; break;
		case 3: jSet(c, 4, (int[]){0,3,6,10}); break;
		case 4: jSet(c, 6, (int[]){0,4,7,11,14,21}); break;
		case 5: jSet(c, 5, (int[]){0,4,7,9,14}); break;
		case 6: c->root -= 1; break;
		case 7: jSet(c, 5, (int[]){0,4,7,10,13}); break;
		}
		break;
	case MAP_BORROW: {
		int par = isMajorScale(sc) ? 1 : 0; // parallel: major <-> natural minor
		switch (dir) {
		case 0: c->root += 7; jSet(c, 4, (int[]){0,4,7,10}); break;          // secondary dominant
		case 1: c->root -= 2; jSet(c, 4, (int[]){0,4,7,10}); break;          // backdoor dom7
		case 2: c->root += 1; jSet(c, 4, (int[]){0,3,6,9}); break;           // passing dim7
		case 3: jSet(c, 4, (int[]){0,3,6,10}); break;                        // half-dim
		case 4: jTriad(c, par, deg, 0); break;                               // parallel major/minor
		case 5: jTriad(c, par, deg, 1); break;                               // borrowed 7th
		case 6: c->root -= 1; jSet(c, 3, (int[]){0,4,7}); break;             // flat-side major
		case 7: c->root += 1; jSet(c, 4, (int[]){0,4,7,10}); break;          // tritone sub
		}
		break;
	}
	}
	qsort(c->iv, c->n, sizeof(int), cmpInt);
}

typedef struct { int n; int iv[6]; const char* q; } QName;
static const QName QNAMES[] = {
	{2,{4,7},""}, {2,{3,7},"m"}, {2,{3,6},"dim"}, {2,{4,8},"aug"}, {2,{5,7},"sus4"}, {2,{2,7},"sus2"},
	{2,{4,6},"(b5)"},
	{3,{4,7,10},"7"}, {3,{4,7,11},"maj7"}, {3,{3,7,10},"m7"}, {3,{3,6,10},"m7b5"}, {3,{3,6,9},"dim7"},
	{3,{3,7,11},"m(maj7)"}, {3,{4,7,9},"6"}, {3,{3,7,9},"m6"}, {3,{5,7,10},"7sus4"}, {3,{4,8,10},"aug7"},
	{3,{4,7,14},"add9"}, {3,{3,7,14},"madd9"}, {3,{4,7,17},"add11"}, {3,{3,7,17},"madd11"},
	{3,{4,8,11},"augmaj7"}, {3,{2,7,9},"6sus2"},
	{4,{4,7,10,14},"9"}, {4,{4,7,11,14},"maj9"}, {4,{3,7,10,14},"m9"}, {4,{4,7,10,15},"7#9"},
	{4,{4,7,10,13},"7b9"}, {4,{4,10,15,20},"7alt"}, {4,{4,7,9,14},"6/9"}, {4,{3,6,10,14},"m9b5"},
	{4,{4,7,10,17},"11"}, {4,{4,7,11,17},"maj7add11"}, {4,{3,7,10,17},"m7add11"}, {4,{3,6,10,17},"m11b5"},
	{4,{3,7,11,14},"m(maj9)"}, {4,{4,7,9,17},"6add11"}, {4,{2,7,9,14},"6sus2"},
	{5,{3,7,10,14,17},"m11"}, {5,{4,7,11,14,21},"maj13"}, {5,{4,7,10,14,17},"11"},
};

static void joyName(const JChord* c, int key, char* out, int len) {
	int pc = ((key + c->root) % 12 + 12) % 12;
	JChord u; u.root = c->root; u.n = 0;   // drop octave doublings (OG voicing)
	for (int i = 0; i < c->n; i++) if (!(c->n > 1 && i > 1 && c->iv[i] == c->iv[1] + 12)) u.iv[u.n++] = c->iv[i];
	c = &u;
	int m = c->n - 1;
	for (unsigned k = 0; k < sizeof(QNAMES) / sizeof(QNAMES[0]); k++) {
		if (QNAMES[k].n != m) continue;
		int ok = 1;
		for (int i = 0; i < m; i++) if (QNAMES[k].iv[i] != c->iv[i + 1]) { ok = 0; break; }
		if (ok) { snprintf(out, len, "%s%s", NOTE_NAMES[pc], QNAMES[k].q); return; }
	}
	// fallback: triad name + extensions
	static const struct { int t, f; const char* q; } TRI[] = {
		{4,7,""},{3,7,"m"},{3,6,"dim"},{4,8,"aug"},{5,7,"sus4"},{2,7,"sus2"},{5,6,"sus4b5"},
		{2,6,"sus2b5"},{3,8,"m#5"},{4,6,"(b5)"},{5,8,"sus4#5"},{2,8,"sus2#5"}};
	const char* base = "?";
	if (c->n >= 3) for (unsigned k = 0; k < sizeof(TRI) / sizeof(TRI[0]); k++)
		if (TRI[k].t == c->iv[1] && TRI[k].f == c->iv[2]) { base = TRI[k].q; break; }
	char ext[24] = "";
	for (int i = 3; i < c->n; i++) {
		const char* e = "";
		switch (c->iv[i]) {
		case 9: e = "6"; break;   case 10: e = "7"; break;   case 11: e = "maj7"; break;
		case 13: e = "b9"; break; case 14: e = "9"; break;   case 15: e = "#9"; break;
		case 17: e = "11"; break; case 18: e = "#11"; break; case 20: e = "b13"; break;
		case 21: e = "13"; break;
		}
		if (strlen(ext) + strlen(e) + 4 < sizeof(ext)) { strcat(ext, i == 3 ? "add" : ","); strcat(ext, e); }
	}
	snprintf(out, len, "%s%s%s", NOTE_NAMES[pc], base, ext);
}

// strum plate: 16 strings across the current chord's pitch classes
#define NSTRINGS 16
static int joyStrings(const JChord* c, int key, int oct, int* out) {
	int k0 = key > 5 ? key - 12 : key;
	int pcs[12] = {0};
	for (int i = 0; i < c->n; i++) pcs[(((k0 + c->root + c->iv[i]) % 12) + 12) % 12] = 1;
	int n = 0, npc = 0;
	for (int i = 0; i < 12; i++) npc += pcs[i];
	int maxN = npc * 4 < NSTRINGS ? npc * 4 : NSTRINGS;   // about 4 octaves
	for (int note = 43 + 12 * oct; n < maxN && note < 128; note++)
		if (note >= 0 && pcs[note % 12]) out[n++] = note;
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

enum { M_HOLD, M_LATCH, M_STRUM, M_ARP, M_EXPRESS, M_JOY, M_COUNT };
static const char* MODE_NAMES[M_COUNT] = {"HOLD", "LATCH", "STRUM", "ARP", "EXPRESS", "HICHORD"};

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

// JOYSTICK mode bottom screen: 7 chord pads on top, strum plate below
#define JPAD_H   72
#define PLATE_Y  78
#define PLATE_H  120
static int stringX(int i, int ns) { return ns > 1 ? 10 + i * 300 / (ns - 1) : 160; }
static int stringAt(int px, int ns) { if (ns < 2) return 0; int i = ((px - 10) * (ns - 1) + 150) / 300; return i < 0 ? 0 : (i > ns - 1 ? ns - 1 : i); }

static void drawPlate(const int* strings, int ns, const int* flash) {
	fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320;
	rect(0, PLATE_Y, 320, PLATE_H, 22, 18, 26);
	rect(3, PLATE_Y + 2, 314, 2, 60, 50, 70);
	for (int i = 0; i < ns; i++) {
		int x = stringX(i, ns);
		int f = flash[i];
		if (f > 0) rect(x - 2, PLATE_Y + 6, 5, PLATE_H - 26, 255, 170 + f * 10 > 255 ? 255 : 170 + f * 10, 80);
		else rect(x, PLATE_Y + 6, 1 + (strings[i] < 55), PLATE_H - 26, 200, 185, 160);
		char nm[4]; snprintf(nm, sizeof(nm), "%s", NOTE_NAMES[strings[i] % 12]);
		drawChar(x - 3, PLATE_Y + PLATE_H - 16, nm[0], 1, f > 0 ? 255 : 150, f > 0 ? 200 : 140, f > 0 ? 120 : 160);
	}
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

static void drawTopInfo(int audioOK, int mode, const char* chordName, const char* roman,
                        const int* notes, int nn, const char* row1, const char* row2) {
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

	drawTextC(200, 112, row1, 1, 255, 200, 120);
	drawTextC(200, 126, row2, 1, 255, 200, 120);
	if (!audioOK) drawTextC(200, 140, "NO AUDIO - run DSP1 once (dspfirm.cdc)", 1, 255, 80, 80);
}


// ---------------------------------------------------------------- HICHORD mode
// Modelled on the HiChord manual: 7 chord buttons, 8-way joystick modifiers (hold chord + push,
// release to return), chord lock, inversion control, gray/yellow/red menus, Play/Strum/Lead/
// Drone/Arp/Repeat modes, ROOT/SLASH bass, voice leading, voice limit, ADSR presets.
// 2DS mapping: pads = chord buttons, circle pad = joystick, L = joystick click (lock),
// R = inversion, X/Y/A = gray/yellow/red menu, D-pad = menu nav (or key/octave), B = close menu.
enum { HM_PLAY, HM_STRUM, HM_LEAD, HM_DRONE, HM_ARP, HM_REPEAT, HM_COUNT };
static const char* HM_NAMES[HM_COUNT] = {"PLAY", "STRUM", "LEAD", "DRONE", "ARP", "REPEAT"};
static const int VOICE_OPTS[4] = {8, 4, 2, 1};
static const char* RATE_NAMES[7] = {"1/1", "1/2", "1/4", "1/8", "1/16", "1/16T", "1/32"};
static const float RATE_BEATS[7] = {4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 1.0f / 6.0f, 0.125f};
static const char* ARP_PAT_NAMES[5] = {"UP", "DOWN", "UP/DN", "DN/UP", "RAND"};
static const char* ARP_LAYER_NAMES[2] = {"ARP ONLY", "CHORD+ARP"};
static const char* HC_STRUM_NAMES[3] = {"SLOW", "MED", "FAST"};
static const float HC_STRUM_SEC[3] = {0.200f, 0.080f, 0.040f};
static const char* INV_NAMES[3] = {"ROOT", "1ST", "2ND"};

static int hcMode = HM_PLAY, hcScale = 0, hcMap = MAP_DEFAULT, hcBass = 0, hcVlead = 0, hcVoicesIdx = 0;
static int hcInv = 0, hcStrumSpd = 1, hcArpPat = 0, hcArpLayer = 0, hcArpRate = 3, hcRepRate = 3, hcBpm = 100;
static int hcKey = 0, hcOct = 0;
static int hcLock[7] = {-1, -1, -1, -1, -1, -1, -1};
static int hcDeg = -1, hcLastDeg = 0, hcDir = -1, hcPrevCode = -99;
static int hcPrevUpper[MAXN], hcPrevUpperN = 0;
static int hcNotes[MAXN], hcN = 0;        // last voiced chord, strum order (bass first)
static char hcName[24] = "-", hcSub[24] = "";
static int hcFromLock = 0;
static int hcMenu = 0, hcMenuSel = 0;     // 0 none, 1 gray, 2 yellow, 3 red
static u64 hcMenuTime = 0;
static int hcQuick = 0, hcPrevTouch = 0;
static int hcStrings[NSTRINGS], hcNs = 0, hcFlash[NSTRINGS], hcPrevString = -1, hcInPlate = 0;
static int hcPlateDirty = 0;

static int hcK0(void) { return hcKey > 5 ? hcKey - 12 : hcKey; }

static int hcEffDir(int deg) {
	if (hcMode == HM_LEAD) return -1;
	if (hcDir >= 0) return hcDir;
	return deg >= 0 ? hcLock[deg] : -1;
}

// voice the chord: notes in strum order (bass, root, 3rd, 5th, extras); returns count, *hasBass
static int hcVoice(const JChord* c, int* out, int* hasBass) {
	int k0 = hcK0();
	int nl = c->n < VOICE_OPTS[hcVoicesIdx] ? c->n : VOICE_OPTS[hcVoicesIdx];
	int tones[MAXN];
	for (int i = 0; i < nl; i++) tones[i] = 48 + k0 + c->root + c->iv[i] + 12 * hcOct;
	if (hcVlead && hcPrevUpperN > 0) {
		int best[MAXN]; float bestCost = 1e9f;
		for (int inv = 0; inv < nl; inv++) for (int sh = -1; sh <= 1; sh++) {
			int cand[MAXN]; float cost = 0;
			for (int i = 0; i < nl; i++) {
				cand[i] = tones[i] + (i < inv ? 12 : 0) + 12 * sh;
				int md = 99;
				for (int j = 0; j < hcPrevUpperN; j++) { int d = abs(cand[i] - hcPrevUpper[j]); if (d < md) md = d; }
				cost += md;
			}
			int lo = 127, hi = 0; for (int i = 0; i < nl; i++) { if (cand[i] < lo) lo = cand[i]; if (cand[i] > hi) hi = cand[i]; }
			if (lo < 36 || hi > 96) continue;
			cost += 0.15f * (hi - lo);
			if (cost < bestCost) { bestCost = cost; memcpy(best, cand, sizeof(cand)); }
		}
		if (bestCost < 1e8f) memcpy(tones, best, sizeof(int) * nl);
	} else {
		int inv = hcInv < nl ? hcInv : nl - 1;
		for (int i = 0; i < inv; i++) tones[i] += 12;
	}
	hcPrevUpperN = nl; memcpy(hcPrevUpper, tones, sizeof(int) * nl);

	int cnt = 0; *hasBass = 0;
	if (hcBass) {
		int bi = 0;
		if (hcBass == 2 && c->n > 1) {         // SLASH: 5th; 3rd on minor; 4th/2nd on sus
			int t = c->iv[1];
			if (t == 2 || t == 5) bi = t;
			else if (t == 3) bi = 3;
			else bi = jHas(c, 7) ? 7 : (jHas(c, 6) ? 6 : (jHas(c, 8) ? 8 : 0));
		}
		int b = 48 + k0 + c->root + bi - 24 + 12 * hcOct;
		while (b < 28) b += 12;
		out[cnt++] = b; *hasBass = 1;
	}
	for (int i = 0; i < nl; i++) out[cnt++] = tones[i];
	return cnt;
}

static int hcIsSounding(int note) {
	for (int i = 0; i < MAXV; i++)
		if (voices[i].active && voices[i].tag == 1 && voices[i].note == note && voices[i].stage >= 0 && voices[i].stage <= 3) return 1;
	return 0;
}

static void hcStopAll(void) {
	releaseAll(); arpOn = 0; arpN = 0; repOn = 0; repN = 0;
}

static void hcSetMode(int m) {
	hcStopAll();
	hcMode = m;
	relCap = (m == HM_REPEAT) ? 0.2f : 0.0f;
}

// play the held chord. legato: keep notes common to the previous chord (joystick moves)
static void hcPlay(int deg, int legato) {
	int eff = hcEffDir(deg);
	JChord c; joyBuild(&c, hcScale, deg, hcMap, eff);
	joyName(&c, hcKey, hcName, sizeof(hcName));
	hcFromLock = (hcDir < 0 && eff >= 0);
	if (eff >= 0) snprintf(hcSub, sizeof(hcSub), "%s%s", DIR_LABELS[hcMap][eff], hcFromLock ? " LOCK" : "");
	else hcSub[0] = 0;

	if (hcMode == HM_LEAD) {
		// root only; joystick up/down = octave, left/right = semitone while held
		int note = 48 + hcK0() + JSC[hcScale][deg] + 12 * hcOct;
		if (hcDir == 7 || hcDir == 0 || hcDir == 1) note += 12;
		if (hcDir == 3 || hcDir == 4 || hcDir == 5) note -= 12;
		if (hcDir == 1 || hcDir == 2 || hcDir == 3) note += 1;
		if (hcDir == 5 || hcDir == 6 || hcDir == 7) note -= 1;
		releaseTag(1);
		noteOn(note, 0, 1.0f);
		hcNotes[0] = note; hcN = 1;
		snprintf(hcName, sizeof(hcName), "%s%d", NOTE_NAMES[note % 12], note / 12 - 1);
		hcSub[0] = 0;
		return;
	}

	int hasBass;
	int newN = hcVoice(&c, hcNotes, &hasBass);
	hcN = newN;

	if (hcMode == HM_ARP) {
		int up[MAXN], n = 0;
		for (int i = hasBass; i < newN; i++) up[n++] = hcNotes[i];
		qsort(up, n, sizeof(int), cmpInt);
		arpN = 0;
		switch (hcArpPat) {
		case 0: for (int i = 0; i < n; i++) arpNotes[arpN++] = up[i]; break;
		case 1: for (int i = n - 1; i >= 0; i--) arpNotes[arpN++] = up[i]; break;
		case 2: for (int i = 0; i < n; i++) arpNotes[arpN++] = up[i]; for (int i = n - 2; i > 0; i--) arpNotes[arpN++] = up[i]; break;
		case 3: for (int i = n - 1; i >= 0; i--) arpNotes[arpN++] = up[i]; for (int i = 1; i < n - 1; i++) arpNotes[arpN++] = up[i]; break;
		case 4: for (int i = 0; i < n; i++) arpNotes[arpN++] = up[i]; break;
		}
		arpRand = (hcArpPat == 4);
		float step = 60.0f / hcBpm * RATE_BEATS[hcArpRate];
		arpRate = step; arpGate = step * 0.8f;
		if (!legato || !arpOn) { arpIdx = 0; arpCounter = 0; }
		arpOn = 1;
		releaseTag(1);
		if (hcArpLayer == 1 || hasBass) {        // CHORD+ARP pad underneath (bass always holds)
			for (int i = 0; i < newN; i++)
				if (hcArpLayer == 1 || i < hasBass) noteOn(hcNotes[i], 0, hcArpLayer == 1 ? 0.4f : 0.8f);
		}
		return;
	}

	if (hcMode == HM_REPEAT) {
		float step = 60.0f / hcBpm * RATE_BEATS[hcRepRate];
		repRate = step; repGate = step * 0.5f < 0.2f ? step * 0.5f : 0.2f;
		memcpy(repNotes, hcNotes, sizeof(int) * newN); repN = newN;
		if (!legato || !repOn) repCounter = 0;
		repOn = 1;
		return;
	}

	if (legato && hcMode != HM_STRUM) {
		for (int i = 0; i < MAXV; i++) {
			Voice* v = &voices[i];
			if (!v->active || v->tag != 1 || v->stage == 4) continue;
			int keep = 0; for (int k = 0; k < newN; k++) if (hcNotes[k] == v->note) keep = 1;
			if (!keep) releaseVoice(v);
		}
		for (int k = 0; k < newN; k++) if (!hcIsSounding(hcNotes[k])) noteOn(hcNotes[k], 0, 1.0f);
		return;
	}

	releaseTag(1);
	int step = (hcMode == HM_STRUM) ? (int)(HC_STRUM_SEC[hcStrumSpd] * SR) : 0;
	for (int i = 0; i < newN; i++) {
		int k = i - hasBass; if (k < 0) k = 0;   // bass sounds with the first note
		noteOn(hcNotes[i], k * step, 1.0f);
	}
}

static void hcRelease(void) {
	if (hcMode == HM_DRONE) return;                          // drone rings until next chord / mode change
	if (adsrSel == 4 && hcMode != HM_ARP && hcMode != HM_REPEAT) return; // SUSTAIN preset keeps ringing
	if (hcMode == HM_ARP) { arpOn = 0; arpN = 0; releaseTag(3); }
	if (hcMode == HM_REPEAT) { repOn = 0; repN = 0; }
	releaseTag(1);
}

// ---- menus
enum { MI_KEY, MI_OCT, MI_SOUND, MI_ADSR, MI_SCALE, MI_STICK, MI_BASS, MI_VLEAD, MI_VOICES,
       MI_MODE, MI_STRUMSPD, MI_RATE, MI_PATTERN, MI_LAYER, MI_BPM };
static const char* MI_LABEL[] = {"KEY", "OCTAVE", "SOUND", "ADSR", "SCALE", "JOYSTICK", "BASS", "VLEAD",
	"VOICES", "MODE", "STRUM SPD", "RATE", "PATTERN", "LAYER", "BPM"};

static int hcMenuItems(int menu, int* ids) {
	int n = 0;
	if (menu == 1) { ids[n++] = MI_KEY; ids[n++] = MI_OCT; }
	else if (menu == 2) { ids[n++] = MI_SOUND; ids[n++] = MI_ADSR; ids[n++] = MI_SCALE; ids[n++] = MI_STICK;
		ids[n++] = MI_BASS; ids[n++] = MI_VLEAD; ids[n++] = MI_VOICES; }
	else if (menu == 3) {
		ids[n++] = MI_MODE;
		if (hcMode == HM_STRUM) ids[n++] = MI_STRUMSPD;
		if (hcMode == HM_ARP) { ids[n++] = MI_RATE; ids[n++] = MI_PATTERN; ids[n++] = MI_LAYER; }
		if (hcMode == HM_REPEAT) ids[n++] = MI_RATE;
		ids[n++] = MI_BPM;
	}
	return n;
}

static void hcMenuValue(int id, char* b, int len) {
	switch (id) {
	case MI_KEY: snprintf(b, len, "%s", NOTE_NAMES[hcKey]); break;
	case MI_OCT: snprintf(b, len, "%+d", hcOct); break;
	case MI_SOUND: snprintf(b, len, "%s", PATCHES[patchIdx].name); break;
	case MI_ADSR: snprintf(b, len, "%s", ADSR_NAMES[adsrSel]); break;
	case MI_SCALE: snprintf(b, len, "%s", SCALE_NAMES[hcScale]); break;
	case MI_STICK: snprintf(b, len, "%s", MAP_NAMES[hcMap]); break;
	case MI_BASS: snprintf(b, len, "%s", BASS_NAMES[hcBass]); break;
	case MI_VLEAD: snprintf(b, len, "%s", hcVlead ? "AUTO" : "OFF"); break;
	case MI_VOICES: snprintf(b, len, "%d", VOICE_OPTS[hcVoicesIdx]); break;
	case MI_MODE: snprintf(b, len, "%s", HM_NAMES[hcMode]); break;
	case MI_STRUMSPD: snprintf(b, len, "%s", HC_STRUM_NAMES[hcStrumSpd]); break;
	case MI_RATE: snprintf(b, len, "%s", RATE_NAMES[hcMode == HM_ARP ? hcArpRate : hcRepRate]); break;
	case MI_PATTERN: snprintf(b, len, "%s", ARP_PAT_NAMES[hcArpPat]); break;
	case MI_LAYER: snprintf(b, len, "%s", ARP_LAYER_NAMES[hcArpLayer]); break;
	case MI_BPM: snprintf(b, len, "%d", hcBpm); break;
	}
}

static int wrap(int v, int n) { return ((v % n) + n) % n; }

static void hcMenuChange(int id, int d) {
	switch (id) {
	case MI_KEY: hcKey = wrap(hcKey + d, 12); break;
	case MI_OCT: hcOct += d; if (hcOct > 2) hcOct = 2; if (hcOct < -2) hcOct = -2; break;
	case MI_SOUND: patchIdx = wrap(patchIdx + d, NPATCH); break;
	case MI_ADSR: adsrSel = wrap(adsrSel + d, 5); releaseAll(); break;
	case MI_SCALE: hcScale = wrap(hcScale + d, 10); for (int i = 0; i < 7; i++) hcLock[i] = -1; break;
	case MI_STICK: hcMap = wrap(hcMap + d, MAP_COUNT); break;
	case MI_BASS: hcBass = wrap(hcBass + d, 3); break;
	case MI_VLEAD: hcVlead = !hcVlead; break;
	case MI_VOICES: hcVoicesIdx = wrap(hcVoicesIdx + d, 4); break;
	case MI_MODE: hcSetMode(wrap(hcMode + d, HM_COUNT)); hcMenuSel = 0; break;
	case MI_STRUMSPD: hcStrumSpd = wrap(hcStrumSpd + d, 3); break;
	case MI_RATE: if (hcMode == HM_ARP) hcArpRate = wrap(hcArpRate + d, 7); else hcRepRate = wrap(hcRepRate + d, 7); break;
	case MI_PATTERN: hcArpPat = wrap(hcArpPat + d, 5); break;
	case MI_LAYER: hcArpLayer = !hcArpLayer; break;
	case MI_BPM: hcBpm += d * 2; if (hcBpm < 40) hcBpm = 40; if (hcBpm > 240) hcBpm = 240; break;
	}
}

// ---- drawing
static const int DIR_COL[8] = {1, 2, 2, 2, 1, 0, 0, 0};
static const int DIR_ROW[8] = {0, 0, 1, 2, 2, 2, 1, 0};

static void hcDrawTop(int audioOK) {
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	rect(0, 0, 400, KB_Y - 14, 16, 14, 18);
	drawText(8, 6, "ChordBox", 2, 255, 160, 60);
	drawText(400 - 8 - strlen(HM_NAMES[hcMode]) * 16, 6, HM_NAMES[hcMode], 2, 120, 200, 220);

	if (hcMenu) {
		static const char* MT[4] = {"", "KEY & SETTINGS", "SOUNDS & EFFECTS", "MODES & TEMPO"};
		static const u8 MC[4][3] = {{0,0,0}, {170,170,180}, {240,210,60}, {230,70,60}};
		const u8* mc = MC[hcMenu];
		rect(6, 28, 248, 124, 30, 26, 34);
		rect(6, 28, 248, 14, mc[0], mc[1], mc[2]);
		drawText(12, 31, MT[hcMenu], 1, 20, 16, 22);
		drawText(250 - 8 * 7, 31, "B:close", 1, 20, 16, 22);
		int ids[12]; int n = hcMenuItems(hcMenu, ids);
		for (int i = 0; i < n; i++) {
			int y = 47 + i * 14;
			char v[24]; hcMenuValue(ids[i], v, sizeof(v));
			if (i == hcMenuSel) rect(8, y - 3, 244, 13, mc[0] / 3, mc[1] / 3, mc[2] / 3);
			drawText(14, y, MI_LABEL[ids[i]], 1, i == hcMenuSel ? 255 : 170, i == hcMenuSel ? 255 : 160, i == hcMenuSel ? 255 : 180);
			drawText(250 - 8 - strlen(v) * 8, y, v, 1, 255, 200, 120);
		}
	} else {
		int sc = strlen(hcName) <= 6 ? 4 : (strlen(hcName) <= 9 ? 3 : 2);
		drawTextC(128, 34, hcName, sc, 255, 255, 255);
		if (hcSub[0]) drawTextC(128, 34 + 8 * sc + 4, hcSub, 1, 255, 210, 150);
		char line[80] = ""; int len = 0;
		int sorted[MAXN]; memcpy(sorted, hcNotes, sizeof(int) * hcN); qsort(sorted, hcN, sizeof(int), cmpInt);
		for (int i = 0; i < hcN && len < 60; i++)
			len += snprintf(line + len, sizeof(line) - len, "%s%d ", NOTE_NAMES[sorted[i] % 12], sorted[i] / 12 - 1);
		drawTextC(128, 86, line, 1, 200, 190, 210);
		char r[64];
		snprintf(r, sizeof(r), "%s %s  OCT %+d", NOTE_NAMES[hcKey], SCALE_NAMES[hcScale], hcOct);
		drawTextC(128, 104, r, 1, 255, 200, 120);
		snprintf(r, sizeof(r), "%s  %s", PATCHES[patchIdx].name, adsrSel ? ADSR_NAMES[adsrSel] : "");
		drawTextC(128, 118, r, 1, 255, 200, 120);
		snprintf(r, sizeof(r), "BASS %s  %s", BASS_NAMES[hcBass], hcVlead ? "VLEAD AUTO" : INV_NAMES[hcInv]);
		drawTextC(128, 132, r, 1, 255, 200, 120);
	}
	if (!audioOK) drawTextC(200, 146, "NO AUDIO - run DSP1 once (dspfirm.cdc)", 1, 255, 80, 80);

	// joystick compass
	int eff = hcEffDir(hcDeg >= 0 ? hcDeg : hcLastDeg);
	int cx0 = 258, cy0 = 42, cw = 46, ch = 34;
	drawTextC(cx0 + cw * 3 / 2, 30, hcMode == HM_LEAD ? "LEAD" : MAP_NAMES[hcMap], 1, 150, 140, 170);
	for (int d = 0; d < 8; d++) {
		int x = cx0 + DIR_COL[d] * cw, y = cy0 + DIR_ROW[d] * ch;
		int on = (d == eff);
		if (on) rect(x + 1, y + 1, cw - 2, ch - 2, 255, 140, 50);
		else rect(x + 1, y + 1, cw - 2, ch - 2, 40, 34, 46);
		const char* lab = SHORT_DIR[hcMap][d];
		if (hcMode == HM_LEAD) { static const char* LL[8] = {"+OCT", "+O+1", "+1", "-O+1", "-OCT", "-O-1", "-1", "+O-1"}; lab = LL[d]; }
		drawTextC(x + cw / 2, y + ch / 2 - 4, lab, 1, on ? 20 : 220, on ? 16 : 210, on ? 22 : 230);
	}
	int lk = hcLock[hcDeg >= 0 ? hcDeg : hcLastDeg];
	rect(cx0 + cw + 1, cy0 + ch + 1, cw - 2, ch - 2, 24, 20, 28);
	drawTextC(cx0 + cw + cw / 2, cy0 + ch + ch / 2 - 4, lk >= 0 ? "LOCK" : "o", 1, lk >= 0 ? 255 : 120, lk >= 0 ? 200 : 110, lk >= 0 ? 80 : 140);
}

static void hcDrawBottom(int quickMode, int quickSound) {
	fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320;
	rect(0, 0, 320, PLATE_Y, 16, 14, 18);
	for (int i = 0; i < 7; i++) {
		int x0 = i * 320 / 7, x1 = (i + 1) * 320 / 7, w = x1 - x0;
		const u8* col = PADCOL[i];
		int act = (i == hcDeg);
		float k = act ? 1.0f : 0.38f;
		rect(x0 + 2, 3, w - 4, JPAD_H - 6, col[0] * k, col[1] * k, col[2] * k);
		if (act) frame(x0 + 2, 3, w - 4, JPAD_H - 6, 255, 255, 255);
		char num[4]; snprintf(num, sizeof(num), "%d", i + 1);
		drawTextC(x0 + w / 2, 8, num, 1, 255, 240, 220);
		if (quickMode) {
			if (i < HM_COUNT) { char m[8]; snprintf(m, sizeof(m), "%.5s", HM_NAMES[i]); drawTextC(x0 + w / 2, 34, m, 1, 255, 255, 255); }
			continue;
		}
		if (quickSound) {
			static const char* QS[7] = {"SAW", "SINE", "FM EP", "FMPNO", "STRNG", "CLAR", "WOW"};
			const char* m = QS[i];
			drawTextC(x0 + w / 2, 34, m, 1, 255, 255, 255);
			continue;
		}
		JChord c; joyBuild(&c, hcScale, i, hcMap, hcLock[i]);
		char name[16]; joyName(&c, hcKey, name, sizeof(name));
		if (strlen(name) > 5) { drawTextC(x0 + w / 2, 30, name, 1, 255, 255, 255); }
		else drawTextC(x0 + w / 2, 34, name, 1, 255, 255, 255);
		if (hcLock[i] >= 0) drawTextC(x0 + w / 2, 54, "LOCK", 1, 255, 220, 120);
	}
	drawPlate(hcStrings, hcNs, hcFlash);
	rect(0, 200, 320, 40, 30, 26, 34);
	char st[48];
	snprintf(st, sizeof(st), "%s  %s %s  %s", HM_NAMES[hcMode], NOTE_NAMES[hcKey], SCALE_NAMES[hcScale], PATCHES[patchIdx].name);
	drawText(6, 206, st, 1, 255, 200, 120);
	drawText(6, 222, quickMode ? "A held: tap 1-6 = mode" : quickSound ? "Y held: tap 1-7 = sound"
		: "X:key Y:snd A:mode L:lock R:inv", 1, 170, 160, 180);
}

// ---- per-frame logic
static void hcFrame(u32 kDown, u32 kHeld, u32 kUp, int touching, const touchPosition* t, float cx, float cy,
                    int* dirty, int* topDirty) {
	u64 now = osGetTime();
	static const u32 MENU_BTN[3] = {KEY_X, KEY_Y, KEY_A};

	// quick select views redraw when A/Y go down/up
	if ((kDown | kUp) & (KEY_A | KEY_Y)) *dirty = 1;

	for (int m = 0; m < 3; m++) if (kDown & MENU_BTN[m]) {
		if (hcMenu == m + 1) hcMenu = 0; else { hcMenu = m + 1; hcMenuSel = 0; }
		hcMenuTime = now; *topDirty = 1;
	}
	if (kDown & KEY_B) { hcMenu = 0; *topDirty = 1; }
	if (hcMenu && now - hcMenuTime > 5000) { hcMenu = 0; *topDirty = 1; }

	if (hcMenu) {
		int ids[12]; int n = hcMenuItems(hcMenu, ids);
		if (kDown & KEY_DUP)   { hcMenuSel = wrap(hcMenuSel - 1, n); hcMenuTime = now; *topDirty = 1; }
		if (kDown & KEY_DDOWN) { hcMenuSel = wrap(hcMenuSel + 1, n); hcMenuTime = now; *topDirty = 1; }
		int d = (kDown & KEY_DRIGHT) ? 1 : (kDown & KEY_DLEFT) ? -1 : 0;
		if (d) { hcMenuChange(ids[hcMenuSel], d); hcMenuTime = now; *topDirty = *dirty = 1; }
	} else {
		if (kDown & KEY_DRIGHT) { hcKey = wrap(hcKey + 1, 12); *dirty = *topDirty = 1; }
		if (kDown & KEY_DLEFT)  { hcKey = wrap(hcKey - 1, 12); *dirty = *topDirty = 1; }
		if (kDown & KEY_DUP)    { if (hcOct < 2) hcOct++; *topDirty = 1; }
		if (kDown & KEY_DDOWN)  { if (hcOct > -2) hcOct--; *topDirty = 1; }
	}

	// joystick
	hcDir = -1;
	if (cx * cx + cy * cy > 0.30f) {
		float a = atan2f(cx, cy); if (a < 0) a += TWO_PI;     // 0 = up, clockwise
		hcDir = ((int)floorf((a + TWO_PI / 16.0f) / (TWO_PI / 8.0f))) & 7;
	}

	int deg = hcDeg >= 0 ? hcDeg : hcLastDeg;
	// L = joystick click: lock the pushed modifier on this chord (or unlock with stick centred)
	if (kDown & KEY_L) {
		hcLock[deg] = hcDir >= 0 ? (hcLock[deg] == hcDir ? -1 : hcDir) : -1;
		*dirty = *topDirty = 1;
	}
	// R = inversion control
	if (kDown & KEY_R) { hcInv = (hcInv + 1) % 3; *topDirty = 1; if (hcDeg >= 0) hcPrevCode = -99; }

	// touch
	int padHit = -1, plateHit = -1;
	if (touching && t->py < JPAD_H) padHit = t->px * 7 / 320;
	if (touching && t->py >= PLATE_Y && t->py < PLATE_Y + PLATE_H) {
		plateHit = stringAt(t->px, hcNs);
	}
	int touchDown = touching && !hcPrevTouch;
	if (!touching) hcQuick = 0;

	if (touchDown && padHit >= 0 && (kHeld & (KEY_A | KEY_Y))) {     // quick select
		if (kHeld & KEY_A) { if (padHit < HM_COUNT) hcSetMode(padHit); }
		else if (padHit < NPATCH) patchIdx = padHit;
		hcQuick = 1; hcMenu = 0; *dirty = *topDirty = 1;
	}

	if (!hcQuick) {
		if (padHit >= 0 && padHit != hcDeg) {                        // press (or slide to) a chord button
			hcDeg = padHit; hcLastDeg = padHit;
			hcPlay(hcDeg, 0);
			hcPrevCode = hcEffDir(hcDeg) * 16 + hcDir + hcInv * 256;
			*dirty = *topDirty = 1;
		} else if (hcDeg >= 0 && padHit < 0) {                      // release
			hcRelease(); hcDeg = -1; *dirty = *topDirty = 1;
		}
	}
	if (hcDeg >= 0) {
		int code = hcEffDir(hcDeg) * 16 + hcDir + hcInv * 256;
		if (code != hcPrevCode) { hcPlay(hcDeg, 1); hcPrevCode = code; *topDirty = 1; }
	} else {
		int code = hcEffDir(hcLastDeg) * 16 + hcDir;
		static int idleCode = -99;
		if (code != idleCode) { idleCode = code; *topDirty = 1; }
	}

	// strum plate follows the held (or last) chord with its modifier
	JChord jc; joyBuild(&jc, hcScale, deg, hcMap, hcEffDir(deg));
	int ns2[NSTRINGS]; int n2 = joyStrings(&jc, hcKey, hcOct, ns2);
	if (n2 != hcNs || memcmp(ns2, hcStrings, sizeof(int) * n2)) { memcpy(hcStrings, ns2, sizeof(ns2)); hcNs = n2; hcPlateDirty = 1; }
	if (plateHit >= 0 && !hcQuick) {
		float vel = 0.55f + 0.45f * (1.0f - (t->py - PLATE_Y) / (float)PLATE_H);
		int list[NSTRINGS], cnt = 0;
		if (!hcInPlate || hcPrevString < 0) list[cnt++] = plateHit;
		else if (plateHit > hcPrevString) for (int i = hcPrevString + 1; i <= plateHit; i++) list[cnt++] = i;
		else if (plateHit < hcPrevString) for (int i = hcPrevString - 1; i >= plateHit; i--) list[cnt++] = i;
		for (int k = 0; k < cnt; k++) {
			if (list[k] >= hcNs) continue;
			Voice* v = noteOn(hcStrings[list[k]], k * (int)(0.004f * SR), vel);
			v->tag = 2; v->gate = (int)(0.9f * SR) + k * (int)(0.004f * SR);
			hcFlash[list[k]] = 10; hcPlateDirty = 1;
		}
		hcInPlate = 1; hcPrevString = plateHit;
	} else { hcInPlate = 0; hcPrevString = -1; }
	for (int i = 0; i < NSTRINGS; i++) if (hcFlash[i] > 0) { hcFlash[i]--; hcPlateDirty = 1; }

	hcPrevTouch = touching;
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

	int key = 0, minor = 0, oct = 0, voicing = V_CLOSE, mode = M_JOY;
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
		int express = (mode == M_EXPRESS), joy = (mode == M_JOY);

		int add7 = 0, add9 = 0, sus = 0;
		if (!express && !joy) { add7 = !!(kHeld & KEY_L); add9 = !!(kHeld & KEY_R); sus = !!(kHeld & KEY_X); }

		if (!joy) {
			if (kDown & KEY_DRIGHT) { key = (key + 1) % 12; dirty = topDirty = 1; }
			if (kDown & KEY_DLEFT)  { key = (key + 11) % 12; dirty = topDirty = 1; }
			if (kDown & KEY_DUP)    { if (oct < 2) oct++; topDirty = 1; }
			if (kDown & KEY_DDOWN)  { if (oct > -2) oct--; topDirty = 1; }
			if (kDown & KEY_Y)      { patchIdx = (patchIdx + 1) % NPATCH; dirty = topDirty = 1; }
		}
		if (joy) {
			// handled in hcFrame
		} else if (express) {
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
			express = (mode == M_EXPRESS); joy = (mode == M_JOY);
			adsrEnabled = joy; relCap = 0.0f;
			if (joy) { hcSetMode(hcMode); hcDeg = -1; }
			dirty = topDirty = 1;
		}

		circlePosition cp; hidCircleRead(&cp);
		float cy = cp.dy / 156.0f, cx = cp.dx / 156.0f;
		if (fabsf(cy) < 0.08f) cy = 0;
		if (fabsf(cx) < 0.08f) cx = 0;
		cutoffMul = joy ? 1.0f : powf(2.0f, cy * 2.5f);

		// touch
		touchPosition t; int touching = !!(kHeld & KEY_TOUCH);
		if (touching) hidTouchRead(&t);
		int ph = padH(mode);
		int touchPad = -1;
		if (touching && t.py < 200) touchPad = (t.py / ph) * 4 + (t.px / 80);

		if (joy) {
			bendSemis = 0;
			hcFrame(kDown, kHeld, kUp, touching, &t, cx, cy, &dirty, &topDirty);
			activePad = hcDeg;
		} else if (express) {
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
			char r1[64], r2[64];
			if (joy) {
				r1[0] = r2[0] = 0;
			} else if (express) {
				snprintf(r1, sizeof(r1), "SET %s  TRANSPOSE %+d  OCT %+d  INV %+d", XSETS[xset].name, key > 5 ? key - 12 : key, oct, xinv);
				snprintf(r2, sizeof(r2), "SOUND: %s", PATCHES[patchIdx].name);
			} else {
				snprintf(r1, sizeof(r1), "KEY %s %s  OCT %+d  %s", NOTE_NAMES[key], minor ? "MIN" : "MAJ", oct, VOICING_NAMES[voicing]);
				snprintf(r2, sizeof(r2), "SOUND: %s", PATCHES[patchIdx].name);
			}
			if (joy) hcDrawTop(audioOK);
			else drawTopInfo(audioOK, mode, chordName, roman, curNotes, curN, r1, r2);
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
		if (joy && (dirty || hcPlateDirty)) {
			if (dirty) hcDrawBottom(!!(kHeld & KEY_A), !!(kHeld & KEY_Y) && !(kHeld & KEY_A));
			else drawPlate(hcStrings, hcNs, hcFlash);
			dirty = 0; hcPlateDirty = 0;
		}
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
