// PhotoTape - every photo is a synth patch. Nintendo 2DS / 3DS homebrew (libctru)
//
// Snap a photo with the camera. The picture becomes:
//   * a TAPE  - the image rendered to audio (SPECTRAL / WAVE / SCAN), played by a tape looper
//   * GRAINS  - a granular cloud scrubbing through the tape (or your mic sample)
//   * DRUMS   - kick / snare / hat tuned from the photo's colour + contrast, PERC = a slice of the tape
//   * SYNTH   - 64 wavetables cut from the photo's columns, envelope + filter from its look
//   * a SCALE - root note from the dominant hue, mode from brightness + warmth
// A 7-track, 16-step sequencer plays it all. The MIC page records samples with a basic editor.
//
// Pages (SELECT or tap the tabs): CAM  TAPE  GRAIN  SEQ  MIC  KEYS
// START = play / stop sequencer.   Hold SELECT + press START = quit.
// Files live in sdmc:/3ds/phototape  (P####.bmp photo, P####.pat patch, S####.wav samples)

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <dirent.h>
#include <sys/stat.h>

#define SR          32000
#define BUF_SAMPLES 512
#define NBUFS       4
#define PW          400
#define PH          240
#define TAPE_MAX    (SR * 6)
#define SMP_MAX     (SR * 6)
#define MIC_RATE    32730.0f
#define RAW_MAX     (32730 * 6)
#define NTAB        64
#define TABN        256
#define NTR         7
#define NSTEP       16
#define MAXGR       40
#define NSYN        4
#define NSMPV       4
#define PT_DIR      "sdmc:/3ds/phototape"
#define TWO_PI_F    6.28318530718f

static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

// ---------------------------------------------------------------- random / tables
static u32 rngMain = 0x2545F491, rngAud = 0x9E3779B9;
static inline float frandM(void) { rngMain = rngMain * 1664525u + 1013904223u; return (rngMain >> 8) * (1.0f / 16777216.0f); }
static inline float frandA(void) { rngAud = rngAud * 1664525u + 1013904223u; return (rngAud >> 8) * (1.0f / 16777216.0f); }
static inline float nrandA(void) { return frandA() * 2.0f - 1.0f; }

static float sinT[4097];
static float hannT[1025];
static inline float fsin(float ph) {   // sin(2*pi*ph), ph >= 0
	float p = ph * 4096.0f; int i = (int)p; float f = p - i; i &= 4095;
	return sinT[i] + (sinT[i + 1] - sinT[i]) * f;
}
static inline float softclip(float x) {
	if (x > 3.0f) return 1.0f; if (x < -3.0f) return -1.0f;
	return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}
static inline float mtof(float m) { return 440.0f * powf(2.0f, (m - 69.0f) / 12.0f); }

// ---------------------------------------------------------------- scales
typedef struct { const char* name; int n; int iv[7]; } Scale;
static const Scale SCALES[8] = {
	{"PHRYGIAN", 7, {0,1,3,5,7,8,10}},
	{"HARM MIN", 7, {0,2,3,5,7,8,11}},
	{"MINOR",    7, {0,2,3,5,7,8,10}},
	{"DORIAN",   7, {0,2,3,5,7,9,10}},
	{"PENT MIN", 5, {0,3,5,7,10}},
	{"MIXOLYD",  7, {0,2,4,5,7,9,10}},
	{"PENT MAJ", 5, {0,2,4,7,9}},
	{"LYDIAN",   7, {0,2,4,6,7,9,11}},
};
static const char* NOTE_NAMES[12] = {"C","C#","D","Eb","E","F","F#","G","Ab","A","Bb","B"};

// ---------------------------------------------------------------- the patch (derived from the photo)
typedef struct {
	float r, g, b, lum, contrast, sat, hue, edge;
	int root, scale;
	float kickF0, kickF1, kickPD, kickAD, kickClick;
	float snTone, snNoise, snDecay, snBP;
	float hatBase, hatDecay;
	int percCol; float percDecay;
	float synA, synD, synS, synR, synCut, synRes, synEnv, synScan, synMove;
} Patch;
static Patch P;

static inline int degSemi(int d) {
	int n = SCALES[P.scale].n;
	int o = d >= 0 ? d / n : -((-d + n - 1) / n);
	return o * 12 + SCALES[P.scale].iv[d - o * n];
}

// ---------------------------------------------------------------- buffers
static u8 photo[PH][PW][3];
static u8 luma[PH][PW];
static u8* topCache;                 // photo pre-rotated in top framebuffer layout
static u8* topDim;                   // same, darkened for overlay pages
static u8 stripCache[80][320][3];    // photo squashed to the bottom-screen tape strip
static float* wtab;                  // NTAB * TABN wavetables from photo columns
static s16* tape; static volatile int tapeLen = 0;
static s16* smp;  static s16* smpUndo;
static volatile int smpLen = 0, smpS = 0, smpE = 0;
static int smpUndoLen = 0, smpUndoS = 0, smpUndoE = 0, haveUndo = 0;
static s16* rawMic; static int rawLen = 0;
static u8 tapePeak[320], smpPeak[400];
static int photoId = 0;              // 0 = built-in picture

// ---------------------------------------------------------------- shared engine state
enum { T_KICK, T_SNARE, T_HAT, T_PERC, T_SYNTH, T_GRAIN, T_SAMPLE };
static const char* TR_SHORT[NTR] = {"KCK","SNR","HAT","PRC","SYN","GRN","SMP"};
static const char* TR_LONG[NTR]  = {"KICK","SNARE","HAT","PERC","SYNTH","GRAIN","SAMPLE"};
static const u8 TR_COL[NTR][3] = {
	{235,90,60},{240,170,60},{230,220,90},{120,210,110},{80,170,240},{170,120,240},{240,110,190}
};
typedef struct { u8 on; s8 val; } Step;
static Step seq[NTR][NSTEP];
static float trTune[NTR], trDecay[NTR];
static u8 trMute[NTR];
static volatile int tempo = 96;
static volatile float swing = 0.0f;
static volatile int playing = 0, curStep = -1, seqResetReq = 0;
static volatile float stepFrac = 0.0f;

static float tp[6] = {0.5f, 0.15f, 0.3f, 0.1f, 0.75f, 0.8f};   // SPEED WOW DRIVE HISS TONE LEVEL
static float gp[6] = {0.35f, 0.45f, 0.25f, 0.5f, 0.0f, 0.7f};  // SIZE DENS SPRAY PITCH REV LEVEL
static const char* TP_NAMES[6] = {"SPEED","WOW","DRIVE","HISS","TONE","LEVEL"};
static const char* GP_NAMES[6] = {"SIZE","DENS","SPRAY","PITCH","REV","LEVEL"};
static volatile int tapeRun = 0, tapeDir = 1, tapeSync = 0;
static volatile float loopS = 0.0f, loopE = 1.0f, tapeJumpReq = -1.0f, tapePosFrac = 0.0f;
static const int SYNC_BARS[4] = {0, 1, 2, 4};
static volatile int grainSrc = 0, grainTouch = 0, grainLatch = 0;
static volatile float grainPos = 0.5f;
static float grainVisPos[MAXGR]; static volatile int grainVisOn[MAXGR];
static volatile int holdReq = 0, holdAck = 0;
static int audioOK = 0;
enum { TM_SPECTRAL, TM_WAVE, TM_SCAN, TM_MIC, TM_COUNT };
static const char* TM_NAMES[TM_COUNT] = {"SPECTRAL","WAVE","SCAN","MIC"};
static int tapeMode = TM_SPECTRAL;

// events main -> audio (single producer / single consumer)
enum { EV_DRUM, EV_SYN_ON, EV_SYN_OFF, EV_GRAIN_ON, EV_GRAIN_OFF, EV_SMP };
typedef struct { u8 type, tr; s8 val; } Ev;
static Ev evq[64]; static volatile int evW = 0, evR = 0;
static void pushEv(int type, int tr, int val) {
	int n = (evW + 1) & 63; if (n == evR) return;
	evq[evW].type = type; evq[evW].tr = tr; evq[evW].val = val;
	__sync_synchronize(); evW = n;
}

// ---------------------------------------------------------------- voices
typedef struct { int on; float ph, env, penv, cenv, vel, f0, f1, pdk, adk; } Kick;
typedef struct { int on; float ph, f, tenv, nenv, tdk, ndk, vel, lp, bp, bf, mix; } Snare;
typedef struct { int on; float ph[6], f, env, dk, vel, h1, x1, h2, x2; } Hat;
typedef struct { int on; float pos, inc, env, dk, vel; } Perc;
typedef struct { int on, stage, note, gate; float freq, ph, env, wpos, wdir, lp, bp, vel; u32 age; } SynV;
typedef struct { int on, age, len, src, rs, re; float pos, inc, amp, pl, pr; } Grain;
typedef struct { int on; float pos, inc, env, dk, end, vel; } SmpV;

static Kick kick; static Snare snare; static Hat hat; static Perc perc;
static SynV syn[NSYN]; static Grain grains[MAXGR]; static SmpV smpv[NSMPV];
static u32 synAge = 0;
static int burstLeft = 0, burstSemi = 0, keysGrain = 0, keysSemi = 0;
static float grainTimer = 0;
static double tapePosD = 0; static float tapeMul = 0, wowPh = 0, flutPh = 0, toneLP = 0;
static float stepCountdown = 0, stepLen = 1; static int stepIdx = -1;

static float tuneMul(int tr, float oct) { return powf(2.0f, (trTune[tr] - 0.5f) * 2.0f * oct); }
static float decMul(int tr) { return powf(2.0f, (trDecay[tr] - 0.5f) * 3.0f); }

static void trigKick(float vel) {
	float tm = tuneMul(T_KICK, 1.0f);
	kick.on = 1; kick.env = 1; kick.penv = 1; kick.cenv = 1; kick.ph = 0; kick.vel = vel;
	kick.f0 = P.kickF0 * tm; kick.f1 = P.kickF1 * tm;
	kick.pdk = expf(-1.0f / (SR * P.kickPD));
	kick.adk = expf(-1.0f / (SR * P.kickAD * decMul(T_KICK)));
}
static void trigSnare(float vel) {
	float tm = tuneMul(T_SNARE, 1.0f), dm = decMul(T_SNARE);
	snare.on = 1; snare.vel = vel; snare.ph = 0; snare.f = P.snTone * tm;
	snare.tenv = 1; snare.nenv = 1; snare.mix = P.snNoise;
	snare.tdk = expf(-1.0f / (SR * P.snDecay * 0.5f * dm));
	snare.ndk = expf(-1.0f / (SR * P.snDecay * dm));
	snare.bf = 2.0f * sinf(3.14159f * clampf(P.snBP * tm, 200, 5000) / SR);
}
static void trigHat(float vel) {
	hat.on = 1; hat.vel = vel; hat.env = 1; hat.f = P.hatBase * tuneMul(T_HAT, 1.0f);
	hat.dk = expf(-1.0f / (SR * P.hatDecay * decMul(T_HAT)));
}
static void trigPerc(float vel) {
	if (tapeLen < 1000) return;
	perc.on = 1; perc.vel = vel; perc.env = 1;
	perc.pos = (float)P.percCol / PW * (tapeLen - 1);
	perc.inc = tuneMul(T_PERC, 1.0f);
	perc.dk = expf(-1.0f / (SR * P.percDecay * decMul(T_PERC)));
}
static void synOn(int deg, int gate, float vel) {
	int vi = -1; u32 oldest = 0xffffffff;
	for (int i = 0; i < NSYN; i++) if (!syn[i].on) { vi = i; break; }
	if (vi < 0) for (int i = 0; i < NSYN; i++) if (syn[i].age < oldest) { oldest = syn[i].age; vi = i; }
	SynV* v = &syn[vi];
	if (!v->on) { v->env = 0; v->lp = v->bp = 0; v->ph = frandA(); }
	v->on = 1; v->stage = 0; v->note = deg; v->gate = gate; v->vel = vel; v->age = ++synAge;
	v->freq = mtof(48 + P.root + degSemi(deg));
	float w = P.synScan * (NTAB - 1) + (trTune[T_SYNTH] - 0.5f) * NTAB;
	while (w < 0) w += NTAB - 1;
	while (w > NTAB - 1) w -= NTAB - 1;
	v->wpos = w; v->wdir = 1;
}
static void synOff(int deg) {
	for (int i = 0; i < NSYN; i++) if (syn[i].on && syn[i].note == deg && syn[i].gate < 0) syn[i].stage = 3;
}
static void smpOn(int semi, float vel) {
	if (smpE - smpS < 64) return;
	int vi = 0; float best = 2;
	for (int i = 0; i < NSMPV; i++) { if (!smpv[i].on) { vi = i; best = -1; break; } if (smpv[i].env < best) { best = smpv[i].env; vi = i; } }
	SmpV* v = &smpv[vi];
	v->on = 1; v->pos = smpS; v->end = smpE; v->env = 1; v->vel = vel;
	v->inc = powf(2.0f, semi / 12.0f) * tuneMul(T_SAMPLE, 1.0f);
	v->dk = trDecay[T_SAMPLE] > 0.95f ? 1.0f : expf(-1.0f / (SR * (0.03f + trDecay[T_SAMPLE] * 1.5f)));
}

static void trigger(int tr, int val, int gate) {
	float vel = 0.2f + 0.8f * clampf(val, 0, 15) / 15.0f;
	switch (tr) {
	case T_KICK:  trigKick(vel); break;
	case T_SNARE: trigSnare(vel); break;
	case T_HAT:   trigHat(vel); break;
	case T_PERC:  trigPerc(vel); break;
	case T_SYNTH: synOn(val, gate, 0.9f); break;
	case T_GRAIN: burstLeft = gate + SR / 20; burstSemi = degSemi(val); grainTimer = 0; break;
	case T_SAMPLE: smpOn(degSemi(val), 0.9f); break;
	}
}

static void spawnGrain(int semi) {
	int gi = -1;
	for (int i = 0; i < MAXGR; i++) if (!grains[i].on) { gi = i; break; }
	if (gi < 0) return;
	Grain* g = &grains[gi];
	int src = 0, rs = 0, re = tapeLen;
	if (grainSrc == 1 && smpE - smpS > 512) { src = 1; rs = smpS; re = smpE; }
	int rl = re - rs; if (rl < 512) return;
	float spray = gp[2];
	float c = rs + grainPos * rl + spray * nrandA() * rl * 0.25f;
	while (c < rs) c += rl;
	while (c >= re) c -= rl;
	g->src = src; g->rs = rs; g->re = re; g->pos = c; g->age = 0;
	g->len = (int)(SR * 0.01f * powf(2.0f, gp[0] * 6.0f));
	g->inc = powf(2.0f, (roundf((gp[3] - 0.5f) * 48.0f) + semi) / 12.0f);
	if (frandA() < gp[4]) g->inc = -g->inc;
	float pan = clampf(0.5f + nrandA() * spray * 0.5f, 0, 1);
	g->pl = sqrtf(1 - pan); g->pr = sqrtf(pan);
	g->amp = gp[5] * 0.55f; g->on = 1;
	grainVisPos[gi] = (c - rs) / rl; grainVisOn[gi] = 1;
}

static void processEvents(void) {
	while (evR != evW) {
		Ev e = evq[evR]; evR = (evR + 1) & 63;
		switch (e.type) {
		case EV_DRUM: trigger(e.tr, e.val, SR / 8); break;
		case EV_SYN_ON: synOn(e.val, -1, 0.9f); break;
		case EV_SYN_OFF: synOff(e.val); break;
		case EV_GRAIN_ON: keysGrain = 1; keysSemi = degSemi(e.val); grainTimer = 0; break;
		case EV_GRAIN_OFF: keysGrain = 0; break;
		case EV_SMP: smpOn(degSemi(e.val), 0.9f); break;
		}
	}
}

static void fireStep(int s, float sps) {
	for (int tr = 0; tr < NTR; tr++)
		if (seq[tr][s].on && !trMute[tr]) trigger(tr, seq[tr][s].val, (int)(sps * 0.85f));
}

static void fillAudio(s16* out, int n) {
	if (holdReq) { holdAck = 1; memset(out, 0, n * 4); return; }
	processEvents();

	// per-buffer constants
	float sps = SR * 60.0f / tempo / 4.0f;
	int tl = tapeLen;
	float speed = powf(2.0f, (tp[0] - 0.5f) * 4.0f);
	int ls = (int)(loopS * tl), le = (int)(loopE * tl);
	if (le > tl) le = tl;
	if (le - ls < 256) { le = ls + 256; if (le > tl) { le = tl; ls = le - 256; if (ls < 0) ls = 0; } }
	if (tapeSync && tl > 0) speed = (float)(le - ls) / (SYNC_BARS[tapeSync] * 16.0f * sps);
	float wowD = tp[1] * 0.02f, flutD = tp[1] * 0.004f;
	float drive = 1.0f + tp[2] * 8.0f, driveComp = 1.0f / powf(drive, 0.6f);
	float hiss = tp[3] * 0.05f;
	float toneA = 1.0f - expf(-TWO_PI_F * 400.0f * powf(2.0f, tp[4] * 5.5f) / SR);
	float tLevel = tp[5];
	if (tapeJumpReq >= 0) { tapePosD = tapeJumpReq * tl; tapeJumpReq = -1; }
	if (tl > 0 && (tapePosD < 0 || tapePosD >= tl)) tapePosD = ls;
	float dens = 2.0f * powf(2.0f, gp[1] * 5.3f);
	int cloudSemiBase = 0;
	float ds = decMul(T_SYNTH);
	float aInc = 1.0f / (SR * P.synA), sdk = 1.0f - expf(-1.0f / (SR * P.synD * ds)), rk = expf(-1.0f / (SR * P.synR * ds));
	float wspeed = P.synMove * 24.0f / SR;
	float sq = 1.9f - 1.75f * P.synRes;

	for (int i = 0; i < n; i++) {
		// sequencer
		if (seqResetReq) {
			seqResetReq = 0; stepCountdown = 0; stepIdx = -1;
			if (tapeSync && tl > 0) tapePosD = tapeDir > 0 ? ls : le - 1;
		}
		if (playing) {
			if (stepCountdown <= 0) {
				stepIdx = (stepIdx + 1) % NSTEP; curStep = stepIdx;
				stepLen = sps * (1.0f + ((stepIdx & 1) ? -swing : swing) * 0.66f);
				stepCountdown += stepLen;
				if (stepIdx == 0 && tapeSync && tl > 0 && tapeRun) tapePosD = tapeDir > 0 ? ls : le - 1;
				fireStep(stepIdx, sps);
			}
			stepCountdown -= 1.0f;
		}

		float L = 0, R = 0, x;

		// kick
		if (kick.on) {
			float f = kick.f1 + (kick.f0 - kick.f1) * kick.penv;
			kick.ph += f / SR; if (kick.ph >= 1) kick.ph -= 1;
			x = fsin(kick.ph) * kick.env + nrandA() * kick.cenv * P.kickClick * 0.35f;
			kick.penv *= kick.pdk; kick.env *= kick.adk; kick.cenv *= 0.985f;
			if (kick.env < 0.0005f) kick.on = 0;
			x *= kick.vel * 0.95f; L += x; R += x;
		}
		// snare
		if (snare.on) {
			snare.ph += snare.f * (1.0f + snare.tenv * 0.5f) / SR; if (snare.ph >= 1) snare.ph -= 1;
			float nz = nrandA();
			snare.lp += snare.bf * snare.bp;
			float hp = nz - snare.lp - 0.7f * snare.bp;
			snare.bp += snare.bf * hp;
			x = fsin(snare.ph) * snare.tenv * (1.0f - snare.mix * 0.5f) + (snare.bp * 0.8f + hp * 0.3f) * snare.nenv * snare.mix;
			snare.tenv *= snare.tdk; snare.nenv *= snare.ndk;
			if (snare.nenv < 0.0005f && snare.tenv < 0.0005f) snare.on = 0;
			x *= snare.vel * 0.7f; L += x; R += x;
		}
		// hat
		if (hat.on) {
			static const float rat[6] = {1.0f, 1.342f, 1.2312f, 1.6532f, 1.9523f, 2.1523f};
			float s = 0;
			for (int k = 0; k < 6; k++) { hat.ph[k] += hat.f * rat[k] / SR; if (hat.ph[k] >= 1) hat.ph[k] -= 1; s += hat.ph[k] < 0.5f ? 1 : -1; }
			s = s * 0.16f + nrandA() * 0.25f;
			hat.h1 = 0.46f * (hat.h1 + s - hat.x1); hat.x1 = s;
			hat.h2 = 0.46f * (hat.h2 + hat.h1 - hat.x2); hat.x2 = hat.h1;
			x = hat.h2 * hat.env * hat.vel * 0.8f;
			hat.env *= hat.dk; if (hat.env < 0.0005f) hat.on = 0;
			L += x * 0.8f; R += x;
		}
		// perc: a slice of the tape
		if (perc.on) {
			int p0 = (int)perc.pos;
			if (p0 >= tl - 1 || tl < 2) perc.on = 0;
			else {
				float fr = perc.pos - p0;
				x = (tape[p0] + (tape[p0 + 1] - tape[p0]) * fr) * (1.0f / 32768.0f);
				x = softclip(x * 2.0f) * perc.env * perc.vel;
				perc.pos += perc.inc; perc.env *= perc.dk;
				if (perc.env < 0.0005f) perc.on = 0;
				L += x; R += x * 0.8f;
			}
		}
		// synth
		for (int v = 0; v < NSYN; v++) {
			SynV* s = &syn[v];
			if (!s->on) continue;
			if (s->stage == 0) { s->env += aInc; if (s->env >= 1) { s->env = 1; s->stage = 1; } }
			else if (s->stage == 1) s->env += (P.synS - s->env) * sdk;
			else if (s->stage == 3) { s->env *= rk; if (s->env < 0.0005f) { s->on = 0; continue; } }
			if (s->gate > 0) { s->gate--; if (s->gate == 0) s->stage = 3; }
			s->wpos += s->wdir * wspeed;
			if (s->wpos >= NTAB - 1) { s->wpos = NTAB - 1.001f; s->wdir = -1; }
			if (s->wpos < 0) { s->wpos = 0; s->wdir = 1; }
			int t0 = (int)s->wpos; float ft = s->wpos - t0;
			const float* A = wtab + t0 * TABN; const float* B = wtab + (t0 + 1) * TABN;
			float p = s->ph * TABN; int i0 = (int)p; float fr = p - i0; i0 &= TABN - 1; int i1 = (i0 + 1) & (TABN - 1);
			float a = A[i0] + (A[i1] - A[i0]) * fr, b = B[i0] + (B[i1] - B[i0]) * fr;
			x = a + (b - a) * ft;
			s->ph += s->freq / SR; if (s->ph >= 1) s->ph -= 1;
			float fc = clampf(P.synCut * (1.0f + P.synEnv * 3.0f * s->env), 40.0f, 5000.0f);
			float f = 2.0f * fsin(fc / (2.0f * SR));
			s->lp += f * s->bp; float hp = x - s->lp - sq * s->bp; s->bp += f * hp;
			x = s->lp * s->env * s->vel * 0.32f;
			L += x; R += x;
		}
		// grain cloud
		int cloud = grainTouch || grainLatch || burstLeft > 0 || keysGrain;
		if (burstLeft > 0) burstLeft--;
		if (cloud && tl > 0) {
			grainTimer -= 1.0f;
			if (grainTimer <= 0) {
				grainTimer += SR / dens * (0.7f + 0.6f * frandA());
				int semi = cloudSemiBase + (burstLeft > 0 ? burstSemi : (keysGrain ? keysSemi : 0));
				spawnGrain(semi);
			}
		}
		for (int gi = 0; gi < MAXGR; gi++) {
			Grain* g = &grains[gi];
			if (!g->on) continue;
			const s16* buf = g->src ? smp : tape;
			int rl = g->re - g->rs;
			if (rl < 2 || (!g->src && g->re > tl)) { g->on = 0; grainVisOn[gi] = 0; continue; }
			int p0 = (int)g->pos; float fr = g->pos - p0;
			int p1 = p0 + 1; if (p1 >= g->re) p1 = g->rs;
			float s = (buf[p0] + (buf[p1] - buf[p0]) * fr) * (1.0f / 32768.0f);
			s *= hannT[(g->age * 1024) / g->len] * g->amp;
			L += s * g->pl; R += s * g->pr;
			g->pos += g->inc;
			if (g->pos >= g->re) g->pos -= rl;
			if (g->pos < g->rs) g->pos += rl;
			if (++g->age >= g->len) { g->on = 0; grainVisOn[gi] = 0; }
		}
		// sampler voices
		for (int v = 0; v < NSMPV; v++) {
			SmpV* s = &smpv[v];
			if (!s->on) continue;
			int p0 = (int)s->pos;
			if (p0 >= (int)s->end - 1 || p0 >= smpLen - 1) { s->on = 0; continue; }
			float fr = s->pos - p0;
			float y = (smp[p0] + (smp[p0 + 1] - smp[p0]) * fr) * (1.0f / 32768.0f);
			float left = s->end - s->pos;
			if (left < 96) y *= left / 96.0f;
			y *= s->env * s->vel;
			s->env *= s->dk; if (s->env < 0.0005f) s->on = 0;
			s->pos += s->inc;
			L += y; R += y;
		}
		// tape looper
		if (tl > 256) {
			float target = tapeRun ? 1.0f : 0.0f;
			if (tapeMul < target) { tapeMul += 1.0f / (0.12f * SR); if (tapeMul > 1) tapeMul = 1; }
			else if (tapeMul > target) { tapeMul -= 1.0f / (0.6f * SR); if (tapeMul < 0) tapeMul = 0; }
			if (tapeMul > 0) {
				wowPh += 0.55f / SR; if (wowPh >= 1) wowPh -= 1;
				flutPh += 6.3f / SR; if (flutPh >= 1) flutPh -= 1;
				float sp = speed * tapeDir * tapeMul * (1.0f + wowD * fsin(wowPh) + flutD * fsin(flutPh));
				tapePosD += sp;
				while (tapePosD >= le) tapePosD -= (le - ls);
				while (tapePosD < ls) tapePosD += (le - ls);
				int p0 = (int)tapePosD; float fr = (float)(tapePosD - p0);
				int p1 = p0 + 1; if (p1 >= le) p1 = ls;
				x = (tape[p0] + (tape[p1] - tape[p0]) * fr) * (1.0f / 32768.0f);
				x = softclip(x * drive) * driveComp;
				x += hiss * nrandA();
				toneLP += toneA * (x - toneLP);
				float amp = tapeMul * 4.0f; if (amp > 1) amp = 1;
				x = toneLP * tLevel * amp;
				L += x; R += x;
			}
		}

		out[i * 2]     = (s16)(softclip(L * 0.68f) * 32000.0f);
		out[i * 2 + 1] = (s16)(softclip(R * 0.68f) * 32000.0f);
	}
	if (tl > 0) tapePosFrac = (float)(tapePosD / tl);
	stepFrac = 1.0f - stepCountdown / stepLen;
}

// ---------------------------------------------------------------- audio thread
static ndspWaveBuf wbuf[NBUFS];
static s16* audioMem;
static LightEvent audioEvent;
static volatile int audioRun = 1;
static void audioCallback(void* d) { (void)d; LightEvent_Signal(&audioEvent); }
static void audioThread(void* arg) {
	(void)arg;
	while (audioRun) {
		for (int i = 0; i < NBUFS; i++) {
			if (wbuf[i].status == NDSP_WBUF_DONE) {
				fillAudio((s16*)wbuf[i].data_vaddr, BUF_SAMPLES);
				DSP_FlushDataCache(wbuf[i].data_vaddr, BUF_SAMPLES * 4);
				ndspChnWaveBufAdd(0, &wbuf[i]);
			}
		}
		LightEvent_Wait(&audioEvent);
	}
}
static void audioHold(int on) {
	if (!audioOK) return;
	if (on) {
		holdAck = 0; __sync_synchronize(); holdReq = 1;
		for (int i = 0; i < 100 && !holdAck; i++) svcSleepThread(2000000LL);
	} else holdReq = 0;
}

// ---------------------------------------------------------------- photo analysis -> patch
static void rgb2hs(float r, float g, float b, float* h, float* s) {
	float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
	*s = mx > 0.001f ? d / mx : 0;
	if (d < 0.0001f) { *h = 0; return; }
	float hh;
	if (mx == r) hh = (g - b) / d; else if (mx == g) hh = 2.0f + (b - r) / d; else hh = 4.0f + (r - g) / d;
	hh *= 60.0f; if (hh < 0) hh += 360.0f;
	*h = hh;
}

static void analyzePhoto(void) {
	double sr = 0, sg = 0, sb = 0, sl = 0, sl2 = 0, ss = 0, hx = 0, hy = 0, se = 0; int cnt = 0;
	for (int y = 0; y < PH; y++)
		for (int x = 0; x < PW; x++) {
			const u8* p = photo[y][x];
			luma[y][x] = (u8)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
		}
	for (int y = 0; y < PH; y += 2)
		for (int x = 0; x < PW - 2; x += 2) {
			const u8* p = photo[y][x];
			float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f, l = luma[y][x] / 255.0f;
			float h, s; rgb2hs(r, g, b, &h, &s);
			sr += r; sg += g; sb += b; sl += l; sl2 += l * l; ss += s;
			hx += cosf(h * 0.0174533f) * s; hy += sinf(h * 0.0174533f) * s;
			se += fabsf(l - luma[y][x + 2] / 255.0f);
			cnt++;
		}
	P.r = sr / cnt; P.g = sg / cnt; P.b = sb / cnt; P.lum = sl / cnt; P.sat = ss / cnt;
	float var = sl2 / cnt - P.lum * P.lum;
	P.contrast = clampf(sqrtf(fmaxf(var, 0)) * 3.0f, 0, 1);
	P.edge = clampf((float)(se / cnt) * 6.0f, 0, 1);
	float hue = atan2f(hy, hx) * 57.2958f; if (hue < 0) hue += 360.0f;
	P.hue = hue;
	P.root = ((int)floorf(hue / 30.0f + 0.5f)) % 12;
	int lvl = (int)(clampf(P.lum * 1.6f, 0, 0.999f) * 4);
	P.scale = lvl * 2 + (P.r > P.b ? 1 : 0);

	// brightest-contrast column -> perc slice
	float best = -1; P.percCol = PW / 2;
	for (int x = 4; x < PW - 4; x += 2) {
		float m = 0, m2 = 0;
		for (int y = 0; y < PH; y += 4) { float l = luma[y][x] / 255.0f; m += l; m2 += l * l; }
		m /= PH / 4; m2 /= PH / 4;
		float v = m2 - m * m + m * 0.05f;
		if (v > best) { best = v; P.percCol = x; }
	}

	P.kickF0 = 90.0f + 200.0f * P.r;
	P.kickF1 = 38.0f + 30.0f * P.lum;
	P.kickPD = 0.02f + 0.06f * (1.0f - P.contrast);
	P.kickAD = 0.15f + 0.5f * P.contrast;
	P.kickClick = P.edge;
	P.snTone = 150.0f + 150.0f * P.g;
	P.snNoise = 0.4f + 0.6f * P.edge;
	P.snDecay = 0.08f + 0.25f * (1.0f - P.lum);
	P.snBP = 800.0f + 3500.0f * P.b;
	P.hatBase = 300.0f + 500.0f * P.sat;
	P.hatDecay = 0.025f + 0.15f * P.lum;
	P.percDecay = 0.05f + 0.3f * P.sat;
	float ic = 1.0f - P.contrast;
	P.synA = 0.002f + 0.4f * ic * ic;
	P.synD = 0.1f + 0.8f * P.lum;
	P.synS = 0.3f + 0.6f * P.sat;
	P.synR = 0.05f + 1.2f * ic;
	P.synCut = 250.0f + 4000.0f * powf(P.lum, 1.5f);
	P.synRes = clampf(0.1f + P.edge * 0.8f, 0, 0.85f);
	P.synEnv = 0.3f + P.sat;
	P.synScan = hue / 360.0f;
	P.synMove = 0.1f + P.edge;
}

static void buildWavetables(void) {
	float tmp[128];
	for (int t = 0; t < NTAB; t++) {
		int c = t * (PW - 1) / (NTAB - 1);
		float mean = 0;
		for (int i = 0; i < 128; i++) {
			int y = i * (PH - 1) / 127, s = 0, k = 0;
			for (int dx = -1; dx <= 1; dx++) { int xx = c + dx; if (xx < 0 || xx >= PW) continue; s += luma[y][xx]; k++; }
			tmp[i] = s / (255.0f * k); mean += tmp[i];
		}
		mean /= 128;
		float* T = wtab + t * TABN;
		for (int i = 0; i < 128; i++) { T[i] = tmp[i] - mean; T[TABN - 1 - i] = tmp[i] - mean; }
		for (int pass = 0; pass < 2; pass++) {
			float prev = T[TABN - 1], first = T[0];
			for (int i = 0; i < TABN; i++) {
				float nx = (i == TABN - 1) ? first : T[i + 1];
				float cur = T[i];
				T[i] = (prev + 2 * cur + nx) * 0.25f;
				prev = cur;
			}
		}
		float pk = 0;
		for (int i = 0; i < TABN; i++) pk = fmaxf(pk, fabsf(T[i]));
		if (pk < 0.002f) for (int i = 0; i < TABN; i++) T[i] = sinf(TWO_PI_F * i / TABN);
		else for (int i = 0; i < TABN; i++) T[i] /= pk;
	}
}

static void buildCaches(void) {
	for (int x = 0; x < PW; x++)
		for (int y = 0; y < PH; y++) {
			u8* d = topCache + (x * 240 + (239 - y)) * 3;
			const u8* s = photo[y][x];
			d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
		}
	for (int i = 0; i < PW * PH * 3; i++) topDim[i] = (topCache[i] * 100) >> 8;
	for (int y = 0; y < 80; y++)
		for (int x = 0; x < 320; x++) {
			int sx = x * PW / 320, sy = y * PH / 80;
			memcpy(stripCache[y][x], photo[sy][sx], 3);
		}
}

// ---------------------------------------------------------------- photo -> tape
static inline float wtOsc(float t, float ph) {
	int t0 = (int)t; if (t0 >= NTAB - 1) t0 = NTAB - 2; float ft = t - t0;
	const float* A = wtab + t0 * TABN; const float* B = A + TABN;
	float p = ph * TABN; int i0 = (int)p; float fr = p - i0; i0 &= TABN - 1; int i1 = (i0 + 1) & (TABN - 1);
	float a = A[i0] + (A[i1] - A[i0]) * fr, b = B[i0] + (B[i1] - B[i0]) * fr;
	return a + (b - a) * ft;
}

static void computeTapePeaks(void) {
	int tl = tapeLen;
	for (int x = 0; x < 320; x++) {
		int a = (int)((long long)x * tl / 320), b = (int)((long long)(x + 1) * tl / 320); int pk = 0;
		for (int i = a; i < b; i += 4) { int v = abs(tape[i]); if (v > pk) pk = v; }
		tapePeak[x] = pk >> 8;
	}
}
static void computeSmpPeaks(void) {
	int n = smpLen;
	for (int x = 0; x < 400; x++) {
		int a = (int)((long long)x * n / 400), b = (int)((long long)(x + 1) * n / 400); int pk = 0;
		for (int i = a; i < b; i += 2) { int v = abs(smp[i]); if (v > pk) pk = v; }
		smpPeak[x] = pk >> 8;
	}
}

static void renderTape(void) {
	audioHold(1);
	int len = 0;
	float* acc = (float*)malloc(TAPE_MAX * sizeof(float));
	if (!acc) { audioHold(0); return; }
	int colLen = TAPE_MAX / PW;
	if (tapeMode == TM_MIC && smpE - smpS < 1000) tapeMode = TM_SPECTRAL;
	if (tapeMode == TM_SPECTRAL) {
		int nb = SCALES[P.scale].n * 4 + 1;
		static float A[PW + 1];
		len = colLen * PW;
		memset(acc, 0, len * sizeof(float));
		for (int b = 0; b < nb; b++) {
			int y0 = PH - (b + 1) * PH / nb, y1 = PH - b * PH / nb; int any = 0;
			for (int c = 0; c < PW; c++) {
				int s = 0; for (int y = y0; y < y1; y++) s += luma[y][c];
				float v = (s / (255.0f * (y1 - y0)) - 0.3f) / 0.7f;
				if (v < 0) v = 0;
				A[c] = v * v; if (A[c] > 0) any = 1;
			}
			A[PW] = A[PW - 1];
			if (!any) continue;
			float inc = mtof(36 + P.root + degSemi(b)) / SR, ph = frandM();
			float w = 1.0f / (1.0f + b * 0.04f);
			for (int c = 0; c < PW; c++) {
				float a0 = A[c] * w, a1 = A[c + 1] * w;
				if (a0 == 0 && a1 == 0) { ph += inc * colLen; ph -= floorf(ph); continue; }
				float da = (a1 - a0) / colLen, a = a0;
				float* o = acc + c * colLen;
				for (int i = 0; i < colLen; i++) { o[i] += fsin(ph) * a; a += da; ph += inc; if (ph >= 1) ph -= 1; }
			}
		}
	} else if (tapeMode == TM_WAVE) {
		len = colLen * PW;
		float f = mtof(48 + P.root) / SR, p1 = 0, p2 = 0.3f, p3 = 0.6f;
		for (int i = 0; i < len; i++) {
			float t = (float)i / len * (NTAB - 1);
			int c = i / colLen; if (c >= PW) c = PW - 1;
			float amp = 0.3f; { int s = 0; for (int y = 0; y < PH; y += 8) s += luma[y][c]; amp += s / (255.0f * PH / 8); }
			acc[i] = (wtOsc(t, p1) * 0.55f + wtOsc(t, p2) * 0.25f + wtOsc(t, p3) * 0.45f) * amp;
			p1 += f; if (p1 >= 1) p1 -= 1;
			p2 += f * 1.5f * 1.002f; if (p2 >= 1) p2 -= 1;
			p3 += f * 0.5f; if (p3 >= 1) p3 -= 1;
		}
	} else if (tapeMode == TM_SCAN) {
		len = PW * PH;
		int k = 0;
		for (int x = 0; x < PW; x++)
			for (int j = 0; j < PH; j++) { int y = (x & 1) ? PH - 1 - j : j; acc[k++] = luma[y][x] / 255.0f - 0.5f; }
		float xp = 0, y = 0, lp = 0;
		for (int i = 0; i < len; i++) { float xx = acc[i]; y = xx - xp + 0.995f * y; xp = xx; lp += 0.5f * (y - lp); acc[i] = lp; }
	} else {   // MIC
		len = smpE - smpS; if (len > TAPE_MAX) len = TAPE_MAX;
		for (int i = 0; i < len; i++) acc[i] = smp[smpS + i] / 32768.0f;
	}
	float pk = 0.0001f;
	for (int i = 0; i < len; i++) pk = fmaxf(pk, fabsf(acc[i]));
	float g = (tapeMode == TM_MIC ? 1.0f : 0.9f) / pk;
	if (tapeMode == TM_MIC && pk < 1.0f) g = 1.0f;
	for (int i = 0; i < len; i++) tape[i] = (s16)clampf(acc[i] * g * 32767.0f, -32767, 32767);
	free(acc);
	tapeLen = len;
	tapePosD = loopS * len;
	for (int i = 0; i < MAXGR; i++) { grains[i].on = 0; grainVisOn[i] = 0; }
	perc.on = 0;
	computeTapePeaks();
	audioHold(0);
}

// ---------------------------------------------------------------- default picture
static void genDefaultPhoto(void) {
	for (int y = 0; y < PH; y++)
		for (int x = 0; x < PW; x++) {
			float r, g, b;
			float sy = y / 140.0f;
			if (y < 140) {
				r = 250 - 90 * sy; g = 150 - 100 * sy; b = 80 + 90 * sy;
				float dx = x - 270, dy = y - 105, d = sqrtf(dx * dx + dy * dy);
				if (d < 38) { r = 255; g = 220 - d; b = 120; }
				else if (d < 70) { float k = (70 - d) / 32.0f * 0.5f; r += (255 - r) * k; g += (200 - g) * k; }
				if (y > 70 && fabsf(sinf(x * 0.05f) * 20 + 120 - y) < 3 && x < 170) { r *= 0.3f; g *= 0.3f; b *= 0.4f; }
			} else {
				float k = (y - 140) / 100.0f;
				r = 60 - 40 * k; g = 50 - 20 * k; b = 110 - 40 * k;
				float ref = sinf(x * 0.09f + y * 0.7f) * 0.5f + 0.5f;
				if (abs(x - 270) < 50 - k * 30 && ref > 0.6f) { r = 250; g = 180; b = 110; }
			}
			if (x > 40 && x < 52 && y > 60 && y < 140) { r = g = b = 20; }
			if (y > 50 && y < 75 && fabsf((x - 46) * 0.6f) < (75 - y) * 1.2f && fabsf(sinf(x * 0.3f)) > 0.3f) { r = 25; g = 30; b = 25; }
			photo[y][x][0] = (u8)clampf(r, 0, 255); photo[y][x][1] = (u8)clampf(g, 0, 255); photo[y][x][2] = (u8)clampf(b, 0, 255);
		}
}

// ---------------------------------------------------------------- files
static void ensureDir(void) { mkdir("sdmc:/3ds", 0777); mkdir(PT_DIR, 0777); }

static void put32(u8* p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(u8* p, u16 v) { p[0] = v; p[1] = v >> 8; }
static u32 get32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

static int saveBMP(const char* path) {
	FILE* f = fopen(path, "wb"); if (!f) return 0;
	u8 h[54]; memset(h, 0, 54);
	int rowB = PW * 3, sz = rowB * PH;
	h[0] = 'B'; h[1] = 'M'; put32(h + 2, 54 + sz); put32(h + 10, 54); put32(h + 14, 40);
	put32(h + 18, PW); put32(h + 22, PH); put16(h + 26, 1); put16(h + 28, 24); put32(h + 34, sz);
	fwrite(h, 1, 54, f);
	u8 row[PW * 3];
	for (int y = PH - 1; y >= 0; y--) {
		for (int x = 0; x < PW; x++) { row[x * 3] = photo[y][x][2]; row[x * 3 + 1] = photo[y][x][1]; row[x * 3 + 2] = photo[y][x][0]; }
		fwrite(row, 1, rowB, f);
	}
	fclose(f); return 1;
}

static int loadBMP(const char* path) {
	FILE* f = fopen(path, "rb"); if (!f) return 0;
	u8 h[54];
	if (fread(h, 1, 54, f) != 54 || h[0] != 'B' || h[1] != 'M') { fclose(f); return 0; }
	u32 off = get32(h + 10); int w = (int)get32(h + 18); int hh = (int)get32(h + 22); int bpp = h[28] | (h[29] << 8);
	if (bpp != 24 || w <= 0 || w > 4096 || hh == 0 || hh > 4096 || hh < -4096) { fclose(f); return 0; }
	int top = hh < 0; if (top) hh = -hh;
	int rowB = (w * 3 + 3) & ~3;
	u8* img = (u8*)malloc(rowB * hh);
	if (!img) { fclose(f); return 0; }
	fseek(f, off, SEEK_SET);
	int ok = fread(img, 1, rowB * hh, f) == (size_t)(rowB * hh);
	fclose(f);
	if (ok)
		for (int y = 0; y < PH; y++)
			for (int x = 0; x < PW; x++) {
				int sx = x * w / PW, sy = y * hh / PH;
				int ry = top ? sy : hh - 1 - sy;
				const u8* s = img + ry * rowB + sx * 3;
				photo[y][x][0] = s[2]; photo[y][x][1] = s[1]; photo[y][x][2] = s[0];
			}
	free(img);
	return ok;
}

static int saveWAV(const char* path, const s16* d, int n) {
	FILE* f = fopen(path, "wb"); if (!f) return 0;
	u8 h[44]; memcpy(h, "RIFF", 4); put32(h + 4, 36 + n * 2); memcpy(h + 8, "WAVEfmt ", 8);
	put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 1); put32(h + 24, SR); put32(h + 28, SR * 2);
	put16(h + 32, 2); put16(h + 34, 16); memcpy(h + 36, "data", 4); put32(h + 40, n * 2);
	fwrite(h, 1, 44, f); fwrite(d, 2, n, f); fclose(f); return 1;
}

#define PAT_MAGIC 0x50544150
typedef struct {
	u32 magic, ver;
	int root, scale, tapeMode, tapeDir, tapeSync, grainSrc, tempo;
	float tp[6], gp[6], loopS, loopE, swing;
	Step seq[NTR][NSTEP];
	float trTune[NTR], trDecay[NTR];
	u8 trMute[NTR];
	int smpLen, smpS, smpE;
} PatFile;

static int savePatch(int id) {
	char path[64]; snprintf(path, sizeof(path), PT_DIR "/P%04d.pat", id);
	FILE* f = fopen(path, "wb"); if (!f) return 0;
	PatFile pf; memset(&pf, 0, sizeof(pf));
	pf.magic = PAT_MAGIC; pf.ver = 1; pf.root = P.root; pf.scale = P.scale; pf.tapeMode = tapeMode;
	pf.tapeDir = tapeDir; pf.tapeSync = tapeSync; pf.grainSrc = grainSrc; pf.tempo = tempo;
	memcpy(pf.tp, tp, sizeof(tp)); memcpy(pf.gp, gp, sizeof(gp));
	pf.loopS = loopS; pf.loopE = loopE; pf.swing = swing;
	memcpy(pf.seq, seq, sizeof(seq)); memcpy(pf.trTune, trTune, sizeof(trTune));
	memcpy(pf.trDecay, trDecay, sizeof(trDecay)); memcpy(pf.trMute, trMute, sizeof(trMute));
	pf.smpLen = smpLen; pf.smpS = smpS; pf.smpE = smpE;
	fwrite(&pf, sizeof(pf), 1, f);
	if (smpLen > 0) fwrite(smp, 2, smpLen, f);
	fclose(f); return 1;
}

static int loadPatch(int id) {
	char path[64]; snprintf(path, sizeof(path), PT_DIR "/P%04d.pat", id);
	FILE* f = fopen(path, "rb"); if (!f) return 0;
	PatFile pf;
	if (fread(&pf, sizeof(pf), 1, f) != 1 || pf.magic != PAT_MAGIC) { fclose(f); return 0; }
	audioHold(1);
	P.root = ((pf.root % 12) + 12) % 12; P.scale = pf.scale & 7;
	tapeMode = pf.tapeMode % TM_COUNT; tapeDir = pf.tapeDir < 0 ? -1 : 1; tapeSync = pf.tapeSync & 3;
	grainSrc = pf.grainSrc & 1; tempo = pf.tempo < 40 || pf.tempo > 240 ? 96 : pf.tempo;
	memcpy(tp, pf.tp, sizeof(tp)); memcpy(gp, pf.gp, sizeof(gp));
	loopS = clampf(pf.loopS, 0, 1); loopE = clampf(pf.loopE, 0, 1); swing = clampf(pf.swing, 0, 0.6f);
	memcpy(seq, pf.seq, sizeof(seq)); memcpy(trTune, pf.trTune, sizeof(trTune));
	memcpy(trDecay, pf.trDecay, sizeof(trDecay)); memcpy(trMute, pf.trMute, sizeof(trMute));
	if (pf.smpLen > 0 && pf.smpLen <= SMP_MAX && fread(smp, 2, pf.smpLen, f) == (size_t)pf.smpLen) {
		smpLen = pf.smpLen;
		smpS = pf.smpS >= 0 && pf.smpS < smpLen ? pf.smpS : 0;
		smpE = pf.smpE > smpS && pf.smpE <= smpLen ? pf.smpE : smpLen;
	}
	for (int i = 0; i < NSMPV; i++) smpv[i].on = 0;
	audioHold(0);
	fclose(f);
	computeSmpPeaks();
	return 1;
}

// library of saved photos
static int lib[1000], libN = 0, libIdx = -1;
static int cmpInt(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }
static void scanLib(void) {
	libN = 0;
	DIR* d = opendir(PT_DIR); if (!d) return;
	struct dirent* e;
	while ((e = readdir(d)) && libN < 1000) {
		int id;
		if ((e->d_name[0] == 'P') && sscanf(e->d_name, "P%d", &id) == 1 && (strstr(e->d_name, ".bmp") || strstr(e->d_name, ".BMP")))
			lib[libN++] = id;
	}
	closedir(d);
	qsort(lib, libN, sizeof(int), cmpInt);
}
static int nextPhotoId(void) { return libN ? lib[libN - 1] + 1 : 1; }

static void onNewPhoto(void) {
	analyzePhoto();
	buildCaches();
	audioHold(1); buildWavetables(); audioHold(0);
	if (tapeMode == TM_MIC) tapeMode = TM_SPECTRAL;
	if (photoId > 0) loadPatch(photoId);
	renderTape();
}

// ---------------------------------------------------------------- patterns
static void defaultPattern(void) {
	memset(seq, 0, sizeof(seq));
	for (int s = 0; s < NSTEP; s += 4) seq[T_KICK][s] = (Step){1, 12};
	seq[T_KICK][10] = (Step){1, 7};
	seq[T_SNARE][4] = (Step){1, 11}; seq[T_SNARE][12] = (Step){1, 11};
	for (int s = 2; s < NSTEP; s += 4) seq[T_HAT][s] = (Step){1, 9};
	seq[T_HAT][7] = (Step){1, 4}; seq[T_HAT][15] = (Step){1, 5};
	seq[T_PERC][3] = (Step){1, 9}; seq[T_PERC][11] = (Step){1, 7};
	seq[T_SYNTH][0] = (Step){1, 0}; seq[T_SYNTH][3] = (Step){1, 2}; seq[T_SYNTH][6] = (Step){1, 4}; seq[T_SYNTH][10] = (Step){1, 3};
	seq[T_GRAIN][8] = (Step){1, 7};
	for (int t = 0; t < NTR; t++) { trTune[t] = 0.5f; trDecay[t] = 0.5f; trMute[t] = 0; }
	for (int s = 0; s < NSTEP; s++) { if (!seq[T_SAMPLE][s].on) seq[T_SAMPLE][s].val = 0; }
	for (int t = T_SYNTH; t <= T_SAMPLE; t++) for (int s = 0; s < NSTEP; s++) if (!seq[t][s].on) seq[t][s].val = 0;
	for (int t = 0; t < T_SYNTH; t++) for (int s = 0; s < NSTEP; s++) if (!seq[t][s].on) seq[t][s].val = 10;
}

static void genFromPhoto(int tr) {
	static const int K[NTR] = {4, 2, 7, 3, 5, 3, 3};
	float m[NSTEP]; int brightRow[NSTEP];
	int y0 = tr * PH / NTR, y1 = (tr + 1) * PH / NTR;
	for (int s = 0; s < NSTEP; s++) {
		int sum = 0, cnt = 0, bv = -1; brightRow[s] = PH / 2;
		for (int x = s * 25; x < s * 25 + 25; x += 2) {
			for (int y = y0; y < y1; y++) { sum += luma[y][x]; cnt++; }
			for (int y = 0; y < PH; y += 3) if (luma[y][x] > bv) { bv = luma[y][x]; brightRow[s] = y; }
		}
		m[s] = sum / (255.0f * cnt) + (frandM() * 0.02f);
	}
	audioHold(1);
	for (int s = 0; s < NSTEP; s++) seq[tr][s].on = 0;
	for (int k = 0; k < K[tr]; k++) {
		int bi = -1; float bv = -1;
		for (int s = 0; s < NSTEP; s++) if (!seq[tr][s].on && m[s] > bv) { bv = m[s]; bi = s; }
		if (tr == T_KICK && k == 0) bi = 0;
		if (bi < 0) break;
		seq[tr][bi].on = 1;
	}
	for (int s = 0; s < NSTEP; s++) {
		if (tr >= T_SYNTH) seq[tr][s].val = (s8)((int)((1.0f - (float)brightRow[s] / PH) * 11.0f) - 2);
		else seq[tr][s].val = (s8)clampf(5 + m[s] * 10.0f, 1, 15);
	}
	audioHold(0);
}

// ---------------------------------------------------------------- camera
static u16* camBuf; static u16* camFrame;
static Handle camEvt = 0;
static int camOn = 0, camReady = 0, camInner = 0;
static u32 camXfer = 0;
static u32 camSelect(void) { return camInner ? SELECT_IN1 : SELECT_OUT1; }
static void camStart(void) {
	if (camOn) return;
	if (R_FAILED(camInit())) return;
	u32 sel = camSelect();
	CAMU_SetSize(sel, SIZE_CTR_TOP_LCD, CONTEXT_A);
	CAMU_SetOutputFormat(sel, OUTPUT_RGB_565, CONTEXT_A);
	CAMU_SetFrameRate(sel, FRAME_RATE_30);
	CAMU_SetNoiseFilter(sel, true);
	CAMU_SetAutoExposure(sel, true);
	CAMU_SetAutoWhiteBalance(sel, true);
	CAMU_SetTrimming(PORT_CAM1, false);
	CAMU_GetMaxBytes(&camXfer, PW, PH);
	CAMU_SetTransferBytes(PORT_CAM1, camXfer, PW, PH);
	CAMU_Activate(sel);
	CAMU_ClearBuffer(PORT_CAM1);
	CAMU_StartCapture(PORT_CAM1);
	camEvt = 0;
	CAMU_SetReceiving(&camEvt, camBuf, PORT_CAM1, PW * PH * 2, (s16)camXfer);
	camOn = 1; camReady = 0;
}
static void camStop(void) {
	if (!camOn) return;
	CAMU_StopCapture(PORT_CAM1);
	if (camEvt) { svcCloseHandle(camEvt); camEvt = 0; }
	CAMU_Activate(SELECT_NONE);
	camExit();
	camOn = 0; camReady = 0;
}
static void camPoll(void) {
	if (!camOn || !camEvt) return;
	if (svcWaitSynchronization(camEvt, 0) == 0) {
		svcCloseHandle(camEvt); camEvt = 0;
		memcpy(camFrame, camBuf, PW * PH * 2);
		camReady = 1;
		CAMU_SetReceiving(&camEvt, camBuf, PORT_CAM1, PW * PH * 2, (s16)camXfer);
	}
}
static inline void rgb565(u16 p, u8* r, u8* g, u8* b) {
	*r = ((p >> 11) & 0x1F) << 3; *g = ((p >> 5) & 0x3F) << 2; *b = (p & 0x1F) << 3;
}

// ---------------------------------------------------------------- mic
static u8* micBuf; static u32 micDataSize = 0, micLastOff = 0;
static int micOK = 0, recording = 0, micGainIdx = 1;
static float micLevel = 0;
static const u8 MIC_GAINS[4] = {0x20, 0x40, 0x60, 0x7F};

static void recStart(void) {
	if (!micOK || recording) return;
	MICU_SetGain(MIC_GAINS[micGainIdx]);
	if (R_FAILED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_32730, 0, micDataSize, true))) return;
	micLastOff = micGetLastSampleOffset();
	rawLen = 0; recording = 1;
}
static void recPoll(void) {
	if (!recording) return;
	u32 off = micGetLastSampleOffset();
	float pk = 0;
	while (micLastOff != off && rawLen < RAW_MAX) {
		s16 s = *(s16*)(micBuf + micLastOff);
		rawMic[rawLen++] = s;
		float a = fabsf(s / 32768.0f); if (a > pk) pk = a;
		micLastOff += 2; if (micLastOff >= micDataSize) micLastOff = 0;
	}
	micLevel = fmaxf(pk, micLevel * 0.85f);
}
static void pushUndo(void) {
	memcpy(smpUndo, smp, smpLen * 2); smpUndoLen = smpLen; smpUndoS = smpS; smpUndoE = smpE; haveUndo = 1;
}
static int recStop(void) {
	if (!recording) return 0;
	MICU_StopSampling();
	recording = 0; micLevel = 0;
	int skip = 1000;   // drop the start-up click
	if (rawLen < skip + 1000) return 0;
	audioHold(1);
	pushUndo();
	float ratio = MIC_RATE / SR;
	int n = (int)((rawLen - skip - 1) / ratio); if (n > SMP_MAX) n = SMP_MAX;
	for (int i = 0; i < n; i++) {
		float p = skip + i * ratio; int p0 = (int)p; float fr = p - p0;
		smp[i] = (s16)(rawMic[p0] + (rawMic[p0 + 1] - rawMic[p0]) * fr);
	}
	smpLen = n; smpS = 0; smpE = n;
	for (int i = 0; i < NSMPV; i++) smpv[i].on = 0;
	audioHold(0);
	computeSmpPeaks();
	return 1;
}

// sample editor ops
static void edTrim(void) {
	if (smpE - smpS < 64) return;
	audioHold(1); pushUndo();
	int n = smpE - smpS; memmove(smp, smp + smpS, n * 2);
	smpLen = n; smpS = 0; smpE = n;
	for (int i = 0; i < NSMPV; i++) smpv[i].on = 0;
	audioHold(0); computeSmpPeaks();
}
static void edNorm(void) {
	if (smpE - smpS < 2) return;
	audioHold(1); pushUndo();
	int pk = 1; for (int i = smpS; i < smpE; i++) { int v = abs(smp[i]); if (v > pk) pk = v; }
	float g = 31000.0f / pk;
	for (int i = smpS; i < smpE; i++) smp[i] = (s16)clampf(smp[i] * g, -32767, 32767);
	audioHold(0); computeSmpPeaks();
}
static void edRev(void) {
	if (smpE - smpS < 2) return;
	audioHold(1); pushUndo();
	for (int a = smpS, b = smpE - 1; a < b; a++, b--) { s16 t = smp[a]; smp[a] = smp[b]; smp[b] = t; }
	audioHold(0); computeSmpPeaks();
}
static void edFade(void) {
	int n = smpE - smpS; if (n < 64) return;
	audioHold(1); pushUndo();
	int fi = SR / 200; if (fi > n / 4) fi = n / 4;
	int fo = n * 3 / 10;
	for (int i = 0; i < fi; i++) smp[smpS + i] = (s16)(smp[smpS + i] * (float)i / fi);
	for (int i = 0; i < fo; i++) { float k = (float)i / fo; smp[smpE - 1 - i] = (s16)(smp[smpE - 1 - i] * k * k); }
	audioHold(0); computeSmpPeaks();
}
static void edUndo(void) {
	if (!haveUndo) return;
	audioHold(1);
	int n = smpLen > smpUndoLen ? smpLen : smpUndoLen;
	for (int i = 0; i < n; i++) { s16 t = smp[i]; smp[i] = smpUndo[i]; smpUndo[i] = t; }
	int l = smpLen, s = smpS, e = smpE;
	smpLen = smpUndoLen; smpS = smpUndoS; smpE = smpUndoE;
	smpUndoLen = l; smpUndoS = s; smpUndoE = e;
	for (int i = 0; i < NSMPV; i++) smpv[i].on = 0;
	audioHold(0); computeSmpPeaks();
}

// ---------------------------------------------------------------- drawing
static u8* fb; static int fbW = 320; static u8* fontGfx;

static inline void px(int x, int y, u8 r, u8 g, u8 b) {
	if (x < 0 || x >= fbW || y < 0 || y >= 240) return;
	u8* p = fb + (x * 240 + (239 - y)) * 3; p[0] = b; p[1] = g; p[2] = r;
}
static void rect(int x0, int y0, int w, int h, u8 r, u8 g, u8 b) {
	int x1 = x0 + w, y1 = y0 + h;
	if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > fbW) x1 = fbW; if (y1 > 240) y1 = 240;
	if (x0 >= x1 || y0 >= y1) return;
	for (int x = x0; x < x1; x++) {
		u8* p = fb + (x * 240 + (240 - y1)) * 3;
		for (int y = y0; y < y1; y++) { p[0] = b; p[1] = g; p[2] = r; p += 3; }
	}
}
static void blend(int x0, int y0, int w, int h, u8 r, u8 g, u8 b, int a) {
	int x1 = x0 + w, y1 = y0 + h;
	if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > fbW) x1 = fbW; if (y1 > 240) y1 = 240;
	if (x0 >= x1 || y0 >= y1) return;
	int ia = 256 - a;
	for (int x = x0; x < x1; x++) {
		u8* p = fb + (x * 240 + (240 - y1)) * 3;
		for (int y = y0; y < y1; y++) {
			p[0] = (p[0] * ia + b * a) >> 8; p[1] = (p[1] * ia + g * a) >> 8; p[2] = (p[2] * ia + r * a) >> 8; p += 3;
		}
	}
}
static void frame(int x, int y, int w, int h, u8 r, u8 g, u8 b) {
	rect(x, y, w, 1, r, g, b); rect(x, y + h - 1, w, 1, r, g, b);
	rect(x, y, 1, h, r, g, b); rect(x + w - 1, y, 1, h, r, g, b);
}
static void drawChar(int x, int y, char ch, int sc, u8 r, u8 g, u8 b) {
	const u8* gl = fontGfx + ((u8)ch) * 8;
	for (int row = 0; row < 8; row++)
		for (int col = 0; col < 8; col++)
			if (gl[row] & (0x80 >> col)) rect(x + col * sc, y + row * sc, sc, sc, r, g, b);
}
static void text(int x, int y, const char* s, int sc, u8 r, u8 g, u8 b) {
	for (; *s; s++, x += 8 * sc) drawChar(x, y, *s, sc, r, g, b);
}
static void textC(int cx, int y, const char* s, int sc, u8 r, u8 g, u8 b) {
	text(cx - (int)strlen(s) * 4 * sc, y, s, sc, r, g, b);
}
static void button(int x, int y, int w, int h, const char* label, int on) {
	rect(x + 1, y + 1, w - 2, h - 2, on ? 230 : 46, on ? 120 : 42, on ? 60 : 52);
	frame(x + 1, y + 1, w - 2, h - 2, 120, 110, 130);
	textC(x + w / 2, y + h / 2 - 4, label, 1, 255, 255, 255);
}
static int inBox(int tx, int ty, int x, int y, int w, int h) { return tx >= x && tx < x + w && ty >= y && ty < y + h; }

// ---------------------------------------------------------------- UI state
enum { PG_CAM, PG_TAPE, PG_GRAIN, PG_SEQ, PG_MIC, PG_KEYS, PG_COUNT };
static const char* PG_NAMES[PG_COUNT] = {"CAM","TAPE","GRAIN","SEQ","MIC","KEYS"};
static int page = PG_CAM;
static char msg[48] = ""; static int msgT = 0;
static void say(const char* s) { snprintf(msg, sizeof(msg), "%s", s); msgT = 120; }
static int selTrack = 0, keysMode = 0, keysOct = 0, snapFlash = 0;
static const char* KEYS_MODES[3] = {"SYNTH", "GRAIN", "SAMPLE"};

#define STRIP_Y 22
#define STRIP_H 80
#define SL_Y    106
#define SL_H    22

static void drawTabs(void) {
	for (int i = 0; i < PG_COUNT; i++) {
		int x = i * 53, w = (i == PG_COUNT - 1) ? 320 - x : 53;
		rect(x, 0, w, 19, i == page ? 230 : 34, i == page ? 120 : 30, i == page ? 60 : 40);
		rect(x + w - 1, 0, 1, 19, 10, 10, 12);
		textC(x + w / 2, 6, PG_NAMES[i], 1, 255, 255, 255);
	}
}

static void drawStrip(int showLoop) {
	for (int y = 0; y < STRIP_H; y++)
		for (int x = 0; x < 320; x++) {
			const u8* c = stripCache[y][x];
			px(x, STRIP_Y + y, c[0] >> 1, c[1] >> 1, c[2] >> 1);
		}
	int mid = STRIP_Y + STRIP_H / 2;
	for (int x = 0; x < 320; x++) {
		int h = tapePeak[x] * (STRIP_H / 2 - 2) / 128;
		rect(x, mid - h, 1, h * 2 + 1, 240, 235, 220);
	}
	if (showLoop) {
		int a = (int)(loopS * 320), b = (int)(loopE * 320);
		blend(0, STRIP_Y, a, STRIP_H, 0, 0, 0, 170);
		blend(b, STRIP_Y, 320 - b, STRIP_H, 0, 0, 0, 170);
		rect(a, STRIP_Y, 2, STRIP_H, 255, 180, 60); rect(b - 2, STRIP_Y, 2, STRIP_H, 255, 180, 60);
		int ph = (int)(tapePosFrac * 320);
		if (tapeRun || tapeMul > 0) rect(ph, STRIP_Y, 2, STRIP_H, 120, 255, 200);
	} else {
		int gx = (int)(grainPos * 320);
		rect(gx - 1, STRIP_Y, 3, STRIP_H, 200, 150, 255);
		if (grainSrc == 1) text(4, STRIP_Y + 3, "SRC: MIC SAMPLE (X)", 1, 255, 200, 120);
		for (int i = 0; i < MAXGR; i++) if (grainVisOn[i]) rect((int)(grainVisPos[i] * 320), STRIP_Y + 4 + (i * 7) % (STRIP_H - 8), 3, 3, 255, 255, 255);
	}
	frame(0, STRIP_Y, 320, STRIP_H, 90, 85, 100);
}

static void sliderValText(int pg, int i, char* o, int n) {
	if (pg == PG_TAPE) {
		switch (i) {
		case 0:
			if (tapeSync) snprintf(o, n, "%dBAR", SYNC_BARS[tapeSync]);
			else snprintf(o, n, "%s%.2fx", tapeDir < 0 ? "-" : "", powf(2.0f, (tp[0] - 0.5f) * 4.0f));
			break;
		case 4: snprintf(o, n, "%dk", (int)(400.0f * powf(2.0f, tp[4] * 5.5f) / 1000.0f)); break;
		default: snprintf(o, n, "%d%%", (int)(tp[i] * 100)); break;
		}
	} else {
		switch (i) {
		case 0: snprintf(o, n, "%dms", (int)(10.0f * powf(2.0f, gp[0] * 6.0f))); break;
		case 1: snprintf(o, n, "%d/s", (int)(2.0f * powf(2.0f, gp[1] * 5.3f))); break;
		case 3: snprintf(o, n, "%+dst", (int)roundf((gp[3] - 0.5f) * 48.0f)); break;
		default: snprintf(o, n, "%d%%", (int)(gp[i] * 100)); break;
		}
	}
}

static void drawSliders(int pg) {
	float* v = pg == PG_TAPE ? tp : gp;
	const char** names = pg == PG_TAPE ? TP_NAMES : GP_NAMES;
	for (int i = 0; i < 6; i++) {
		int y = SL_Y + i * SL_H;
		rect(0, y, 320, SL_H - 1, 26, 24, 30);
		text(4, y + 7, names[i], 1, 220, 215, 230);
		rect(50, y + 6, 210, 10, 50, 46, 58);
		rect(50, y + 6, (int)(v[i] * 210), 10, pg == PG_TAPE ? 230 : 160, pg == PG_TAPE ? 140 : 110, pg == PG_TAPE ? 60 : 240);
		char s[16]; sliderValText(pg, i, s, sizeof(s));
		text(266, y + 7, s, 1, 255, 255, 255);
	}
}

static void drawBottomCam(void) {
	char b[64];
	rect(0, 20, 320, 220, 18, 16, 22);
	if (photoId) snprintf(b, sizeof(b), "PATCH P%04d", photoId); else snprintf(b, sizeof(b), "PATCH (built-in)");
	text(8, 26, b, 2, 255, 220, 160);
	snprintf(b, sizeof(b), "KEY  %s %s", NOTE_NAMES[P.root], SCALES[P.scale].name); text(8, 48, b, 1, 230, 230, 240);
	snprintf(b, sizeof(b), "TAPE %s  %.1fs", TM_NAMES[tapeMode], tapeLen / (float)SR); text(8, 60, b, 1, 230, 230, 240);
	snprintf(b, sizeof(b), "KICK %d>%dHz  SNR %dHz  HAT %dHz", (int)P.kickF0, (int)P.kickF1, (int)P.snTone, (int)P.hatBase); text(8, 72, b, 1, 200, 200, 210);
	snprintf(b, sizeof(b), "SYN A%.2f D%.2f S%.2f R%.2f", P.synA, P.synD, P.synS, P.synR); text(8, 84, b, 1, 200, 200, 210);
	snprintf(b, sizeof(b), "    CUT %dHz RES %d%% MOVE %d%%", (int)P.synCut, (int)(P.synRes * 100), (int)(P.synMove * 100)); text(8, 96, b, 1, 200, 200, 210);
	snprintf(b, sizeof(b), "LUM %d%% SAT %d%% CON %d%% EDGE %d%%", (int)(P.lum * 100), (int)(P.sat * 100), (int)(P.contrast * 100), (int)(P.edge * 100)); text(8, 108, b, 1, 160, 160, 175);
	snprintf(b, sizeof(b), "LIBRARY %d/%d", libIdx + 2, libN + 1); text(8, 124, b, 1, 160, 160, 175);
	for (int i = 0; i < 6; i++) { int sw = 320 / 6; rect(i * sw + 2, 136, sw - 4, 8, photo[120][i * 66 + 30][0], photo[120][i * 66 + 30][1], photo[120][i * 66 + 30][2]); }
	button(0, 150, 80, 46, "<<", 0);
	button(80, 150, 160, 46, camOn ? "SNAP (A)" : "CAMERA...", 1);
	button(240, 150, 80, 46, ">>", 0);
	snprintf(b, sizeof(b), "%s (Y)", TM_NAMES[tapeMode]);
	button(0, 200, 106, 38, b, 0);
	button(106, 200, 107, 38, "SAVE (B)", 0);
	button(213, 200, 107, 38, camInner ? "INNER (X)" : "OUTER (X)", 0);
}

static void drawBottomSeq(void) {
	rect(0, 20, 320, 220, 18, 16, 22);
	for (int t = 0; t < NTR; t++) {
		int y = 22 + t * 16;
		const u8* c = TR_COL[t];
		if (t == selTrack) rect(0, y, 31, 15, c[0], c[1], c[2]);
		text(4, y + 4, TR_SHORT[t], 1, t == selTrack ? 0 : (trMute[t] ? 90 : c[0]), t == selTrack ? 0 : (trMute[t] ? 90 : c[1]), t == selTrack ? 0 : (trMute[t] ? 90 : c[2]));
		for (int s = 0; s < NSTEP; s++) {
			int x = 32 + s * 18;
			int beat = (s % 4 == 0);
			u8 br = seq[t][s].on ? 255 : (beat ? 56 : 40);
			if (seq[t][s].on) {
				float k = trMute[t] ? 0.3f : (0.45f + 0.55f * (t < T_SYNTH ? seq[t][s].val / 15.0f : 1.0f));
				rect(x + 1, y + 1, 16, 13, c[0] * k, c[1] * k, c[2] * k);
			} else rect(x + 1, y + 1, 16, 13, br, br, br + 6);
			if (playing && s == curStep) frame(x, y, 18, 15, 255, 255, 255);
		}
	}
	// lane
	int ly = 138, lh = 80;
	rect(32, ly, 288, lh, 30, 28, 36);
	const u8* c = TR_COL[selTrack];
	for (int s = 0; s < NSTEP; s++) {
		int x = 32 + s * 18, v = seq[selTrack][s].val;
		int h;
		if (selTrack >= T_SYNTH) h = (v + 7) * (lh - 4) / 21; else h = v * (lh - 4) / 15;
		if (h < 2) h = 2;
		float k = seq[selTrack][s].on ? 1.0f : 0.35f;
		rect(x + 2, ly + lh - h, 14, h, c[0] * k, c[1] * k, c[2] * k);
	}
	if (selTrack >= T_SYNTH) { int zy = ly + lh - 7 * (lh - 4) / 21; rect(32, zy, 288, 1, 120, 120, 130); }
	text(2, ly + 4, selTrack >= T_SYNTH ? "NOTE" : "VEL", 1, 200, 200, 210);
	char b[64];
	snprintf(b, sizeof(b), "T%d", (int)(trTune[selTrack] * 100)); text(2, ly + 30, b, 1, 160, 160, 170);
	snprintf(b, sizeof(b), "D%d", (int)(trDecay[selTrack] * 100)); text(2, ly + 42, b, 1, 160, 160, 170);
	snprintf(b, sizeof(b), "%s BPM %d SWING %d%%  %s", playing ? ">" : "#", tempo, (int)(swing * 100), trMute[selTrack] ? "MUTED" : "");
	text(4, 226, b, 1, 255, 230, 200);
}

static void drawBottomMic(void) {
	rect(0, 20, 320, 220, 18, 16, 22);
	int wy = 22, wh = 100, mid = wy + wh / 2;
	rect(0, wy, 320, wh, 28, 26, 34);
	if (recording) {
		int lv = (int)(micLevel * 320);
		rect(0, wy, lv, wh, 160, 40, 40);
		textC(160, mid - 8, "RECORDING", 2, 255, 255, 255);
		char b[32]; snprintf(b, sizeof(b), "%.1fs", rawLen / MIC_RATE); textC(160, mid + 12, b, 1, 255, 220, 220);
	} else if (smpLen > 0) {
		for (int x = 0; x < 320; x++) {
			int p = smpPeak[x * 400 / 320];
			int h = p * (wh / 2 - 2) / 128;
			rect(x, mid - h, 1, h * 2 + 1, 240, 120, 190);
		}
		int a = (int)((long long)smpS * 320 / smpLen), b = (int)((long long)smpE * 320 / smpLen);
		blend(0, wy, a, wh, 0, 0, 0, 170); blend(b, wy, 320 - b, wh, 0, 0, 0, 170);
		rect(a, wy, 2, wh, 255, 220, 80); rect(b - 2, wy, 2, wh, 255, 220, 80);
		char s[48]; snprintf(s, sizeof(s), "%.2fs / %.2fs", (smpE - smpS) / (float)SR, smpLen / (float)SR); text(4, wy + 3, s, 1, 255, 255, 255);
	} else textC(160, mid - 4, micOK ? "PRESS A OR REC TO RECORD" : "MIC UNAVAILABLE", 1, 200, 200, 210);
	frame(0, wy, 320, wh, 90, 85, 100);
	button(0, 128, 80, 34, recording ? "STOP" : "REC (A)", recording);
	button(80, 128, 80, 34, "PLAY (B)", 0);
	button(160, 128, 80, 34, "TRIM", 0);
	button(240, 128, 80, 34, "NORM", 0);
	button(0, 164, 80, 34, "REVERSE", 0);
	button(80, 164, 80, 34, "FADE", 0);
	button(160, 164, 80, 34, "UNDO", 0);
	button(240, 164, 80, 34, ">TAPE", tapeMode == TM_MIC);
	char g[24]; snprintf(g, sizeof(g), "GAIN %d", micGainIdx + 1);
	button(0, 200, 106, 38, "SAVE WAV", 0);
	button(106, 200, 107, 38, grainSrc ? "GRAIN:MIC" : "GRAIN:TAPE", grainSrc);
	button(213, 200, 107, 38, g, 0);
}

static int keysHeldPad = -1;
static void drawBottomKeys(void) {
	rect(0, 20, 320, 220, 18, 16, 22);
	int n = SCALES[P.scale].n;
	for (int i = 0; i < 16; i++) {
		int col = i % 8, row = i / 8;
		int x = col * 40, y = 22 + (1 - row) * 50;
		int deg = i + keysOct * n;
		int semi = degSemi(deg);
		int pc = ((P.root + semi) % 12 + 12) % 12;
		int isRoot = (((deg % n) + n) % n) == 0;
		const u8* c = TR_COL[keysMode == 0 ? T_SYNTH : (keysMode == 1 ? T_GRAIN : T_SAMPLE)];
		float k = i == keysHeldPad ? 1.0f : (isRoot ? 0.6f : 0.35f);
		rect(x + 2, y + 2, 36, 46, c[0] * k, c[1] * k, c[2] * k);
		textC(x + 20, y + 20, NOTE_NAMES[pc], 1, 255, 255, 255);
	}
	for (int d = 0; d < 4; d++) {
		const u8* c = TR_COL[d];
		rect(d * 80 + 2, 126, 76, 54, c[0] * 0.5f, c[1] * 0.5f, c[2] * 0.5f);
		textC(d * 80 + 40, 149, TR_LONG[d], 1, 255, 255, 255);
	}
	for (int m = 0; m < 3; m++) button(m * 106, 184, m == 2 ? 108 : 106, 32, KEYS_MODES[m], keysMode == m);
	char b[64]; snprintf(b, sizeof(b), "OCT %+d (UP/DN)  HOLD R = REC TO SEQ", keysOct);
	text(4, 224, b, 1, 220, 215, 230);
}

static void drawTop(void) {
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	char b[80];
	if (page == PG_CAM && camOn && camReady) {
		for (int x = 0; x < PW; x++) {
			u8* p = fb + x * 240 * 3;
			int sx = camInner ? PW - 1 - x : x;
			for (int y = PH - 1; y >= 0; y--) {
				u8 r, g, bb; rgb565(camFrame[y * PW + sx], &r, &g, &bb);
				p[0] = bb; p[1] = g; p[2] = r; p += 3;
			}
		}
	} else memcpy(fb, (page == PG_GRAIN || page == PG_SEQ || page == PG_MIC || page == PG_KEYS) ? topDim : topCache, PW * PH * 3);

	if (snapFlash > 0) { blend(0, 0, 400, 240, 255, 255, 255, snapFlash * 20); snapFlash--; }

	switch (page) {
	case PG_CAM:
		if (!camOn) textC(200, 110, "CAMERA OFF", 2, 255, 255, 255);
		break;
	case PG_TAPE: {
		int a = (int)(loopS * 400), e = (int)(loopE * 400);
		blend(0, 0, a, 240, 0, 0, 0, 150); blend(e, 0, 400 - e, 240, 0, 0, 0, 150);
		if (tapeRun || tapeMul > 0) rect((int)(tapePosFrac * 400), 0, 2, 240, 120, 255, 200);
		break; }
	case PG_GRAIN:
		rect((int)(grainPos * 400), 0, 1, 240, 200, 150, 255);
		for (int i = 0; i < MAXGR; i++) if (grainVisOn[i]) {
			int gx = (int)(grainVisPos[i] * 400), gy = 30 + (i * 37) % 180;
			rect(gx - 2, gy - 2, 5, 5, 255, 255, 255);
		}
		break;
	case PG_SEQ:
		if (playing && curStep >= 0) blend(curStep * 25, 0, 25, 240, 255, 255, 255, 60);
		for (int t = 0; t < NTR; t++) {
			int y = 22 + t * 26; const u8* c = TR_COL[t];
			snprintf(b, sizeof(b), "%-6s TUNE %3d DEC %3d %s", TR_LONG[t], (int)(trTune[t] * 100), (int)(trDecay[t] * 100), trMute[t] ? "MUTE" : "");
			if (t == selTrack) blend(0, y - 3, 400, 14, c[0], c[1], c[2], 90);
			text(8, y, b, 1, c[0], c[1], c[2]);
		}
		break;
	case PG_MIC: {
		if (smpLen > 0 && !recording) {
			for (int x = 0; x < 400; x++) { int h = smpPeak[x] * 70 / 128; rect(x, 120 - h, 1, h * 2 + 1, 240, 120, 190); }
			int a = (int)((long long)smpS * 400 / smpLen), e = (int)((long long)smpE * 400 / smpLen);
			rect(a, 30, 2, 180, 255, 220, 80); rect(e - 2, 30, 2, 180, 255, 220, 80);
		}
		if (recording) { rect(20, 110, (int)(micLevel * 360), 20, 255, 60, 60); textC(200, 80, "REC", 3, 255, 80, 80); }
		break; }
	case PG_KEYS:
		snprintf(b, sizeof(b), "%s %s", NOTE_NAMES[P.root], SCALES[P.scale].name); textC(200, 70, b, 3, 255, 230, 180);
		snprintf(b, sizeof(b), "PADS PLAY %s", KEYS_MODES[keysMode]); textC(200, 110, b, 2, 255, 255, 255);
		{
			int n = SCALES[P.scale].n; char s[64] = "";
			for (int i = 0; i < n; i++) { strcat(s, NOTE_NAMES[(P.root + SCALES[P.scale].iv[i]) % 12]); strcat(s, " "); }
			textC(200, 140, s, 1, 230, 230, 240);
		}
		break;
	}

	// header
	blend(0, 0, 400, 12, 0, 0, 0, 170);
	if (photoId) snprintf(b, sizeof(b), "PHOTOTAPE P%04d %s %s", photoId, NOTE_NAMES[P.root], SCALES[P.scale].name);
	else snprintf(b, sizeof(b), "PHOTOTAPE  %s %s", NOTE_NAMES[P.root], SCALES[P.scale].name);
	text(3, 2, b, 1, 255, 230, 190);
	snprintf(b, sizeof(b), "%s%3dBPM %2d", playing ? ">" : "#", tempo, playing ? curStep + 1 : 0);
	text(400 - 3 - (int)strlen(b) * 8, 2, b, 1, playing ? 120 : 200, 255, playing ? 160 : 200);
	if (playing) for (int s = 0; s < NSTEP; s++) rect(220 + s * 6, 4, 4, 4, s == curStep ? 255 : 80, s == curStep ? 255 : 80, 90);

	// footer hints
	static const char* H1[PG_COUNT] = {
		"A SNAP  X FLIP CAM  Y TAPE MODE  B SAVE PATCH",
		"A PLAY/STOP  B REVERSE  Y SYNC  X FULL LOOP",
		"TOUCH STRIP=PLAY  A/R LATCH  X SOURCE",
		"TAP GRID  L/R TRACK  A MUTE  B HEAR  X PHOTO-GEN",
		"A REC  B PLAY  L/R NUDGE START  ZL/ZR END",
		"PADS PLAY  R+PLAY=REC  UP/DN OCT  Y MODE",
	};
	static const char* H2[PG_COUNT] = {
		"<</>> BROWSE PHOTOS  UP/DN ROOT",
		"DRAG STRIP=LOOP  TAP=JUMP",
		"SLIDE ON STRIP TO SCRUB THE PHOTO",
		"STICK TUNE/DECAY  UP/DN BPM  LEFT/RIGHT SWING",
		"DRAG WAVE=SET START/END  D-PAD NUDGE",
		"DRUM PADS ALWAYS LIVE",
	};
	blend(0, 218, 400, 22, 0, 0, 0, 170);
	text(3, 220, H1[page], 1, 230, 230, 240);
	text(3, 230, H2[page], 1, 170, 170, 185);
	if (msgT > 0) {
		msgT--;
		int w = strlen(msg) * 16 + 16;
		rect(200 - w / 2, 96, w, 28, 230, 120, 60);
		textC(200, 102, msg, 2, 255, 255, 255);
	}
}

// ---------------------------------------------------------------- actions
static void loadLibIdx(void) {
	if (libIdx < 0) { photoId = 0; genDefaultPhoto(); onNewPhoto(); say("BUILT-IN"); return; }
	int id = lib[libIdx];
	char path[64]; snprintf(path, sizeof(path), PT_DIR "/P%04d.bmp", id);
	if (!loadBMP(path)) { say("LOAD FAILED"); return; }
	photoId = id; onNewPhoto();
	char s[24]; snprintf(s, sizeof(s), "P%04d", id); say(s);
}

static void doSnap(void) {
	if (!camOn || !camReady) return;
	for (int y = 0; y < PH; y++)
		for (int x = 0; x < PW; x++) {
			int sx = camInner ? PW - 1 - x : x;
			rgb565(camFrame[y * PW + sx], &photo[y][x][0], &photo[y][x][1], &photo[y][x][2]);
		}
	snapFlash = 10;
	ensureDir();
	scanLib();
	int id = nextPhotoId();
	char path[64]; snprintf(path, sizeof(path), PT_DIR "/P%04d.bmp", id);
	int ok = saveBMP(path);
	photoId = ok ? id : 0;
	analyzePhoto(); buildCaches();
	audioHold(1); buildWavetables(); audioHold(0);
	if (tapeMode == TM_MIC) tapeMode = TM_SPECTRAL;
	renderTape();
	if (ok) { scanLib(); for (int i = 0; i < libN; i++) if (lib[i] == id) libIdx = i; savePatch(id); }
	char s[24]; snprintf(s, sizeof(s), ok ? "SNAP P%04d" : "SNAP (NOT SAVED)", id); say(s);
}

static void doSavePatch(void) {
	ensureDir();
	if (!photoId) {
		scanLib(); int id = nextPhotoId();
		char path[64]; snprintf(path, sizeof(path), PT_DIR "/P%04d.bmp", id);
		if (!saveBMP(path)) { say("SAVE FAILED"); return; }
		photoId = id; scanLib(); for (int i = 0; i < libN; i++) if (lib[i] == id) libIdx = i;
	}
	if (savePatch(photoId)) { char s[24]; snprintf(s, sizeof(s), "SAVED P%04d", photoId); say(s); }
	else say("SAVE FAILED");
}

static void keysRecord(int tr, int val) {
	if (!playing || curStep < 0) return;
	int s = curStep; if (stepFrac > 0.5f) s = (s + 1) % NSTEP;
	seq[tr][s].on = 1; seq[tr][s].val = (s8)val;
}

// ---------------------------------------------------------------- main
int main(void) {
	gfxInitDefault();
	fontGfx = consoleGetDefault()->font.gfx;
	osSetSpeedupEnable(true);
	srand(osGetTime()); rngMain ^= (u32)osGetTime(); rngAud ^= (u32)(osGetTime() * 7);

	for (int i = 0; i <= 4096; i++) sinT[i] = sinf(TWO_PI_F * i / 4096.0f);
	for (int i = 0; i <= 1024; i++) hannT[i] = 0.5f - 0.5f * cosf(TWO_PI_F * i / 1024.0f);

	topCache = (u8*)malloc(PW * PH * 3);
	topDim = (u8*)malloc(PW * PH * 3);
	wtab = (float*)malloc(NTAB * TABN * sizeof(float));
	tape = (s16*)malloc(TAPE_MAX * 2);
	smp = (s16*)calloc(SMP_MAX, 2);
	smpUndo = (s16*)calloc(SMP_MAX, 2);
	rawMic = (s16*)malloc(RAW_MAX * 2 + 16);
	camBuf = (u16*)linearAlloc(PW * PH * 2);
	camFrame = (u16*)malloc(PW * PH * 2);
	micBuf = (u8*)memalign(0x1000, 0x30000);
	if (!topCache || !topDim || !wtab || !tape || !smp || !smpUndo || !rawMic || !camBuf || !camFrame || !micBuf) {
		consoleInit(GFX_TOP, NULL); printf("PhotoTape: out of memory\nPress START");
		while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
		gfxExit(); return 0;
	}
	memset(tapePeak, 0, sizeof(tapePeak)); memset(smpPeak, 0, sizeof(smpPeak));

	if (R_SUCCEEDED(micInit(micBuf, 0x30000))) { micOK = 1; micDataSize = micGetSampleDataSize(); }

	defaultPattern();
	genDefaultPhoto();
	analyzePhoto(); buildCaches(); buildWavetables();
	renderTape();   // audio not running yet; hold is a no-op
	scanLib();

	// audio
	Thread athr = NULL;
	audioOK = R_SUCCEEDED(ndspInit());
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
			memset(audioMem + i * BUF_SAMPLES * 2, 0, BUF_SAMPLES * 4);
			DSP_FlushDataCache(audioMem + i * BUF_SAMPLES * 2, BUF_SAMPLES * 4);
			ndspChnWaveBufAdd(0, &wbuf[i]);
		}
		LightEvent_Init(&audioEvent, RESET_ONESHOT);
		ndspSetCallback(audioCallback, NULL);
		s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		athr = threadCreate(audioThread, NULL, 32 * 1024, prio - 1, -2, false);
	}
	if (!audioOK) say("NO DSP FIRM - AUDIO OFF");

	camStart();

	// touch / drag state
	enum { DR_NONE, DR_STRIP, DR_SLIDER, DR_GRID, DR_LANE, DR_WAVE, DR_PAD };
	int drag = DR_NONE, dragIdx = 0, dragX0 = 0, dragMoved = 0, paintVal = 1, waveMarker = 0;
	int lastPaintCell = -1;
	int prevPage = page;

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown(), kHeld = hidKeysHeld(), kUp = hidKeysUp();
		if ((kHeld & KEY_SELECT) && (kDown & KEY_START)) break;
		if ((kDown & KEY_START) && !(kHeld & KEY_SELECT)) {
			playing = !playing;
			if (playing) seqResetReq = 1; else curStep = -1;
		}
		if (kDown & KEY_SELECT) page = (page + 1) % PG_COUNT;

		touchPosition t; int touching = !!(kHeld & KEY_TOUCH), touchDown = !!(kDown & KEY_TOUCH);
		if (touching) hidTouchRead(&t); else { t.px = 0; t.py = 0; }
		if (touchDown && t.py < 20) { page = t.px / 53; if (page >= PG_COUNT) page = PG_COUNT - 1; touchDown = 0; drag = DR_NONE; }

		if (page != prevPage) {
			if (page == PG_CAM) camStart(); else camStop();
			if (prevPage == PG_GRAIN) grainTouch = 0;
			if (prevPage == PG_KEYS && keysHeldPad >= 0) {
				int deg = keysHeldPad + keysOct * SCALES[P.scale].n;
				pushEv(keysMode == 0 ? EV_SYN_OFF : EV_GRAIN_OFF, 0, deg); keysHeldPad = -1;
			}
			if (prevPage == PG_MIC && recording) recStop();
			drag = DR_NONE; prevPage = page;
		}

		circlePosition cp; hidCircleRead(&cp);
		float cx = cp.dx / 156.0f, cy = cp.dy / 156.0f;
		if (fabsf(cx) < 0.15f) cx = 0; if (fabsf(cy) < 0.15f) cy = 0;

		switch (page) {
		// ------------------------------------------------ CAM
		case PG_CAM: {
			int prev = 0, next = 0;
			if (touchDown) {
				if (inBox(t.px, t.py, 0, 150, 80, 46)) prev = 1;
				else if (inBox(t.px, t.py, 240, 150, 80, 46)) next = 1;
				else if (inBox(t.px, t.py, 80, 150, 160, 46)) kDown |= KEY_A;
				else if (inBox(t.px, t.py, 0, 200, 106, 38)) kDown |= KEY_Y;
				else if (inBox(t.px, t.py, 106, 200, 107, 38)) kDown |= KEY_B;
				else if (inBox(t.px, t.py, 213, 200, 107, 38)) kDown |= KEY_X;
			}
			if (kDown & KEY_DLEFT) prev = 1;
			if (kDown & KEY_DRIGHT) next = 1;
			if (prev || next) {
				scanLib();
				if (next) libIdx = libIdx + 1 >= libN ? -1 : libIdx + 1;
				else libIdx = libIdx - 1 < -1 ? libN - 1 : libIdx - 1;
				loadLibIdx();
			}
			if (kDown & KEY_A) doSnap();
			if (kDown & KEY_X) { camStop(); camInner = !camInner; camStart(); }
			if (kDown & KEY_Y) { tapeMode = (tapeMode + 1) % TM_COUNT; if (tapeMode == TM_MIC && smpE - smpS < 1000) tapeMode = TM_SPECTRAL; renderTape(); say(TM_NAMES[tapeMode]); }
			if (kDown & KEY_B) doSavePatch();
			if (kDown & (KEY_DUP | KEY_DDOWN)) {
				P.root = (P.root + ((kDown & KEY_DUP) ? 1 : 11)) % 12;
				if (tapeMode == TM_SPECTRAL || tapeMode == TM_WAVE) renderTape();
				char s[16]; snprintf(s, sizeof(s), "ROOT %s", NOTE_NAMES[P.root]); say(s);
			}
			break; }
		// ------------------------------------------------ TAPE / GRAIN
		case PG_TAPE:
		case PG_GRAIN: {
			float* v = page == PG_TAPE ? tp : gp;
			if (touchDown) {
				if (inBox(t.px, t.py, 0, STRIP_Y, 320, STRIP_H)) { drag = DR_STRIP; dragX0 = t.px; dragMoved = 0; }
				else if (t.py >= SL_Y) { drag = DR_SLIDER; dragIdx = (t.py - SL_Y) / SL_H; if (dragIdx > 5) dragIdx = 5; }
			}
			if (touching && drag == DR_SLIDER) {
				float nv = clampf((t.px - 50) / 210.0f, 0, 1);
				if (page == PG_TAPE && dragIdx == 0) { tapeSync = 0; if (fabsf(nv - 0.5f) < 0.025f) nv = 0.5f; }
				if (page == PG_GRAIN && dragIdx == 3) nv = (roundf((nv - 0.5f) * 48.0f)) / 48.0f + 0.5f;
				v[dragIdx] = nv;
			}
			if (page == PG_TAPE) {
				if (touching && drag == DR_STRIP) {
					if (abs(t.px - dragX0) > 6) dragMoved = 1;
					if (dragMoved) {
						int a = dragX0 < t.px ? dragX0 : t.px, b = dragX0 < t.px ? t.px : dragX0;
						loopS = a / 320.0f; loopE = clampf((b + 1) / 320.0f, 0, 1);
					}
				}
				if (!touching && drag == DR_STRIP) {
					if (!dragMoved) {
						float f = dragX0 / 320.0f;
						if (f < loopS || f > loopE) { loopS = 0; loopE = 1; }
						tapeJumpReq = f;
						if (!tapeRun) tapeRun = 1;
					}
				}
				if (kDown & KEY_A) tapeRun = !tapeRun;
				if (kDown & KEY_B) tapeDir = -tapeDir;
				if (kDown & KEY_Y) { tapeSync = (tapeSync + 1) % 4; say(tapeSync ? (tapeSync == 1 ? "SYNC 1 BAR" : tapeSync == 2 ? "SYNC 2 BARS" : "SYNC 4 BARS") : "SYNC OFF"); }
				if (kDown & KEY_X) { loopS = 0; loopE = 1; }
			} else {
				if (touching && drag == DR_STRIP) { grainPos = clampf(t.px / 320.0f, 0, 0.999f); grainTouch = 1; }
				else grainTouch = 0;
				if (kDown & (KEY_A | KEY_R)) { grainLatch = !grainLatch; say(grainLatch ? "LATCH ON" : "LATCH OFF"); }
				if (kDown & KEY_X) { grainSrc = !grainSrc; if (grainSrc && smpE - smpS < 512) { grainSrc = 0; say("RECORD A SAMPLE FIRST"); } else say(grainSrc ? "SOURCE MIC" : "SOURCE TAPE"); }
				if (cx != 0) grainPos = clampf(grainPos + cx * 0.006f, 0, 0.999f);
			}
			if (!touching) drag = DR_NONE;
			break; }
		// ------------------------------------------------ SEQ
		case PG_SEQ: {
			if (touchDown) {
				if (t.py >= 22 && t.py < 22 + NTR * 16) {
					int tr = (t.py - 22) / 16;
					if (t.px < 32) { selTrack = tr; drag = DR_NONE; }
					else {
						int s = (t.px - 32) / 18; if (s > 15) s = 15;
						paintVal = !seq[tr][s].on; seq[tr][s].on = paintVal; selTrack = tr;
						drag = DR_GRID; lastPaintCell = tr * 16 + s;
					}
				} else if (t.py >= 138 && t.py < 218 && t.px >= 32) drag = DR_LANE;
			}
			if (touching && drag == DR_GRID && t.py >= 22 && t.py < 22 + NTR * 16 && t.px >= 32) {
				int tr = (t.py - 22) / 16, s = (t.px - 32) / 18; if (s > 15) s = 15;
				if (tr * 16 + s != lastPaintCell) { seq[tr][s].on = paintVal; lastPaintCell = tr * 16 + s; }
			}
			if (touching && drag == DR_LANE && t.px >= 32) {
				int s = (t.px - 32) / 18; if (s > 15) s = 15;
				float f = clampf((218 - t.py) / 80.0f, 0, 1);
				if (selTrack >= T_SYNTH) seq[selTrack][s].val = (s8)(roundf(f * 21) - 7);
				else seq[selTrack][s].val = (s8)clampf(roundf(f * 15), 1, 15);
			}
			if (!touching) drag = DR_NONE;
			if (kDown & KEY_L) selTrack = (selTrack + NTR - 1) % NTR;
			if (kDown & KEY_R) selTrack = (selTrack + 1) % NTR;
			if (kDown & KEY_A) trMute[selTrack] = !trMute[selTrack];
			if (kDown & KEY_X) { genFromPhoto(selTrack); say("FROM PHOTO"); }
			if (kDown & KEY_Y) { for (int s = 0; s < NSTEP; s++) seq[selTrack][s].on = 0; say("CLEARED"); }
			if (kDown & KEY_B) pushEv(EV_DRUM, selTrack, selTrack >= T_SYNTH ? 0 : 12);
			static int rep = 0;
			if (kHeld & (KEY_DUP | KEY_DDOWN)) {
				if ((kDown & (KEY_DUP | KEY_DDOWN)) || (++rep > 20 && rep % 3 == 0)) {
					int nt = tempo + ((kHeld & KEY_DUP) ? 1 : -1); tempo = nt < 40 ? 40 : (nt > 240 ? 240 : nt);
				}
			} else rep = 0;
			if (kDown & KEY_DRIGHT) swing = clampf(swing + 0.05f, 0, 0.6f);
			if (kDown & KEY_DLEFT) swing = clampf(swing - 0.05f, 0, 0.6f);
			if (cx != 0) trTune[selTrack] = clampf(trTune[selTrack] + cx * 0.012f, 0, 1);
			if (cy != 0) trDecay[selTrack] = clampf(trDecay[selTrack] + cy * 0.012f, 0, 1);
			break; }
		// ------------------------------------------------ MIC
		case PG_MIC: {
			if (touchDown) {
				if (inBox(t.px, t.py, 0, 22, 320, 100) && smpLen > 0 && !recording) {
					int sx = (int)((long long)t.px * smpLen / 320);
					waveMarker = abs(sx - smpS) <= abs(sx - smpE) ? 0 : 1;
					drag = DR_WAVE;
				}
				else if (inBox(t.px, t.py, 0, 128, 80, 34)) kDown |= KEY_A;
				else if (inBox(t.px, t.py, 80, 128, 80, 34)) kDown |= KEY_B;
				else if (inBox(t.px, t.py, 160, 128, 80, 34)) { edTrim(); say("TRIM"); }
				else if (inBox(t.px, t.py, 240, 128, 80, 34)) { edNorm(); say("NORMALIZE"); }
				else if (inBox(t.px, t.py, 0, 164, 80, 34)) { edRev(); say("REVERSE"); }
				else if (inBox(t.px, t.py, 80, 164, 80, 34)) { edFade(); say("FADE"); }
				else if (inBox(t.px, t.py, 160, 164, 80, 34)) { edUndo(); say(haveUndo ? "UNDO" : "NOTHING TO UNDO"); }
				else if (inBox(t.px, t.py, 240, 164, 80, 34)) {
					if (smpE - smpS >= 1000) { tapeMode = TM_MIC; renderTape(); say("SAMPLE -> TAPE"); } else say("RECORD A SAMPLE FIRST");
				}
				else if (inBox(t.px, t.py, 0, 200, 106, 38)) {
					if (smpE - smpS > 0) {
						ensureDir();
						static int wavN = 0; char path[64]; FILE* f;
						do { wavN++; snprintf(path, sizeof(path), PT_DIR "/S%04d.wav", wavN); f = fopen(path, "rb"); if (f) fclose(f); } while (f && wavN < 9999);
						char s[24]; snprintf(s, sizeof(s), "S%04d.WAV", wavN);
						say(saveWAV(path, smp + smpS, smpE - smpS) ? s : "SAVE FAILED");
					}
				}
				else if (inBox(t.px, t.py, 106, 200, 107, 38)) {
					grainSrc = !grainSrc; if (grainSrc && smpE - smpS < 512) grainSrc = 0;
				}
				else if (inBox(t.px, t.py, 213, 200, 107, 38)) micGainIdx = (micGainIdx + 1) % 4;
			}
			if (touching && drag == DR_WAVE && smpLen > 0) {
				int sx = (int)((long long)clampf(t.px, 0, 320) * smpLen / 320);
				if (waveMarker == 0) smpS = sx < smpE - 64 ? sx : smpE - 64; else smpE = sx > smpS + 64 ? sx : smpS + 64;
				if (smpS < 0) smpS = 0; if (smpE > smpLen) smpE = smpLen;
			}
			if (!touching) drag = DR_NONE;
			if (kDown & KEY_A) { if (recording) { if (recStop()) say("GOT IT"); else say("TOO SHORT"); } else { recStart(); if (!recording) say("MIC ERROR"); } }
			if (kDown & KEY_B) pushEv(EV_SMP, 0, 0);
			int step = smpLen / 400 + 1;
			if (kHeld & KEY_L) smpS = smpS - step < 0 ? 0 : smpS - step;
			if (kHeld & KEY_R) smpS = smpS + step < smpE - 64 ? smpS + step : smpS;
			if (kHeld & KEY_ZL) smpE = smpE - step > smpS + 64 ? smpE - step : smpE;
			if (kHeld & KEY_ZR) smpE = smpE + step > smpLen ? smpLen : smpE + step;
			if (kHeld & KEY_DLEFT) smpS = smpS - step < 0 ? 0 : smpS - step;
			if (kHeld & KEY_DRIGHT) smpS = smpS + step < smpE - 64 ? smpS + step : smpS;
			if (kHeld & KEY_DDOWN) smpE = smpE - step > smpS + 64 ? smpE - step : smpE;
			if (kHeld & KEY_DUP) smpE = smpE + step > smpLen ? smpLen : smpE + step;
			if (recording && rawLen >= RAW_MAX) { recStop(); say("MAX 6s"); }
			break; }
		// ------------------------------------------------ KEYS
		case PG_KEYS: {
			int n = SCALES[P.scale].n;
			int pad = -1;
			if (touching && t.py >= 22 && t.py < 122) { int col = t.px / 40, row = 1 - (t.py - 22) / 50; if (col > 7) col = 7; pad = row * 8 + col; }
			if (pad != keysHeldPad) {
				if (keysHeldPad >= 0) {
					int deg = keysHeldPad + keysOct * n;
					if (keysMode == 0) pushEv(EV_SYN_OFF, 0, deg); else if (keysMode == 1) pushEv(EV_GRAIN_OFF, 0, deg);
				}
				if (pad >= 0) {
					int deg = pad + keysOct * n;
					if (keysMode == 0) pushEv(EV_SYN_ON, 0, deg);
					else if (keysMode == 1) pushEv(EV_GRAIN_ON, 0, deg);
					else pushEv(EV_SMP, 0, deg);
					if (kHeld & KEY_R) keysRecord(T_SYNTH + keysMode, deg);
				}
				keysHeldPad = pad;
			}
			if (touchDown && t.py >= 126 && t.py < 180) {
				int d = t.px / 80; if (d > 3) d = 3;
				pushEv(EV_DRUM, d, 12);
				if (kHeld & KEY_R) keysRecord(d, 12);
			}
			if (touchDown && t.py >= 184 && t.py < 216) {
				int m = t.px / 106; if (m > 2) m = 2;
				keysMode = m;
			}
			if (kDown & KEY_Y) { if (keysHeldPad < 0) keysMode = (keysMode + 1) % 3; }
			if (kDown & KEY_DUP) keysOct = keysOct < 2 ? keysOct + 1 : 2;
			if (kDown & KEY_DDOWN) keysOct = keysOct > -2 ? keysOct - 1 : -2;
			if (kDown & KEY_A) pushEv(EV_DRUM, T_KICK, 12);
			if (kDown & KEY_B) pushEv(EV_DRUM, T_SNARE, 12);
			if (kDown & KEY_X) pushEv(EV_DRUM, T_HAT, 12);
			if (kDown & KEY_L) pushEv(EV_DRUM, T_PERC, 12);
			break; }
		}
		(void)kUp;

		camPoll();
		recPoll();

		// draw
		drawTop();
		fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320;
		switch (page) {
		case PG_CAM: drawBottomCam(); break;
		case PG_TAPE: rect(0, 20, 320, 220, 18, 16, 22); drawStrip(1); drawSliders(PG_TAPE); break;
		case PG_GRAIN: rect(0, 20, 320, 220, 18, 16, 22); drawStrip(0); drawSliders(PG_GRAIN); break;
		case PG_SEQ: drawBottomSeq(); break;
		case PG_MIC: drawBottomMic(); break;
		case PG_KEYS: drawBottomKeys(); break;
		}
		drawTabs();

		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}

	if (recording) MICU_StopSampling();
	camStop();
	if (audioOK) {
		audioRun = 0; LightEvent_Signal(&audioEvent);
		if (athr) { threadJoin(athr, U64_MAX); threadFree(athr); }
		ndspChnWaveBufClear(0); ndspExit(); linearFree(audioMem);
	}
	if (micOK) micExit();
	gfxExit();
	return 0;
}
