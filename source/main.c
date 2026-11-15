// PadBox - mini MPC-style sampling drum machine for Nintendo 2DS / 3DS (homebrew, libctru)
//
// Bottom screen: 16 velocity pads (4 banks A-D = 64 pads), page tabs, bank / BTN / 16 LEVELS toggles.
// Top screen: the "LCD" for the current page: MAIN  PROG  STEP  SMPL  CHOP  DISK.
//
// Global controls
//   START ........ play / stop             SELECT ..... next page   (SELECT+START quits)
//   A ............ REC (overdub) on/off    X .......... undo / redo last record pass or erase
//   B (hold)+pad . erase that pad from the sequence      L+B ... clear the whole sequence
//   Y ............ FULL LEVEL on/off (off = touch height sets velocity)
//   R (hold)+pad . NOTE REPEAT at the QUANT rate (records when REC is on)
//   L (hold)+pad . select a pad without playing it     L+Y ... 16 LEVELS   L+X ... tap tempo
//   L+A .......... metronome on/off        L+D-pad up/down .. bank
//   D-pad ........ up/down = field, left/right = value (hold L + left/right for big steps)
//   Circle pad ... DJ filter on the master (down = low-pass, up = high-pass)
// BTN mode (tap BTN): D-pad + Y B A X + L play pads 1-9 with your thumbs, SELECT = REC.
//
// Files: sdmc:/3ds/padbox/  (samples/ for WAVs, projects/Pnn/ for saved projects)

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <malloc.h>
#include <dirent.h>
#include <sys/stat.h>

#define SR          32000
#define BUF_SAMPLES 256
#define NBUFS       4
#define NBANKS      4
#define NPADS       64
#define MAXSMP      128
#define MAXVOX      16
#define NSEQ        16
#define MAXEV       4096
#define PPQ         960
#define TPB         (PPQ * 4)
#define POOL_LIMIT  (28 * 1024 * 1024)
#define MIC_RATE    32730.0f
#define PB_DIR      "sdmc:/3ds/padbox"
#define PI_F        3.14159265f

// ================================================================ samples
typedef struct { s16* d; int len; char name[16]; } Sample;
static Sample smp[MAXSMP];
static int poolBytes = 0;

static int smpAlloc(int len, const char* name) {
	if (len < 2) return -1;
	if (poolBytes + len * 2 > POOL_LIMIT) return -1;
	for (int i = 0; i < MAXSMP; i++) if (!smp[i].d) {
		smp[i].d = (s16*)malloc(len * 2);
		if (!smp[i].d) return -1;
		smp[i].len = len; poolBytes += len * 2;
		snprintf(smp[i].name, sizeof(smp[i].name), "%s", name);
		return i;
	}
	return -1;
}
static void smpFreeSlot(int i) {
	if (i < 0 || i >= MAXSMP || !smp[i].d) return;
	free(smp[i].d); poolBytes -= smp[i].len * 2;
	smp[i].d = NULL; smp[i].len = 0; smp[i].name[0] = 0;
}

// ================================================================ pads
typedef struct {
	s16 smp;            // sample slot or -1
	int start, end;     // region in samples
	s8 tune, fine;      // semitones, cents
	u8 level;           // 0-100
	s8 pan;             // -50..50
	u8 attack, decay;   // 0-100 (decay 100 = off)
	u8 mode;            // 0 one shot, 1 note on
	u8 mgroup;          // 0 off, 1-8
	u8 cutoff, reso;    // 0-100 (cutoff 100 = open)
	u8 reverse;
} Pad;
static Pad pads[NPADS];

static void padReset(Pad* p) {
	memset(p, 0, sizeof(*p));
	p->smp = -1; p->level = 80; p->decay = 100; p->cutoff = 100;
}
static void padAssign(int pi, int s) {
	Pad* p = &pads[pi];
	p->smp = s; p->start = 0; p->end = (s >= 0) ? smp[s].len : 0;
}
static const char* padSmpName(int pi) {
	int s = pads[pi].smp;
	return (s >= 0 && smp[s].d) ? smp[s].name : "";
}

// ================================================================ voices
typedef struct {
	int on, pad, s, holdId;
	const s16* d;
	int pos, start, end, dir, played;
	float frac, step;
	float gl, gr;
	float env, att, dec, rel;
	int stage;           // 0 attack, 1 body, 2 release
	int noteOn, decOn, gate;
	int filt; float a1, a2, a3, k, ic1, ic2;
	u32 age;
} Voice;
static Voice vox[MAXVOX];
static u32 voxAge = 0;
static volatile u8 hitFlag[NPADS];   // set by audio, read by UI for pad flashes

static void voiceCutGroup(int g) {
	for (int i = 0; i < MAXVOX; i++) {
		Voice* v = &vox[i];
		if (v->on && pads[v->pad].mgroup == g) { v->stage = 2; v->rel = 0.993f; }
	}
}

static void voiceStart(int pad, int trans, int vel, int gate, int holdId) {
	Pad* p = &pads[pad];
	if (p->smp < 0 || !smp[p->smp].d) return;
	Sample* S = &smp[p->smp];
	int st = p->start, en = p->end;
	if (en > S->len) en = S->len;
	if (st < 0) st = 0;
	if (en - st < 4) return;
	if (p->mgroup) voiceCutGroup(p->mgroup);
	Voice* v = NULL;
	for (int i = 0; i < MAXVOX; i++) if (!vox[i].on) { v = &vox[i]; break; }
	if (!v) {   // steal: prefer releasing, then oldest
		u32 best = 0xffffffff;
		for (int i = 0; i < MAXVOX; i++) {
			u32 a = vox[i].age - (vox[i].stage == 2 ? 0x40000000u : 0);
			if (a < best) { best = a; v = &vox[i]; }
		}
	}
	memset(v, 0, sizeof(*v));
	v->on = 1; v->pad = pad; v->s = p->smp; v->d = S->d; v->holdId = holdId;
	v->start = st; v->end = en;
	v->dir = p->reverse ? -1 : 1;
	v->pos = p->reverse ? en - 1 : st;
	v->step = powf(2.0f, (p->tune + trans + p->fine / 100.0f) / 12.0f);
	float g = (p->level / 100.0f) * (p->level / 100.0f) * 1.6f * (vel / 127.0f);
	float ang = (p->pan + 50) / 100.0f * (PI_F / 2);
	v->gl = g * cosf(ang) * 1.414f; v->gr = g * sinf(ang) * 1.414f;
	if (p->attack) { v->stage = 0; v->env = 0; v->att = 1.0f / (p->attack / 100.0f * p->attack / 100.0f * SR + 1); }
	else { v->stage = 1; v->env = 1; }
	v->decOn = p->decay < 100;
	float dt = 0.02f * powf(250.0f, p->decay / 99.0f);
	v->dec = expf(-1.0f / (dt * SR));
	v->noteOn = p->mode == 1;
	v->gate = gate;
	v->rel = v->decOn ? v->dec : 0.997f;
	if (p->cutoff < 100) {
		float fc = 40.0f * powf(2.0f, p->cutoff / 100.0f * 8.6f);
		if (fc > 14000) fc = 14000;
		float gg = tanf(PI_F * fc / SR);
		v->k = 2.0f - 2.0f * (p->reso / 100.0f) * 0.96f;
		v->a1 = 1.0f / (1.0f + gg * (gg + v->k)); v->a2 = gg * v->a1; v->a3 = gg * v->a2;
		v->filt = 1;
	}
	v->age = ++voxAge;
	hitFlag[pad] = vel ? vel : 1;
}

static void voiceReleaseHold(int holdId) {
	for (int i = 0; i < MAXVOX; i++)
		if (vox[i].on && vox[i].holdId == holdId && vox[i].noteOn && vox[i].stage != 2) vox[i].stage = 2;
}
static void voicesKillSample(int s) {
	for (int i = 0; i < MAXVOX; i++) if (vox[i].s == s) vox[i].on = 0;
}

// ================================================================ sequencer data
typedef struct { u32 tick; u16 len; u8 pad; u8 vel; s8 trans; u8 flags; } Event;
#define EVF_SKIP 1
static Event seqEv[NSEQ][MAXEV];
static int seqN[NSEQ];
static u8 seqBars[NSEQ];
static Event undoEv[MAXEV]; static int undoN = -1, undoSeq = 0, undoBars = 1;

static int bpmX10 = 900, swing = 50, quantSel = 4, metroSel = 0, countIn = 1, vintage = 0, masterVol = 80, fullLevel = 1;
static int curSeq = 0;
static const char* QUANT_NAMES[7] = {"OFF","1/4","1/8","1/8T","1/16","1/16T","1/32"};
static const int QUANT_TICKS[7] = {0, 960, 480, 320, 240, 160, 120};
static const char* VINT_NAMES[4] = {"CLEAN","MPC60","SP-1200","LOFI"};
static const char* METRO_NAMES[3] = {"OFF","ON","REC ONLY"};

static int seqLen(void) { return seqBars[curSeq] * TPB; }

static volatile int playState = 0;     // 0 stop, 1 count-in, 2 play
static volatile int recOn = 0;
static double songTick = 0, ciTick = 0, freeTick = 0;
static int lastTick = -1, lastFree = -1, evIdx = 0;
static LightLock alock;

static int lowerBound(int sq, u32 t) {
	int lo = 0, hi = seqN[sq];
	while (lo < hi) { int m = (lo + hi) / 2; if (seqEv[sq][m].tick < t) lo = m + 1; else hi = m; }
	return lo;
}
static void seqResync(void) { evIdx = lowerBound(curSeq, (u32)(lastTick + 1)); }

static int evInsert(int sq, u32 tick, int pad, int trans, int vel, int len, int flags) {
	Event* E = seqEv[sq];
	int i = lowerBound(sq, tick);
	for (int j = i; j < seqN[sq] && E[j].tick == tick; j++)
		if (E[j].pad == pad && E[j].trans == trans) { E[j].vel = vel; E[j].len = len; E[j].flags = flags; return j; }
	if (seqN[sq] >= MAXEV) return -1;
	memmove(&E[i + 1], &E[i], (seqN[sq] - i) * sizeof(Event));
	E[i].tick = tick; E[i].pad = pad; E[i].trans = trans; E[i].vel = vel; E[i].len = len; E[i].flags = flags;
	seqN[sq]++;
	if (sq == curSeq && i < evIdx) evIdx++;
	return i;
}
static void evDelete(int sq, int i) {
	memmove(&seqEv[sq][i], &seqEv[sq][i + 1], (seqN[sq] - i - 1) * sizeof(Event));
	seqN[sq]--;
	if (sq == curSeq && i < evIdx) evIdx--;
}
static void undoSnap(void) {
	memcpy(undoEv, seqEv[curSeq], seqN[curSeq] * sizeof(Event));
	undoN = seqN[curSeq]; undoSeq = curSeq; undoBars = seqBars[curSeq];
}
static int undoSwap(void) {
	if (undoN < 0 || undoSeq != curSeq) return 0;
	static Event tmp[MAXEV];
	int n = seqN[curSeq], b = seqBars[curSeq];
	memcpy(tmp, seqEv[curSeq], n * sizeof(Event));
	memcpy(seqEv[curSeq], undoEv, undoN * sizeof(Event));
	seqN[curSeq] = undoN; seqBars[curSeq] = undoBars;
	memcpy(undoEv, tmp, n * sizeof(Event)); undoN = n; undoBars = b;
	seqResync();
	return 1;
}

// ================================================================ held pads / note repeat
typedef struct { int on, pad, trans, vel, evTick, pressTick, lastRep; } Hold;
static Hold hold[16];
static volatile int repeatOn = 0;
static int recPassArmed = 0;     // take an undo snapshot at the first note of a record pass

static int repDiv(void) { int d = QUANT_TICKS[quantSel]; return d ? d : 240; }
static int swingDelay(int t, int div) {
	if (swing <= 50 || (div != 240 && div != 120)) return 0;
	if (t % 480 == 240) return (swing - 50) * 240 / 50;
	return 0;
}

static void recordHit(int pad, int trans, int vel, int len, Hold* h) {
	if (!recOn || playState != 2) return;
	if (recPassArmed) { undoSnap(); recPassArmed = 0; }
	int L = seqLen();
	int t = (int)songTick;
	int div = QUANT_TICKS[quantSel], q = t;
	if (div) { q = ((t + div / 2) / div) * div; }
	if (q >= L) q -= L;
	int ahead = ((q - lastTick) % L + L) % L;
	int flags = (ahead > 0 && ahead < (div ? div : 1)) ? EVF_SKIP : 0;
	evInsert(curSeq, q, pad, trans, vel, len, flags);
	if (h) { h->evTick = q; h->pressTick = t; }
}

// ================================================================ audio engine
typedef struct { int due, pad, trans, vel, len; } Pend;
static Pend pend[64]; static int npend = 0;
static float metroPh = 0, metroEnv = 0, metroF = 1000;
static float mfIc1L, mfIc2L, mfIc1R, mfIc2R, mfG = 1, mfK = 1.4f; static volatile float circleY = 0; static float mfPos = 0;
static float vinHoldL, vinHoldR, vinPh;
static volatile float meterL = 0;

static float bpm(void) { return bpmX10 / 10.0f; }
static int ticksToSamples(int t) { return (int)(t * 60.0f * SR / (bpm() * PPQ)); }

static void playEvent(int pad, int trans, int vel, int len) {
	voiceStart(pad, trans, vel, ticksToSamples(len > 0 ? len : 240), 0);
}

static void clickMetro(int accent) { metroEnv = 1.0f; metroF = accent ? 1600 : 1050; metroPh = 0; }

static void songTickFn(int t) {
	int sq = curSeq;
	Event* E = seqEv[sq];
	while (evIdx < seqN[sq] && E[evIdx].tick <= (u32)t) {
		Event* e = &E[evIdx++];
		if ((int)e->tick < t) continue;
		if (e->flags & EVF_SKIP) { e->flags &= ~EVF_SKIP; continue; }
		int d = swingDelay(t, 240);
		if (d && npend < 64) { Pend* p = &pend[npend++]; p->due = t + d; p->pad = e->pad; p->trans = e->trans; p->vel = e->vel; p->len = e->len; }
		else playEvent(e->pad, e->trans, e->vel, e->len);
	}
	for (int i = 0; i < npend; i++) if (pend[i].due <= t) {
		playEvent(pend[i].pad, pend[i].trans, pend[i].vel, pend[i].len);
		pend[i--] = pend[--npend];
	}
	if (t % PPQ == 0 && (metroSel == 1 || (metroSel == 2 && recOn))) clickMetro(t % TPB == 0);
}

static void repeatTick(int t, int song) {
	if (!repeatOn) return;
	int div = repDiv();
	if (t % div) return;
	for (int k = 0; k < 16; k++) {
		Hold* h = &hold[k];
		if (!h->on) continue;
		int since = t - h->lastRep; if (since < 0) since += 1 << 20;
		if (since < div / 2) continue;
		h->lastRep = t;
		int d = song ? swingDelay(t, div) : 0;
		if (d && npend < 64) { Pend* p = &pend[npend++]; p->due = t + d; p->pad = h->pad; p->trans = h->trans; p->vel = h->vel; p->len = div / 2; }
		else voiceStart(h->pad, h->trans, h->vel, ticksToSamples(div / 2), 0);
		if (song) recordHit(h->pad, h->trans, h->vel, div / 2, NULL);
	}
}

static inline float vintageProc(float x, float* hold, int ch) {
	switch (vintage) {
	case 1: return floorf(x * 2048.0f) / 2048.0f;                      // MPC60: 12-bit
	case 2: if (vinPh >= 1.0f) *hold = floorf(x * 2048.0f) / 2048.0f; return *hold;   // SP-1200: 12-bit @ 26 kHz
	case 3: if (vinPh >= 1.0f) *hold = floorf(x * 128.0f) / 128.0f; return *hold;     // 8-bit @ 11 kHz
	}
	(void)ch; return x;
}

static void fillAudio(s16* out, int n) {
	float tps = bpm() * PPQ / (60.0f * SR);
	// master DJ filter target
	float cy = circleY;
	float tgt = (cy < -0.12f) ? cy : (cy > 0.12f ? cy : 0);
	mfPos += (tgt - mfPos) * 0.2f;
	float fc;
	if (mfPos < 0) fc = 16000.0f * powf(2.0f, mfPos * 7.0f);   // LP: 16k down to ~125 Hz
	else fc = 20.0f * powf(2.0f, mfPos * 9.0f);                 // HP: 20 Hz up to ~10 kHz
	if (fc > 14500) fc = 14500;
	mfG = tanf(PI_F * fc / SR); mfK = 1.2f;
	float ma1 = 1.0f / (1.0f + mfG * (mfG + mfK)), ma2 = mfG * ma1, ma3 = mfG * ma2;
	int useMF = fabsf(mfPos) > 0.01f, hp = mfPos > 0;
	float vinInc = vintage == 2 ? 26040.0f / SR : (vintage == 3 ? 11025.0f / SR : 1.0f);
	float mv = (masterVol / 100.0f) * 0.55f, pk = 0;

	for (int i = 0; i < n; i++) {
		// ---- clocks
		freeTick += tps;
		if (playState == 1) {
			int before = (int)(ciTick / PPQ); ciTick += tps; int after = (int)(ciTick / PPQ);
			if (after != before && after < 4) clickMetro(0);
			if (ciTick >= TPB) { playState = 2; songTick = 0; lastTick = -1; evIdx = 0; npend = 0; }
		}
		if (playState == 2) {
			songTick += tps;
			int L = seqLen();
			int target = (int)songTick;
			while (lastTick < target) {
				lastTick++;
				if (lastTick >= L) {
					lastTick -= L; songTick -= L; target -= L; evIdx = 0;
					for (int j = 0; j < npend; j++) pend[j].due -= L;
				}
				songTickFn(lastTick);
				repeatTick(lastTick, 1);
			}
		} else {
			int target = (int)freeTick;
			while (lastFree < target) { lastFree++; repeatTick(lastFree & ((1 << 20) - 1), 0); }
			for (int j = 0; j < npend; j++) if (pend[j].due <= (lastFree & ((1 << 20) - 1))) {
				voiceStart(pend[j].pad, pend[j].trans, pend[j].vel, ticksToSamples(pend[j].len), 0); pend[j--] = pend[--npend];
			}
		}
		if (lastFree > (1 << 20)) { lastFree -= 1 << 20; freeTick -= 1 << 20; }

		// ---- voices
		float L = 0, R = 0;
		for (int vi = 0; vi < MAXVOX; vi++) {
			Voice* v = &vox[vi];
			if (!v->on) continue;
			int p = v->pos, p1 = p + v->dir;
			float s0 = v->d[p], s1 = (p1 >= v->start && p1 < v->end) ? v->d[p1] : 0;
			float x = (s0 + (s1 - s0) * v->frac) * (1.0f / 32768.0f);
			int remain = v->dir > 0 ? v->end - p : p - v->start + 1;
			if (remain < 48) x *= remain / 48.0f;
			if (v->played < 24) x *= v->played / 24.0f;
			v->played++;
			v->frac += v->step;
			int adv = (int)v->frac; v->frac -= adv;
			v->pos += adv * v->dir;
			if (v->pos >= v->end || v->pos < v->start) v->on = 0;
			if (v->filt) {
				float v3 = x - v->ic2;
				float v1 = v->a1 * v->ic1 + v->a2 * v3;
				float v2 = v->ic2 + v->a2 * v->ic1 + v->a3 * v3;
				v->ic1 = 2 * v1 - v->ic1; v->ic2 = 2 * v2 - v->ic2;
				x = v2;
			}
			switch (v->stage) {
			case 0: v->env += v->att; if (v->env >= 1) { v->env = 1; v->stage = 1; } break;
			case 1:
				if (v->noteOn) {
					if (v->gate > 0 && --v->gate == 0) v->stage = 2;
				} else if (v->decOn) v->env *= v->dec;
				break;
			case 2: v->env *= v->rel; break;
			}
			if (v->env < 0.0005f && v->stage != 0) v->on = 0;
			L += x * v->env * v->gl; R += x * v->env * v->gr;
		}
		// ---- metronome
		if (metroEnv > 0.001f) {
			metroPh += metroF / SR; if (metroPh > 1) metroPh -= 1;
			float c = (metroPh < 0.5f ? 0.25f : -0.25f) * metroEnv;
			metroEnv *= 0.9985f;
			L += c; R += c;
		}
		// ---- master
		L *= mv; R *= mv;
		vinPh += vinInc;
		if (vintage) { L = vintageProc(L, &vinHoldL, 0); R = vintageProc(R, &vinHoldR, 1); }
		if (vinPh >= 1.0f) vinPh -= 1.0f;
		if (useMF) {
			float v3 = L - mfIc2L, v1 = ma1 * mfIc1L + ma2 * v3, v2 = mfIc2L + ma2 * mfIc1L + ma3 * v3;
			mfIc1L = 2 * v1 - mfIc1L; mfIc2L = 2 * v2 - mfIc2L; L = hp ? L - mfK * v1 - v2 : v2;
			v3 = R - mfIc2R; v1 = ma1 * mfIc1R + ma2 * v3; v2 = mfIc2R + ma2 * mfIc1R + ma3 * v3;
			mfIc1R = 2 * v1 - mfIc1R; mfIc2R = 2 * v2 - mfIc2R; R = hp ? R - mfK * v1 - v2 : v2;
		} else { mfIc1L = mfIc2L = mfIc1R = mfIc2R = 0; }
		// soft clip
		if (L > 1.5f) L = 1.5f; if (L < -1.5f) L = -1.5f;
		if (R > 1.5f) R = 1.5f; if (R < -1.5f) R = -1.5f;
		L = L - L * L * L * 0.148f; R = R - R * R * R * 0.148f;
		float a = fabsf(L) > fabsf(R) ? fabsf(L) : fabsf(R); if (a > pk) pk = a;
		out[i * 2] = (s16)(L * 32000.0f); out[i * 2 + 1] = (s16)(R * 32000.0f);
	}
	meterL = pk > meterL ? pk : meterL * 0.9f;
}

// ---- audio thread
static ndspWaveBuf wbuf[NBUFS];
static s16* audioMem = NULL;
static LightEvent audioEv;
static volatile int audioQuit = 0;

static void ndspCb(void* arg) { (void)arg; LightEvent_Signal(&audioEv); }
static void audioThread(void* arg) {
	(void)arg;
	while (!audioQuit) {
		for (int i = 0; i < NBUFS; i++) {
			if (wbuf[i].status == NDSP_WBUF_DONE) {
				LightLock_Lock(&alock);
				fillAudio((s16*)wbuf[i].data_vaddr, BUF_SAMPLES);
				LightLock_Unlock(&alock);
				DSP_FlushDataCache(wbuf[i].data_vaddr, BUF_SAMPLES * 4);
				ndspChnWaveBufAdd(0, &wbuf[i]);
			}
		}
		LightEvent_Wait(&audioEv);
	}
}

// ================================================================ drum synthesis (built-in kits)
static u32 rngS = 0x1234567;
static inline float frand(void) { rngS = rngS * 1664525u + 1013904223u; return ((rngS >> 9) / 4194304.0f) - 1.0f; }

typedef struct { float ic1, ic2, a1, a2, a3, k; } SVF;
static void svfSet(SVF* f, float fc, float q) {
	if (fc > SR * 0.45f) fc = SR * 0.45f;
	float g = tanf(PI_F * fc / SR); f->k = 1.0f / q;
	f->a1 = 1.0f / (1.0f + g * (g + f->k)); f->a2 = g * f->a1; f->a3 = g * f->a2;
}
// mode 0 LP 1 BP 2 HP
static inline float svf(SVF* f, float x, int mode) {
	float v3 = x - f->ic2, v1 = f->a1 * f->ic1 + f->a2 * v3, v2 = f->ic2 + f->a2 * f->ic1 + f->a3 * v3;
	f->ic1 = 2 * v1 - f->ic1; f->ic2 = 2 * v2 - f->ic2;
	return mode == 0 ? v2 : (mode == 1 ? v1 : x - f->k * v1 - v2);
}

static float* gbuf; static int gN;
static void gStart(float secs) { gN = (int)(secs * SR); memset(gbuf, 0, gN * sizeof(float)); }
static int gFinish(int pad, const char* name, int mgroup) {
	float pk = 0.0001f;
	for (int i = 0; i < gN; i++) { float a = fabsf(gbuf[i]); if (a > pk) pk = a; }
	int n = gN;   // trim silent tail
	while (n > 64 && fabsf(gbuf[n - 1]) < pk * 0.0015f) n--;
	int s = smpAlloc(n, name);
	if (s < 0) return -1;
	float g = 0.92f / pk;
	for (int i = 0; i < n; i++) smp[s].d[i] = (s16)(gbuf[i] * g * 32767.0f);
	padReset(&pads[pad]); padAssign(pad, s); pads[pad].mgroup = mgroup;
	return s;
}

static void gKick(float f0, float f1, float pdec, float adec, float click, float drive) {
	float ph = 0; gStart(adec * 5);
	for (int i = 0; i < gN; i++) {
		float t = (float)i / SR, f = f1 + (f0 - f1) * expf(-t / pdec);
		ph += 2 * PI_F * f / SR;
		float s = sinf(ph) * expf(-t / adec) + frand() * click * expf(-t / 0.0015f);
		gbuf[i] = tanhf(s * drive) / tanhf(drive);
	}
}
static void gSnare(float tone, float tdec, float ndec, float nmix, float bright) {
	SVF hp; memset(&hp, 0, sizeof(hp)); svfSet(&hp, bright, 0.7f);
	float p1 = 0, p2 = 0; gStart(ndec * 5);
	for (int i = 0; i < gN; i++) {
		float t = (float)i / SR;
		p1 += 2 * PI_F * tone * (1 + 0.5f * expf(-t / 0.01f)) / SR; p2 += 2 * PI_F * tone * 1.6f / SR;
		float body = (sinf(p1) + 0.45f * sinf(p2)) * expf(-t / tdec);
		float nz = svf(&hp, frand(), 2) * expf(-t / ndec);
		gbuf[i] = body * (1 - nmix) + nz * nmix * 1.3f;
	}
}
static const float METAL[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
static void gMetal(float dec, float hpf, float scale, float noise, float secs) {
	SVF hp, bp; memset(&hp, 0, sizeof(hp)); memset(&bp, 0, sizeof(bp));
	svfSet(&hp, hpf, 0.9f); svfSet(&bp, hpf * 1.3f, 1.2f);
	float ph[6] = {0}; gStart(secs);
	for (int i = 0; i < gN; i++) {
		float t = (float)i / SR, s = 0;
		for (int k = 0; k < 6; k++) { ph[k] += METAL[k] * scale / SR; if (ph[k] >= 1) ph[k] -= 1; s += ph[k] < 0.5f ? 1 : -1; }
		s = s / 6 + frand() * noise;
		s = svf(&hp, svf(&bp, s, 1) + s * 0.3f, 2);
		gbuf[i] = s * expf(-t / dec) * (1 - expf(-t / 0.0008f));
	}
}
static void gClap(float tail) {
	SVF bp; memset(&bp, 0, sizeof(bp)); svfSet(&bp, 1150, 1.6f); gStart(0.08f + tail * 5);
	for (int i = 0; i < gN; i++) {
		float t = (float)i / SR, e = 0;
		for (int b = 0; b < 3; b++) { float dt = t - b * 0.011f; if (dt >= 0) e += expf(-dt / 0.0035f); }
		float dt = t - 0.033f; if (dt >= 0) e += 0.8f * expf(-dt / tail);
		gbuf[i] = svf(&bp, frand(), 1) * e;
	}
}
static void gRim(void) {
	SVF bp; memset(&bp, 0, sizeof(bp)); svfSet(&bp, 2200, 3); float p1 = 0, p2 = 0; gStart(0.08f);
	for (int i = 0; i < gN; i++) {
		float t = (float)i / SR; p1 += 2 * PI_F * 1700 / SR; p2 += 2 * PI_F * 480 / SR;
		gbuf[i] = sinf(p1) * expf(-t / 0.007f) + 0.6f * sinf(p2) * expf(-t / 0.012f) + svf(&bp, frand(), 1) * expf(-t / 0.004f);
	}
}
static void gTone(float f0, float f1, float pdec, float adec, float noise, int sq) {
	float ph = 0; gStart(adec * 5);
	for (int i = 0; i < gN; i++) {
		float t = (float)i / SR, f = f1 + (f0 - f1) * expf(-t / pdec);
		ph += 2 * PI_F * f / SR;
		float s = sq ? (sinf(ph) > 0 ? 0.6f : -0.6f) : sinf(ph);
		gbuf[i] = s * expf(-t / adec) + frand() * noise * expf(-t / 0.01f);
	}
}
static void gShaker(float a, float d) {
	SVF hp; memset(&hp, 0, sizeof(hp)); svfSet(&hp, 6500, 0.8f); gStart(a + d * 5);
	for (int i = 0; i < gN; i++) {
		float t = (float)i / SR, e = t < a ? t / a : expf(-(t - a) / d);
		gbuf[i] = svf(&hp, frand(), 2) * e;
	}
}
static void gCowbell(void) {
	SVF bp; memset(&bp, 0, sizeof(bp)); svfSet(&bp, 2400, 1.5f); float a = 0, b = 0; gStart(0.5f);
	for (int i = 0; i < gN; i++) {
		float t = (float)i / SR; a += 562.0f / SR; b += 845.0f / SR; if (a >= 1) a -= 1; if (b >= 1) b -= 1;
		float s = (a < 0.5f ? 1 : -1) + (b < 0.5f ? 1 : -1);
		gbuf[i] = svf(&bp, s, 1) * (0.6f * expf(-t / 0.012f) + 0.4f * expf(-t / 0.11f));
	}
}
static void gBass(float f, float dec) {
	float ph = 0; gStart(dec * 4);
	for (int i = 0; i < gN; i++) {
		float t = (float)i / SR, ff = f * (1 + 0.6f * expf(-t / 0.02f));
		ph += 2 * PI_F * ff / SR;
		gbuf[i] = tanhf(1.6f * sinf(ph) * expf(-t / dec)) * (1 - expf(-t / 0.001f));
	}
}
static void gStab(void) {
	static const float NOTES[5] = {130.81f, 155.56f, 196.0f, 233.08f, 293.66f};
	SVF lp; memset(&lp, 0, sizeof(lp)); float ph[10] = {0}; gStart(1.4f);
	for (int i = 0; i < gN; i++) {
		float t = (float)i / SR, s = 0;
		for (int k = 0; k < 10; k++) {
			ph[k] += NOTES[k % 5] * (k < 5 ? 1.003f : 0.997f) / SR; if (ph[k] >= 1) ph[k] -= 1;
			s += ph[k] * 2 - 1;
		}
		if ((i & 15) == 0) svfSet(&lp, 500 + 4200 * expf(-t / 0.12f), 1.1f);
		gbuf[i] = svf(&lp, s * 0.12f, 0) * expf(-t / 0.35f) * (1 - expf(-t / 0.002f));
	}
}

// MPC pad numbering: index 0 = pad 1 (bottom-left)
static void buildKits(void) {
	gbuf = (float*)malloc(SR * 8 * sizeof(float));
	if (!gbuf) return;
	// Bank A: boom bap
	gKick(150, 50, 0.04f, 0.32f, 0.5f, 2.5f);        gFinish(0, "KICK", 0);
	gSnare(190, 0.06f, 0.17f, 0.75f, 1500);          gFinish(1, "SNARE", 0);
	gMetal(0.04f, 7000, 1.0f, 0.3f, 0.3f);           gFinish(2, "HAT CL", 1);
	gMetal(0.33f, 7000, 1.0f, 0.3f, 1.6f);           gFinish(3, "HAT OP", 1);
	gClap(0.12f);                                    gFinish(4, "CLAP", 0);
	gRim();                                          gFinish(5, "RIM", 0);
	gKick(120, 44, 0.05f, 0.55f, 0.25f, 1.6f);       gFinish(6, "KICK 2", 0);
	gSnare(235, 0.04f, 0.11f, 0.85f, 2600);          gFinish(7, "SNARE 2", 0);
	gTone(150, 92, 0.06f, 0.28f, 0.3f, 0);           gFinish(8, "TOM LO", 0);
	gTone(210, 135, 0.06f, 0.24f, 0.3f, 0);          gFinish(9, "TOM MID", 0);
	gTone(290, 190, 0.05f, 0.2f, 0.3f, 0);           gFinish(10, "TOM HI", 0);
	gShaker(0.018f, 0.05f);                          gFinish(11, "SHAKER", 0);
	gMetal(0.6f, 5200, 1.9f, 0.15f, 3.0f);           gFinish(12, "RIDE", 0);
	gMetal(0.9f, 4200, 1.37f, 0.6f, 4.5f);           gFinish(13, "CRASH", 0);
	gBass(65.41f, 0.9f);                             gFinish(14, "808 C", 0);
	gStab();                                         gFinish(15, "STAB Cm9", 0);
	// Bank B: 808-style
	gKick(70, 47, 0.03f, 0.75f, 0.15f, 1.15f);       gFinish(16, "808 KICK", 0);
	gSnare(180, 0.05f, 0.2f, 0.6f, 900);             gFinish(17, "808 SNR", 0);
	gMetal(0.05f, 8000, 1.0f, 0.1f, 0.3f);           gFinish(18, "808 CH", 1);
	gMetal(0.45f, 8000, 1.0f, 0.1f, 2.2f);           gFinish(19, "808 OH", 1);
	gClap(0.18f);                                    gFinish(20, "808 CLAP", 0);
	gRim();                                          gFinish(21, "808 RIM", 0);
	gCowbell();                                      gFinish(22, "COWBELL", 0);
	gTone(2500, 2500, 1, 0.022f, 0.0f, 0);           gFinish(23, "CLAVE", 0);
	gTone(240, 220, 0.03f, 0.17f, 0.05f, 0);         gFinish(24, "CONGA LO", 0);
	gTone(340, 320, 0.03f, 0.15f, 0.05f, 0);         gFinish(25, "CONGA MD", 0);
	gTone(450, 430, 0.03f, 0.13f, 0.05f, 0);         gFinish(26, "CONGA HI", 0);
	gShaker(0.005f, 0.03f);                          gFinish(27, "MARACAS", 0);
	gTone(110, 82, 0.08f, 0.45f, 0.1f, 0);           gFinish(28, "808 TOM L", 0);
	gTone(180, 140, 0.08f, 0.35f, 0.1f, 0);          gFinish(29, "808 TOM H", 0);
	gBass(55.0f, 1.6f);                              gFinish(30, "808 A", 0);
	gMetal(1.1f, 6000, 1.15f, 0.4f, 5.0f);           gFinish(31, "CYMBAL", 0);
	free(gbuf); gbuf = NULL;
}

static void demoSeq(void) {
	// boom bap, 2 bars, pad numbers are 0-based
	static const char* K = "x.........x.x...x.....x...x.....";
	static const char* S = "....x.......x.......x.......x..x";
	static const char* H = "x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.";
	for (int i = 0; i < 32; i++) {
		if (K[i] == 'x') evInsert(0, i * 240, 0, 0, 120, 240, 0);
		if (S[i] == 'x') evInsert(0, i * 240, 1, 0, i == 31 ? 70 : 115, 240, 0);
		if (H[i] == 'x') evInsert(0, i * 240, 2, 0, (i % 4 == 0) ? 105 : 75, 240, 0);
	}
	evInsert(0, 15 * 240, 3, 0, 80, 240, 0);
	evInsert(0, 0, 15, 0, 90, 240, 0);
	evInsert(0, 16 * 240 + 960, 15, -2, 80, 240, 0);
	swing = 58;
}

// ================================================================ WAV I/O
static void wr32(FILE* f, u32 v) { fwrite(&v, 4, 1, f); }
static void wr16(FILE* f, u16 v) { fwrite(&v, 2, 1, f); }
static int wavWrite(const char* path, const s16* d, int n) {
	FILE* f = fopen(path, "wb"); if (!f) return 0;
	fwrite("RIFF", 1, 4, f); wr32(f, 36 + n * 2); fwrite("WAVEfmt ", 1, 8, f);
	wr32(f, 16); wr16(f, 1); wr16(f, 1); wr32(f, SR); wr32(f, SR * 2); wr16(f, 2); wr16(f, 16);
	fwrite("data", 1, 4, f); wr32(f, n * 2);
	int ok = (int)fwrite(d, 2, n, f) == n;
	fclose(f); return ok;
}

// returns sample slot or -1; err gets a short reason
static int wavLoad(const char* path, const char* name, const char** err) {
	*err = "CAN'T OPEN";
	FILE* f = fopen(path, "rb"); if (!f) return -1;
	char id[4]; u32 sz; u8 hdr[12];
	*err = "NOT A WAV";
	if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fclose(f); return -1; }
	int fmt = 0, ch = 0, rate = 0, bits = 0; long dataPos = -1; u32 dataSz = 0;
	while (fread(id, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
		if (!memcmp(id, "fmt ", 4)) {
			u8 b[40]; u32 r = sz < 40 ? sz : 40;
			if (fread(b, 1, r, f) != r) break;
			fmt = b[0] | b[1] << 8; ch = b[2] | b[3] << 8; rate = b[4] | b[5] << 8 | b[6] << 16 | b[7] << 24; bits = b[14] | b[15] << 8;
			if (fmt == 0xFFFE && r >= 26) fmt = b[24] | b[25] << 8;
			fseek(f, sz - r + (sz & 1), SEEK_CUR);
		} else if (!memcmp(id, "data", 4)) { dataPos = ftell(f); dataSz = sz; break; }
		else fseek(f, sz + (sz & 1), SEEK_CUR);
	}
	*err = "UNSUPPORTED WAV";
	if (dataPos < 0 || ch < 1 || ch > 8 || rate < 4000 || !(fmt == 1 || fmt == 3) || !(bits == 8 || bits == 16 || bits == 24 || bits == 32)) { fclose(f); return -1; }
	int bpf = ch * bits / 8;
	int frames = dataSz / bpf;
	if (frames > rate * 60) frames = rate * 60;
	int outN = (int)((double)frames * SR / rate);
	if (outN < 2) { fclose(f); return -1; }
	*err = "OUT OF MEMORY";
	s16* mono = (s16*)malloc(frames * 2);
	if (!mono) { fclose(f); return -1; }
	u8* chunk = (u8*)malloc(4096 * bpf);
	if (!chunk) { free(mono); fclose(f); return -1; }
	fseek(f, dataPos, SEEK_SET);
	int got = 0;
	while (got < frames) {
		int want = frames - got; if (want > 4096) want = 4096;
		int r = fread(chunk, bpf, want, f); if (r <= 0) break;
		for (int i = 0; i < r; i++) {
			float acc = 0; u8* p = chunk + i * bpf;
			for (int c = 0; c < ch; c++) {
				float v = 0;
				if (bits == 8) v = (p[0] - 128) / 128.0f;
				else if (bits == 16) v = (s16)(p[0] | p[1] << 8) / 32768.0f;
				else if (bits == 24) v = (s32)((p[0] << 8) | (p[1] << 16) | (p[2] << 24)) / 2147483648.0f;
				else if (fmt == 3) { float fv; memcpy(&fv, p, 4); v = fv; }
				else v = (s32)(p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24) / 2147483648.0f;
				acc += v; p += bits / 8;
			}
			acc /= ch; if (acc > 1) acc = 1; if (acc < -1) acc = -1;
			mono[got + i] = (s16)(acc * 32767);
		}
		got += r;
	}
	free(chunk); fclose(f);
	frames = got; outN = (int)((double)frames * SR / rate);
	int s = smpAlloc(outN, name);
	if (s < 0) { free(mono); return -1; }
	double ratio = (double)rate / SR;
	for (int i = 0; i < outN; i++) {
		double p = i * ratio; int p0 = (int)p; float fr = (float)(p - p0);
		int p1 = p0 + 1 < frames ? p0 + 1 : p0;
		smp[s].d[i] = (s16)(mono[p0] + (mono[p1] - mono[p0]) * fr);
	}
	free(mono);
	*err = "";
	return s;
}

// ================================================================ projects
#define PROJ_MAGIC 0x31584250   // "PBX1"
static void ensureDirs(void) {
	mkdir("sdmc:/3ds", 0777); mkdir(PB_DIR, 0777); mkdir(PB_DIR "/samples", 0777); mkdir(PB_DIR "/projects", 0777);
}
static int projExists(int slot) {
	char path[96]; snprintf(path, sizeof(path), PB_DIR "/projects/P%02d/project.bin", slot + 1);
	FILE* f = fopen(path, "rb"); if (f) { fclose(f); return 1; } return 0;
}
static int projSave(int slot) {
	ensureDirs();
	char dir[80], path[112];
	snprintf(dir, sizeof(dir), PB_DIR "/projects/P%02d", slot + 1); mkdir(dir, 0777);
	snprintf(path, sizeof(path), "%s/project.bin", dir);
	FILE* f = fopen(path, "wb"); if (!f) return 0;
	u32 hdr[12] = {PROJ_MAGIC, (u32)bpmX10, (u32)swing, (u32)quantSel, (u32)metroSel, (u32)countIn, (u32)vintage, (u32)masterVol, (u32)fullLevel, 0, 0, 0};
	fwrite(hdr, 4, 12, f);
	for (int i = 0; i < MAXSMP; i++) { char nm[16]; memset(nm, 0, 16); if (smp[i].d) memcpy(nm, smp[i].name, 15); u8 used = smp[i].d ? 1 : 0; fwrite(&used, 1, 1, f); fwrite(nm, 1, 16, f); }
	fwrite(pads, sizeof(Pad), NPADS, f);
	fwrite(seqBars, 1, NSEQ, f);
	for (int s = 0; s < NSEQ; s++) { fwrite(&seqN[s], 4, 1, f); fwrite(seqEv[s], sizeof(Event), seqN[s], f); }
	fclose(f);
	for (int i = 0; i < MAXSMP; i++) {
		snprintf(path, sizeof(path), "%s/s%03d.wav", dir, i);
		if (smp[i].d) { if (!wavWrite(path, smp[i].d, smp[i].len)) return 0; }
		else remove(path);
	}
	return 1;
}
static int projLoad(int slot) {
	char dir[80], path[112];
	snprintf(dir, sizeof(dir), PB_DIR "/projects/P%02d", slot + 1);
	snprintf(path, sizeof(path), "%s/project.bin", dir);
	FILE* f = fopen(path, "rb"); if (!f) return 0;
	u32 hdr[12];
	if (fread(hdr, 4, 12, f) != 12 || hdr[0] != PROJ_MAGIC) { fclose(f); return 0; }
	for (int i = 0; i < MAXVOX; i++) vox[i].on = 0;
	for (int i = 0; i < MAXSMP; i++) smpFreeSlot(i);
	bpmX10 = hdr[1]; swing = hdr[2]; quantSel = hdr[3]; metroSel = hdr[4]; countIn = hdr[5]; vintage = hdr[6]; masterVol = hdr[7]; fullLevel = hdr[8];
	u8 used[MAXSMP]; char names[MAXSMP][16];
	for (int i = 0; i < MAXSMP; i++) { fread(&used[i], 1, 1, f); fread(names[i], 1, 16, f); names[i][15] = 0; }
	fread(pads, sizeof(Pad), NPADS, f);
	fread(seqBars, 1, NSEQ, f);
	for (int s = 0; s < NSEQ; s++) {
		int n = 0; fread(&n, 4, 1, f); if (n < 0 || n > MAXEV) n = 0;
		seqN[s] = fread(seqEv[s], sizeof(Event), n, f);
		if (seqBars[s] < 1 || seqBars[s] > 8) seqBars[s] = 1;
	}
	fclose(f);
	// load samples into the same slots
	for (int i = 0; i < MAXSMP; i++) if (used[i]) {
		const char* err; snprintf(path, sizeof(path), "%s/s%03d.wav", dir, i);
		int s = wavLoad(path, names[i], &err);
		if (s >= 0 && s != i) {   // move into slot i
			smp[i] = smp[s]; smp[s].d = NULL; smp[s].len = 0;
		}
	}
	for (int p = 0; p < NPADS; p++) if (pads[p].smp >= 0 && (pads[p].smp >= MAXSMP || !smp[pads[p].smp].d)) pads[p].smp = -1;
	undoN = -1;
	return 1;
}

// remove samples no pad uses
static void smpGC(int keep) {
	for (int i = 0; i < MAXSMP; i++) {
		if (!smp[i].d || i == keep) continue;
		int used = 0;
		for (int p = 0; p < NPADS; p++) if (pads[p].smp == i) { used = 1; break; }
		if (!used) { voicesKillSample(i); smpFreeSlot(i); }
	}
}

// ================================================================ drawing
static u8* fb; static int fbW = 320;
static u8* fontGfx;

static void rect(int x0, int y0, int w, int h, u8 r, u8 g, u8 b) {
	if (x0 < 0) { w += x0; x0 = 0; } if (y0 < 0) { h += y0; y0 = 0; }
	if (x0 + w > fbW) w = fbW - x0; if (y0 + h > 240) h = 240 - y0;
	if (w <= 0 || h <= 0) return;
	for (int x = x0; x < x0 + w; x++) {
		u8* p = fb + (x * 240 + (239 - y0)) * 3;
		for (int y = 0; y < h; y++, p -= 3) { p[0] = b; p[1] = g; p[2] = r; }
	}
}
static void frame(int x, int y, int w, int h, int t, u8 r, u8 g, u8 b) {
	rect(x, y, w, t, r, g, b); rect(x, y + h - t, w, t, r, g, b);
	rect(x, y, t, h, r, g, b); rect(x + w - t, y, t, h, r, g, b);
}
static void drawChar(int x, int y, char ch, int sc, u8 r, u8 g, u8 b) {
	const u8* gl = fontGfx + ((u8)ch) * 8;
	for (int row = 0; row < 8; row++)
		for (int col = 0; col < 8; col++)
			if (gl[row] & (0x80 >> col)) rect(x + col * sc, y + row * sc, sc, sc, r, g, b);
}
static void text(int x, int y, const char* s, int sc, u8 r, u8 g, u8 b) { for (; *s; s++, x += 8 * sc) drawChar(x, y, *s, sc, r, g, b); }
static void textC(int cx, int y, const char* s, int sc, u8 r, u8 g, u8 b) { text(cx - (int)strlen(s) * 4 * sc, y, s, sc, r, g, b); }
static void textR(int rx, int y, const char* s, int sc, u8 r, u8 g, u8 b) { text(rx - (int)strlen(s) * 8 * sc, y, s, sc, r, g, b); }

// LCD palette
#define LCD_BG  168,186,140
#define LCD_FG   28, 38, 26
#define LCD_MID 120,140,100
#define LCD_INV 210,224,180

// ================================================================ UI state
enum { PG_MAIN, PG_PROG, PG_STEP, PG_SMPL, PG_CHOP, PG_DISK, PG_COUNT };
static const char* PG_NAMES[PG_COUNT] = {"MAIN","PROG","STEP","SMPL","CHOP","DISK"};
static int page = PG_MAIN, bank = 0, selPad = 0, levels16 = 0, btnMode = 0;
static int cursor[PG_COUNT];
static int stepBar = 0, stepVel = 100;
static char msg[48] = ""; static int msgT = 0;
static int topDirty = 1, padsDirty = 1, hdrDirty = 1;
static float padFlash[16]; static float padShown[16];

static void say(const char* m) { snprintf(msg, sizeof(msg), "%s", m); msgT = 150; topDirty = 1; }

// ---- sampling (mic)
static u8* micBuf; static u32 micDataSize = 0, micLastOff = 0;
static int micOK = 0, micGain = 2, recThresh = 0, recMaxS = 10, recNorm = 1;
static int smplState = 0;   // 0 idle, 1 armed (waiting for threshold), 2 recording
static s16* rawMic = NULL; static int rawLen = 0, rawMax = 0;
static float micLevel = 0;
static const u8 MIC_GAINS[4] = {0x18, 0x30, 0x50, 0x7F};

// ---- chop
static int chopSrc = 0, chopN = 8, chopAuto = 0, chopSens = 5, chopSel = 0, chopDest = 1, chopBars = 2;
static int chopSt[17];

static void chopCompute(void) {
	Pad* p = &pads[chopSrc];
	if (p->smp < 0) { chopN = chopN < 2 ? 2 : chopN; for (int i = 0; i <= chopN; i++) chopSt[i] = 0; return; }
	int a = p->start, b = p->end, len = b - a;
	if (len < chopN * 64) { for (int i = 0; i <= chopN; i++) chopSt[i] = a + len * i / chopN; return; }
	if (!chopAuto) { for (int i = 0; i <= chopN; i++) chopSt[i] = a + (int)((long long)len * i / chopN); return; }
	// transient detection on 256-sample hops
	const s16* d = smp[p->smp].d;
	int hop = 256, nh = len / hop;
	float* E = (float*)malloc(nh * sizeof(float)); float* D = (float*)malloc(nh * sizeof(float));
	if (!E || !D) { free(E); free(D); for (int i = 0; i <= chopN; i++) chopSt[i] = a + len * i / chopN; return; }
	for (int h = 0; h < nh; h++) { float e = 0; const s16* q = d + a + h * hop; for (int i = 0; i < hop; i++) e += fabsf(q[i]); E[h] = e / hop; }
	float mx = 1;
	for (int h = 0; h < nh; h++) {
		float m = 0; int c = 0; for (int j = h - 4; j < h; j++) if (j >= 0) { m += E[j]; c++; }
		m = c ? m / c : 0; D[h] = E[h] - m * 1.1f; if (D[h] < 0) D[h] = 0; if (D[h] > mx) mx = D[h];
	}
	float th = mx * (0.6f - chopSens * 0.055f);
	int cnt = 1; chopSt[0] = a;
	int minGap = (int)(0.07f * SR / hop);
	// pick strongest peaks greedily
	for (int k = 1; k < chopN; k++) {
		int best = -1; float bv = th;
		for (int h = 2; h < nh; h++) {
			if (D[h] <= bv) continue;
			int ok = 1;
			for (int j = 0; j < cnt; j++) { int hh = (chopSt[j] - a) / hop; if (abs(hh - h) < minGap) { ok = 0; break; } }
			if (ok) { bv = D[h]; best = h; }
		}
		if (best < 0) break;
		int pos = a + best * hop - 96; if (pos < a) pos = a;
		chopSt[cnt++] = pos;
	}
	free(E); free(D);
	// sort
	for (int i = 1; i < cnt; i++) for (int j = i; j > 0 && chopSt[j] < chopSt[j - 1]; j--) { int t = chopSt[j]; chopSt[j] = chopSt[j - 1]; chopSt[j - 1] = t; }
	chopN = cnt < 2 ? 2 : cnt;
	if (cnt < 2) chopSt[1] = a + len / 2;
	chopSt[chopN] = b;
	if (chopSel >= chopN) chopSel = chopN - 1;
}

// ---- disk browser
#define MAXFILES 256
static char cwd[192] = PB_DIR "/samples";
static char fileName[MAXFILES][64]; static u8 fileIsDir[MAXFILES]; static int fileCount = 0, fileSel = 0, fileTop = 0;
static int projSlot = 0, confirmAct = 0;

static int cmpFile(const void* a, const void* b) {
	int ia = *(const int*)a, ib = *(const int*)b;
	if (fileIsDir[ia] != fileIsDir[ib]) return fileIsDir[ib] - fileIsDir[ia];
	return strcasecmp(fileName[ia], fileName[ib]);
}
static void scanDir(void) {
	ensureDirs();
	static char tn[MAXFILES][64]; static u8 td[MAXFILES]; int idx[MAXFILES];
	int n = 0;
	if (strcmp(cwd, "sdmc:/") != 0) { strcpy(tn[n], ".."); td[n] = 1; n++; }
	DIR* dp = opendir(cwd);
	if (dp) {
		struct dirent* e;
		while ((e = readdir(dp)) && n < MAXFILES) {
			if (e->d_name[0] == '.') continue;
			int isDir = e->d_type == DT_DIR;
			int l = strlen(e->d_name);
			if (!isDir && !(l > 4 && !strcasecmp(e->d_name + l - 4, ".wav"))) continue;
			snprintf(tn[n], 64, "%s", e->d_name); td[n] = isDir; n++;
		}
		closedir(dp);
	}
	memcpy(fileName, tn, sizeof(tn)); memcpy(fileIsDir, td, sizeof(td));
	int start = (n && !strcmp(fileName[0], "..")) ? 1 : 0;
	for (int i = 0; i < n; i++) idx[i] = i;
	qsort(idx + start, n - start, sizeof(int), cmpFile);
	for (int i = 0; i < n; i++) { memcpy(tn[i], fileName[idx[i]], 64); td[i] = fileIsDir[idx[i]]; }
	memcpy(fileName, tn, sizeof(tn)); memcpy(fileIsDir, td, sizeof(td));
	fileCount = n; if (fileSel >= n) fileSel = n ? n - 1 : 0; fileTop = 0;
}

// ================================================================ fields
static int fieldCount(int pg) {
	switch (pg) {
	case PG_MAIN: return 11;
	case PG_PROG: return 15;
	case PG_STEP: return 3;
	case PG_SMPL: return 4;
	case PG_CHOP: return 8;
	default: return 1;
	}
}
static void padLabel(int p, char* b) { snprintf(b, 8, "%c%02d", 'A' + p / 16, p % 16 + 1); }

static void fieldText(int pg, int i, char* lab, char* val) {
	Pad* p = &pads[selPad]; char pl[8];
	val[0] = 0; lab[0] = 0;
	switch (pg) {
	case PG_MAIN:
		switch (i) {
		case 0: strcpy(lab, "SEQ"); sprintf(val, "%02d", curSeq + 1); break;
		case 1: strcpy(lab, "BPM"); sprintf(val, "%d.%d", bpmX10 / 10, bpmX10 % 10); break;
		case 2: strcpy(lab, "BARS"); sprintf(val, "%d", seqBars[curSeq]); break;
		case 3: strcpy(lab, "QUANT"); strcpy(val, QUANT_NAMES[quantSel]); break;
		case 4: strcpy(lab, "SWING"); sprintf(val, "%d%%", swing); break;
		case 5: strcpy(lab, "METRO"); strcpy(val, METRO_NAMES[metroSel]); break;
		case 6: strcpy(lab, "COUNT IN"); strcpy(val, countIn ? "ON" : "OFF"); break;
		case 7: strcpy(lab, "VINTAGE"); strcpy(val, VINT_NAMES[vintage]); break;
		case 8: strcpy(lab, "VOLUME"); sprintf(val, "%d", masterVol); break;
		case 9: strcpy(lab, "FULL LVL"); strcpy(val, fullLevel ? "ON" : "OFF"); break;
		case 10: strcpy(lab, "DOUBLE"); strcpy(val, "PRESS >"); break;
		}
		break;
	case PG_PROG:
		switch (i) {
		case 0: strcpy(lab, "PAD"); padLabel(selPad, pl); strcpy(val, pl); break;
		case 1: strcpy(lab, "SAMPLE"); strcpy(val, p->smp >= 0 ? padSmpName(selPad) : "(EMPTY)"); break;
		case 2: strcpy(lab, "LEVEL"); sprintf(val, "%d", p->level); break;
		case 3: strcpy(lab, "PAN"); if (p->pan == 0) strcpy(val, "MID"); else sprintf(val, "%c%d", p->pan < 0 ? 'L' : 'R', abs(p->pan)); break;
		case 4: strcpy(lab, "TUNE"); sprintf(val, "%+d", p->tune); break;
		case 5: strcpy(lab, "FINE"); sprintf(val, "%+dc", p->fine); break;
		case 6: strcpy(lab, "START"); sprintf(val, "%dms", (int)(p->start * 1000LL / SR)); break;
		case 7: strcpy(lab, "END"); sprintf(val, "%dms", (int)(p->end * 1000LL / SR)); break;
		case 8: strcpy(lab, "REVERSE"); strcpy(val, p->reverse ? "ON" : "OFF"); break;
		case 9: strcpy(lab, "ATTACK"); sprintf(val, "%d", p->attack); break;
		case 10: strcpy(lab, "DECAY"); if (p->decay >= 100) strcpy(val, "OFF"); else sprintf(val, "%d", p->decay); break;
		case 11: strcpy(lab, "MODE"); strcpy(val, p->mode ? "NOTE ON" : "ONE SHOT"); break;
		case 12: strcpy(lab, "MUTE GRP"); if (p->mgroup) sprintf(val, "%d", p->mgroup); else strcpy(val, "OFF"); break;
		case 13: strcpy(lab, "CUTOFF"); if (p->cutoff >= 100) strcpy(val, "OPEN"); else sprintf(val, "%d", p->cutoff); break;
		case 14: strcpy(lab, "RESO"); sprintf(val, "%d", p->reso); break;
		}
		break;
	case PG_STEP:
		switch (i) {
		case 0: strcpy(lab, "BAR"); sprintf(val, "%d/%d", stepBar + 1, seqBars[curSeq]); break;
		case 1: strcpy(lab, "PAD"); padLabel(selPad, pl); sprintf(val, "%s %.8s", pl, padSmpName(selPad)); break;
		case 2: strcpy(lab, "VEL"); sprintf(val, "%d", stepVel); break;
		}
		break;
	case PG_SMPL:
		switch (i) {
		case 0: strcpy(lab, "GAIN"); sprintf(val, "%d", micGain + 1); break;
		case 1: strcpy(lab, "THRESH"); if (recThresh) sprintf(val, "%d%%", recThresh * 2); else strcpy(val, "OFF"); break;
		case 2: strcpy(lab, "MAX LEN"); sprintf(val, "%ds", recMaxS); break;
		case 3: strcpy(lab, "NORMALIZE"); strcpy(val, recNorm ? "ON" : "OFF"); break;
		}
		break;
	case PG_CHOP:
		switch (i) {
		case 0: strcpy(lab, "SOURCE"); padLabel(chopSrc, pl); sprintf(val, "%s %.8s", pl, padSmpName(chopSrc)); break;
		case 1: strcpy(lab, "SLICES"); sprintf(val, "%d", chopN); break;
		case 2: strcpy(lab, "MODE"); strcpy(val, chopAuto ? "AUTO" : "EQUAL"); break;
		case 3: strcpy(lab, "SENS"); sprintf(val, "%d", chopSens); break;
		case 4: strcpy(lab, "SLICE"); sprintf(val, "%d", chopSel + 1); break;
		case 5: strcpy(lab, "NUDGE"); sprintf(val, "%dms", (int)((chopSt[chopSel] - pads[chopSrc].start) * 1000LL / SR)); break;
		case 6: strcpy(lab, "TO BANK"); sprintf(val, "%c", 'A' + chopDest); break;
		case 7: strcpy(lab, "LOOP BARS"); sprintf(val, "%d", chopBars); break;
		}
		break;
	}
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void doubleSeq(void) {
	int b = seqBars[curSeq];
	if (b > 4) { say("MAX 8 BARS"); return; }
	undoSnap();
	int n = seqN[curSeq];
	for (int i = 0; i < n; i++) { Event e = seqEv[curSeq][i]; if (e.tick < (u32)(b * TPB)) evInsert(curSeq, e.tick + b * TPB, e.pad, e.trans, e.vel, e.len, 0); }
	seqBars[curSeq] = b * 2; seqResync();
	say("DOUBLED");
}

static void fieldChange(int pg, int i, int d, int big) {
	Pad* p = &pads[selPad];
	switch (pg) {
	case PG_MAIN:
		switch (i) {
		case 0: curSeq = clampi(curSeq + d, 0, NSEQ - 1); if (seqBars[curSeq] < 1) seqBars[curSeq] = 2;
			if (songTick >= seqLen()) { songTick = fmod(songTick, seqLen()); lastTick = (int)songTick; }
			seqResync(); stepBar = 0; break;
		case 1: bpmX10 = clampi(bpmX10 + d * (big ? 100 : 10), 300, 3000); break;
		case 2: seqBars[curSeq] = clampi(seqBars[curSeq] + d, 1, 8);
			if (songTick >= seqLen()) { songTick = 0; lastTick = -1; }
			seqResync(); if (stepBar >= seqBars[curSeq]) stepBar = 0; break;
		case 3: quantSel = clampi(quantSel + d, 0, 6); break;
		case 4: swing = clampi(swing + d * (big ? 4 : 1), 50, 75); break;
		case 5: metroSel = (metroSel + d + 3) % 3; break;
		case 6: countIn ^= 1; break;
		case 7: vintage = (vintage + d + 4) % 4; break;
		case 8: masterVol = clampi(masterVol + d * (big ? 10 : 2), 0, 100); break;
		case 9: fullLevel ^= 1; break;
		case 10: if (d > 0) doubleSeq(); break;
		}
		break;
	case PG_PROG: {
		int len = p->smp >= 0 ? smp[p->smp].len : 0;
		int st = big ? SR / 100 : SR / 1000;
		switch (i) {
		case 0: selPad = clampi(selPad + d, 0, NPADS - 1); bank = selPad / 16; padsDirty = hdrDirty = 1; break;
		case 1: {   // cycle through loaded samples
			int s = p->smp;
			for (int k = 0; k < MAXSMP; k++) { s = (s + d + MAXSMP) % MAXSMP; if (smp[s].d) break; }
			if (s >= 0 && smp[s].d) { padAssign(selPad, s); padsDirty = 1; }
			break; }
		case 2: p->level = clampi(p->level + d * (big ? 10 : 1), 0, 100); break;
		case 3: p->pan = clampi(p->pan + d * (big ? 10 : 1), -50, 50); break;
		case 4: p->tune = clampi(p->tune + d * (big ? 12 : 1), -36, 36); break;
		case 5: p->fine = clampi(p->fine + d * (big ? 10 : 1), -50, 50); break;
		case 6: p->start = clampi(p->start + d * st, 0, p->end - 64 > 0 ? p->end - 64 : 0); break;
		case 7: p->end = clampi(p->end + d * st, p->start + 64, len); break;
		case 8: p->reverse ^= 1; break;
		case 9: p->attack = clampi(p->attack + d * (big ? 10 : 1), 0, 100); break;
		case 10: p->decay = clampi(p->decay + d * (big ? 10 : 1), 0, 100); break;
		case 11: p->mode ^= 1; break;
		case 12: p->mgroup = clampi(p->mgroup + d, 0, 8); break;
		case 13: p->cutoff = clampi(p->cutoff + d * (big ? 10 : 1), 0, 100); break;
		case 14: p->reso = clampi(p->reso + d * (big ? 10 : 1), 0, 100); break;
		}
		break; }
	case PG_STEP:
		switch (i) {
		case 0: stepBar = clampi(stepBar + d, 0, seqBars[curSeq] - 1); break;
		case 1: selPad = clampi(selPad + d, 0, NPADS - 1); bank = selPad / 16; hdrDirty = 1; break;
		case 2: stepVel = clampi(stepVel + d * (big ? 10 : 1), 1, 127); break;
		}
		padsDirty = 1;
		break;
	case PG_SMPL:
		switch (i) {
		case 0: micGain = clampi(micGain + d, 0, 3); break;
		case 1: recThresh = clampi(recThresh + d, 0, 25); break;
		case 2: recMaxS = clampi(recMaxS + d * (big ? 5 : 1), 1, 30); break;
		case 3: recNorm ^= 1; break;
		}
		break;
	case PG_CHOP:
		switch (i) {
		case 0: chopSrc = clampi(chopSrc + d, 0, NPADS - 1); chopSel = 0; chopCompute(); break;
		case 1: chopN = clampi(chopN + d, 2, 16); chopCompute(); break;
		case 2: chopAuto ^= 1; chopCompute(); break;
		case 3: chopSens = clampi(chopSens + d, 1, 10); if (chopAuto) chopCompute(); break;
		case 4: chopSel = clampi(chopSel + d, 0, chopN - 1); break;
		case 5: if (chopSel > 0) { int s = big ? SR / 1000 : SR / 200; chopSt[chopSel] = clampi(chopSt[chopSel] + d * s, chopSt[chopSel - 1] + 64, chopSt[chopSel + 1] - 64); } break;
		case 6: chopDest = clampi(chopDest + d, 0, 3); break;
		case 7: chopBars = clampi(chopBars * (d > 0 ? 2 : 1) / (d < 0 ? 2 : 1), 1, 8); break;
		}
		padsDirty = 1;
		break;
	}
	topDirty = 1;
}

// ================================================================ top screen pages
static void drawWave(int x0, int y0, int w, int h, int pi, int a, int b, int markA, int markB, const int* marks, int nm, int sel) {
	rect(x0, y0, w, h, LCD_BG);
	frame(x0, y0, w, h, 1, LCD_MID);
	int s = pads[pi].smp;
	if (s < 0 || !smp[s].d || b - a < 2) { textC(x0 + w / 2, y0 + h / 2 - 4, "NO SAMPLE", 1, LCD_MID); return; }
	const s16* d = smp[s].d;
	int mid = y0 + h / 2, hh = h / 2 - 3;
	for (int x = 0; x < w - 2; x++) {
		int i0 = a + (int)((long long)(b - a) * x / (w - 2)), i1 = a + (int)((long long)(b - a) * (x + 1) / (w - 2));
		if (i1 <= i0) i1 = i0 + 1;
		int mn = 0, mx = 0, stp = (i1 - i0) > 64 ? (i1 - i0) / 64 : 1;
		for (int i = i0; i < i1; i += stp) { int v = d[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
		int y1 = mid - mx * hh / 32768, y2 = mid - mn * hh / 32768;
		int inside = (markA < 0) || (i0 >= markA && i0 < markB);
		if (inside) rect(x0 + 1 + x, y1, 1, y2 - y1 + 1, LCD_FG); else rect(x0 + 1 + x, y1, 1, y2 - y1 + 1, LCD_MID);
	}
	for (int m = 0; m < nm; m++) {
		int x = x0 + 1 + (int)((long long)(marks[m] - a) * (w - 2) / (b - a));
		char t[4]; snprintf(t, 4, "%d", m + 1);
		if (m == sel) { rect(x, y0 + 1, 2, h - 2, 200, 40, 30); text(x + 3, y0 + 3, t, 1, 200, 40, 30); }
		else { rect(x, y0 + 1, 1, h - 2, LCD_FG); text(x + 3, y0 + 3, t, 1, LCD_FG); }
	}
}

static void topHeader(void) {
	rect(0, 0, 400, 20, 40, 44, 40);
	text(6, 6, "PADBOX", 1, 230, 160, 60);
	text(64, 6, PG_NAMES[page], 1, 210, 224, 180);
	int L = seqLen();
	int t = playState == 2 ? (int)songTick : 0;
	if (t >= L) t = 0;
	char b[64];
	snprintf(b, sizeof(b), "SEQ%02d %03d.%d.%02d %d.%d", curSeq + 1, t / TPB + 1, (t % TPB) / PPQ + 1, (t % PPQ) * 100 / PPQ, bpmX10 / 10, bpmX10 % 10);
	text(110, 6, b, 1, 210, 224, 180);
	const char* st = playState == 1 ? "COUNT" : (playState == 2 ? "PLAY" : "STOP");
	rect(318, 3, 40, 14, playState ? 40 : 60, playState ? 140 : 60, playState ? 60 : 60);
	textC(338, 6, st, 1, 255, 255, 255);
	rect(360, 3, 36, 14, recOn ? 210 : 70, recOn ? 40 : 60, recOn ? 30 : 60);
	textC(378, 6, "REC", 1, 255, 255, 255);
}

static void topFooter(void) {
	rect(0, 226, 400, 14, 40, 44, 40);
	const char* h = "";
	if (msgT > 0) { text(6, 229, msg, 1, 255, 200, 90); return; }
	switch (page) {
	case PG_MAIN: h = "A:REC X:UNDO B+PAD:ERASE R+PAD:REPEAT"; break;
	case PG_PROG: h = "TAP PADS TO SELECT  L+D-PAD: BIG STEPS"; break;
	case PG_STEP: h = "PADS = 16 STEPS OF THE BAR  L+PAD: PICK"; break;
	case PG_SMPL: h = micOK ? "A: RECORD / STOP  -> SELECTED PAD" : "MIC UNAVAILABLE"; break;
	case PG_CHOP: h = "A: SLICES->PADS  X: BPM FROM LOOP"; break;
	case PG_DISK: h = "A:LOAD B:SAVE PAD X:SAVE PRJ Y:LOAD PRJ"; break;
	}
	text(6, 229, h, 1, 170, 180, 150);
}

static void drawFields(int y0, int cols) {
	int n = fieldCount(page), rows = (n + cols - 1) / cols;
	int cw = 400 / cols;
	for (int i = 0; i < n; i++) {
		int c = i / rows, r = i % rows;
		int x = 4 + c * cw, y = y0 + r * 12;
		char lab[24], val[32]; fieldText(page, i, lab, val);
		int sel = cursor[page] == i;
		if (sel) rect(x - 2, y - 2, cw - 6, 12, LCD_FG);
		if (sel) { text(x, y, lab, 1, LCD_INV); textR(x + cw - 12, y, val, 1, LCD_INV); }
		else { text(x, y, lab, 1, LCD_MID); textR(x + cw - 12, y, val, 1, LCD_FG); }
	}
}

// sequence overview (MAIN)
#define OV_X 8
#define OV_Y 152
#define OV_W 384
#define OV_H 70
static int ovPlayX = -1;
static void drawOverview(void) {
	rect(OV_X, OV_Y, OV_W, OV_H, LCD_BG);
	int L = seqLen();
	for (int b = 0; b <= seqBars[curSeq] * 4; b++) {
		int x = OV_X + b * (OV_W - 1) / (seqBars[curSeq] * 4);
		rect(x, OV_Y, 1, OV_H, (b % 4 == 0) ? 28 : 120, (b % 4 == 0) ? 38 : 140, (b % 4 == 0) ? 26 : 100);
	}
	// rows: pads of the current bank (16 rows x 4px), others marked in a 6px strip at the bottom
	for (int i = 0; i < seqN[curSeq]; i++) {
		Event* e = &seqEv[curSeq][i];
		if (e->tick >= (u32)L) continue;
		int x = OV_X + (int)((long long)e->tick * (OV_W - 1) / L);
		int y;
		if (e->pad / 16 == bank) y = OV_Y + 1 + (15 - e->pad % 16) * 4; else y = OV_Y + 65;
		int w = 2 + e->vel / 64;
		rect(x, y, w, 3, e->pad == selPad ? 200 : 28, e->pad == selPad ? 40 : 38, e->pad == selPad ? 30 : 26);
	}
	ovPlayX = -1;
}

static void drawStepGrid(void) {
	int gx = 40, gy = 44, cw = 22, ch = 11;
	rect(0, gy - 2, 400, 16 * ch + 4, LCD_BG);
	int t0 = stepBar * TPB;
	int ph = (playState == 2) ? (((int)songTick - t0) / 240) : -1;
	for (int r = 0; r < 16; r++) {
		int pi = bank * 16 + 15 - r;
		char pl[8]; padLabel(pi, pl);
		int sel = pi == selPad;
		if (sel) rect(0, gy + r * ch, 38, ch - 1, LCD_FG);
		text(4, gy + r * ch + 1, pl, 1, sel ? 210 : 28, sel ? 224 : 38, sel ? 180 : 26);
		for (int s = 0; s < 16; s++) {
			int x = gx + s * cw, y = gy + r * ch;
			int on = 0, vel = 0;
			int lo = lowerBound(curSeq, t0 + s * 240);
			for (int k = lo; k < seqN[curSeq] && seqEv[curSeq][k].tick < (u32)(t0 + s * 240 + 240); k++)
				if (seqEv[curSeq][k].pad == pi) { on = 1; vel = seqEv[curSeq][k].vel; }
			u8 cr = 150, cg = 168, cb = 124;
			if (s % 4 == 0) { cr = 140; cg = 158; cb = 114; }
			if (s == ph) { cr = 200; cg = 210; cb = 160; }
			rect(x, y, cw - 2, ch - 2, cr, cg, cb);
			if (on) {
				int w = 4 + (cw - 8) * vel / 127;
				rect(x + 2, y + 2, w, ch - 6, sel ? 200 : 28, sel ? 40 : 38, sel ? 30 : 26);
			}
		}
	}
}

static int lastMeterW = -1;
static void drawSmplLive(void) {
	int w = (int)(micLevel * 380); if (w > 380) w = 380;
	if (w == lastMeterW) return;
	lastMeterW = w;
	rect(10, 104, 380, 14, LCD_MID);
	rect(10, 104, w, 14, micLevel > 0.95f ? 200 : 28, micLevel > 0.95f ? 40 : 38, micLevel > 0.95f ? 30 : 26);
	if (recThresh) { int tx = 10 + recThresh * 2 * 380 / 100; rect(tx, 100, 2, 22, 200, 40, 30); }
}

static void drawTop(void) {
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	rect(0, 20, 400, 206, LCD_BG);
	topHeader();
	switch (page) {
	case PG_MAIN:
		drawFields(28, 2);
		drawOverview();
		break;
	case PG_PROG: {
		drawFields(26, 2);
		Pad* p = &pads[selPad];
		int len = p->smp >= 0 ? smp[p->smp].len : 0;
		drawWave(8, 124, 384, 98, selPad, 0, len, p->start, p->end, NULL, 0, -1);
		if (len) {
			int xs = 9 + (int)((long long)p->start * 382 / len), xe = 9 + (int)((long long)p->end * 382 / len);
			rect(xs, 124, 2, 98, 200, 40, 30); rect(xe - 2, 124, 2, 98, 200, 40, 30);
		}
		break; }
	case PG_STEP:
		drawFields(26, 3);
		drawStepGrid();
		break;
	case PG_SMPL: {
		drawFields(26, 2);
		char b[64];
		if (smplState == 1) textC(200, 80, "ARMED - WAITING FOR SOUND", 1, LCD_FG);
		else if (smplState == 2) { snprintf(b, sizeof(b), "RECORDING %.1fs", rawLen / MIC_RATE); textC(200, 76, b, 2, 200, 40, 30); }
		else { char pl[8]; padLabel(selPad, pl); snprintf(b, sizeof(b), "A TO SAMPLE INTO PAD %s", pl); textC(200, 80, b, 1, LCD_FG); }
		lastMeterW = -1; drawSmplLive();
		Pad* p = &pads[selPad];
		drawWave(8, 126, 384, 96, selPad, p->start, p->end, -1, 0, NULL, 0, -1);
		break; }
	case PG_CHOP: {
		drawFields(26, 2);
		Pad* p = &pads[chopSrc];
		drawWave(8, 82, 384, 140, chopSrc, p->start, p->end > p->start ? p->end : p->start + 1, -1, 0, chopSt, chopN, chopSel);
		break; }
	case PG_DISK: {
		char b[64];
		snprintf(b, sizeof(b), "PROJECT P%02d %s", projSlot + 1, projExists(projSlot) ? "(SAVED)" : "(EMPTY)");
		rect(2, 24, 396, 12, LCD_FG); text(6, 26, b, 1, LCD_INV); textR(394, 26, "< >", 1, LCD_INV);
		snprintf(b, sizeof(b), "%.48s", cwd + 5);
		text(6, 40, b, 1, LCD_MID);
		if (fileSel < fileTop) fileTop = fileSel;
		if (fileSel >= fileTop + 14) fileTop = fileSel - 13;
		for (int i = 0; i < 14 && fileTop + i < fileCount; i++) {
			int k = fileTop + i, y = 54 + i * 12;
			if (k == fileSel) rect(2, y - 2, 396, 12, LCD_FG);
			snprintf(b, sizeof(b), "%s%.46s", fileIsDir[k] ? "/" : " ", fileName[k]);
			text(6, y, b, 1, k == fileSel ? 210 : 28, k == fileSel ? 224 : 38, k == fileSel ? 180 : 26);
		}
		if (!fileCount) text(6, 54, "NO WAV FILES HERE", 1, LCD_MID);
		snprintf(b, sizeof(b), "MEM %d.%dMB/%dMB", poolBytes / 1048576, (poolBytes % 1048576) * 10 / 1048576, POOL_LIMIT / 1048576);
		textR(394, 40, b, 1, LCD_MID);
		break; }
	}
	topFooter();
}

static int lastHdrT = -1, lastStepPh = -2;
static void drawTopLive(void) {
	fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); fbW = 400;
	int t = playState == 2 ? (int)songTick : 0;
	int ht = t / 24;
	if (ht != lastHdrT) { topHeader(); lastHdrT = ht; }
	if (page == PG_MAIN) {
		int L = seqLen();
		int x = playState == 2 ? OV_X + (int)((long long)(t % L) * (OV_W - 1) / L) : -1;
		if (x != ovPlayX) {
			if (ovPlayX >= 0 || x < 0) drawOverview();
			if (x >= 0) rect(x, OV_Y, 1, OV_H, 200, 40, 30);
			ovPlayX = x;
		}
	} else if (page == PG_STEP) {
		int ph = playState == 2 ? ((t - stepBar * TPB) / 240) : -1;
		if (ph != lastStepPh) { drawStepGrid(); lastStepPh = ph; padsDirty = 1; }
	} else if (page == PG_SMPL) drawSmplLive();
	if (msgT > 0 && --msgT == 0) topFooter();
}

// ================================================================ bottom screen
#define HDR_H 20
static void padRect(int k, int* x, int* y) {   // k = 0..15 in MPC order (0 bottom-left)
	int col = k % 4, row = 3 - k / 4;
	*x = col * 80; *y = HDR_H + row * 55;
}
static void stepRect(int s, int* x, int* y) {  // reading order
	*x = (s % 4) * 80; *y = HDR_H + (s / 4) * 55;
}

static void drawHeader(void) {
	fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320;
	rect(0, 0, 320, HDR_H, 26, 26, 28);
	for (int i = 0; i < PG_COUNT; i++) {
		int x = i * 40, a = i == page;
		rect(x + 1, 1, 38, HDR_H - 2, a ? 230 : 48, a ? 150 : 48, a ? 50 : 52);
		textC(x + 20, 6, PG_NAMES[i], 1, a ? 20 : 200, a ? 20 : 200, a ? 20 : 200);
	}
	char b[4] = {(char)('A' + bank), 0};
	rect(241, 1, 22, HDR_H - 2, 70, 110, 160); textC(252, 6, b, 1, 255, 255, 255);
	rect(265, 1, 26, HDR_H - 2, btnMode ? 60 : 48, btnMode ? 160 : 48, btnMode ? 90 : 52); textC(278, 6, "BTN", 1, 230, 230, 230);
	rect(293, 1, 26, HDR_H - 2, levels16 ? 170 : 48, levels16 ? 70 : 48, levels16 ? 170 : 52); textC(306, 6, "16L", 1, 230, 230, 230);
}

static void drawPad(int k) {
	fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); fbW = 320;
	int x, y;
	if (page == PG_STEP) {
		stepRect(k, &x, &y);
		int t0 = stepBar * TPB + k * 240, on = 0, vel = 0;
		int lo = lowerBound(curSeq, t0);
		for (int i = lo; i < seqN[curSeq] && seqEv[curSeq][i].tick < (u32)(t0 + 240); i++)
			if (seqEv[curSeq][i].pad == selPad) { on = 1; vel = seqEv[curSeq][i].vel; }
		int ph = playState == 2 && ((int)songTick / 240) == stepBar * 16 + k;
		u8 r = 58, g = 58, b = 62;
		if (k % 4 == 0) { r = 70; g = 70; b = 76; }
		if (on) { r = 220; g = 120; b = 40; }
		if (ph) { r = on ? 255 : 120; g = on ? 200 : 120; b = on ? 120 : 130; }
		rect(x + 2, y + 2, 76, 51, r, g, b);
		char t[8]; snprintf(t, 8, "%d", k + 1);
		text(x + 6, y + 6, t, 1, on ? 30 : 180, on ? 20 : 180, on ? 10 : 180);
		if (on) { rect(x + 6, y + 44, 68 * vel / 127, 4, 60, 30, 10); }
		return;
	}
	padRect(k, &x, &y);
	int pi = bank * 16 + k;
	float f = padShown[k];
	u8 r = 72 + (u8)(f * 170), g = 72 + (u8)(f * 40), b = 76 - (u8)(f * 40);
	rect(x + 2, y + 2, 76, 51, r, g, b);
	int sel = (page == PG_CHOP) ? 0 : (pi == selPad && !levels16);
	if (levels16 && k == 0) sel = 1;
	if (sel) frame(x + 2, y + 2, 76, 51, 2, 255, 190, 60);
	char t[24];
	if (page == PG_CHOP) {
		if (k < chopN) { snprintf(t, sizeof(t), "SLICE %d", k + 1); text(x + 6, y + 6, t, 1, 230, 230, 230); }
		if (k == chopSel) frame(x + 2, y + 2, 76, 51, 2, 255, 90, 60);
		return;
	}
	if (levels16) {
		snprintf(t, sizeof(t), "%+d", pads[selPad].tune + k); text(x + 6, y + 6, t, 1, 230, 230, 230);
		text(x + 6, y + 40, "16 LVL", 1, 200, 150, 220);
		return;
	}
	padLabel(pi, t); text(x + 6, y + 6, t, 1, 150, 150, 155);
	if (btnMode && k < 9) {
		static const char* BTN[9] = {"<", "v", ">", "^", "Y", "B", "A", "X", "L"};
		textR(x + 74, y + 6, BTN[k], 1, 120, 220, 140);
	}
	char nm[16]; snprintf(nm, sizeof(nm), "%.9s", padSmpName(pi));
	text(x + 6, y + 36, nm, 1, 235, 235, 235);
}

static void drawPads(void) { for (int k = 0; k < 16; k++) drawPad(k); }

// ================================================================ actions
static int lockHeld = 0;
static void lock(void) { LightLock_Lock(&alock); lockHeld = 1; }
static void unlock(void) { LightLock_Unlock(&alock); lockHeld = 0; }

static void transportToggle(void) {
	lock();
	if (playState) {
		playState = 0;
		for (int i = 0; i < MAXVOX; i++) if (vox[i].on && vox[i].noteOn) vox[i].stage = 2;
		npend = 0;
	} else {
		songTick = 0; lastTick = -1; evIdx = 0; npend = 0;
		for (int i = 0; i < seqN[curSeq]; i++) seqEv[curSeq][i].flags = 0;
		if (recOn && countIn) { playState = 1; ciTick = 0; clickMetro(1); }
		else playState = 2;
		recPassArmed = 1;
	}
	unlock();
	topDirty = hdrDirty = 1; padsDirty = 1;
}

static void eraseCurrentPad(int pi) {
	lock();
	undoSnap();
	int n = 0;
	for (int i = seqN[curSeq] - 1; i >= 0; i--) if (seqEv[curSeq][i].pad == pi) { evDelete(curSeq, i); n++; }
	unlock();
	char b[32], pl[8]; padLabel(pi, pl); snprintf(b, sizeof(b), "ERASED %s (%d)", pl, n); say(b);
}

// physical pad k (0-15) pressed with velocity vel. slot = hold slot index
static void padDown(int k, int vel, int slot, u32 kHeld) {
	int pi = bank * 16 + k, trans = 0;
	if (page == PG_STEP) {
		if (kHeld & KEY_L) { selPad = bank * 16 + (3 - k / 4) * 4 + k % 4; topDirty = padsDirty = 1; return; }
		// toggle step k of current bar for selPad (k here is reading order)
		int t0 = stepBar * TPB + k * 240, found = 0;
		lock();
		undoSnap();
		for (int i = seqN[curSeq] - 1; i >= 0; i--) {
			Event* e = &seqEv[curSeq][i];
			if (e->pad == selPad && e->tick >= (u32)t0 && e->tick < (u32)(t0 + 240)) { evDelete(curSeq, i); found = 1; }
		}
		if (!found) { evInsert(curSeq, t0, selPad, 0, stepVel, 240, 0); voiceStart(selPad, 0, stepVel, ticksToSamples(240), 0); }
		unlock();
		topDirty = padsDirty = 1;
		return;
	}
	if (page == PG_CHOP) {
		if (k < chopN && pads[chopSrc].smp >= 0) {
			chopSel = k;
			Pad save = pads[chopSrc];
			lock();
			pads[chopSrc].start = chopSt[k]; pads[chopSrc].end = chopSt[k + 1]; pads[chopSrc].reverse = 0;
			voiceStart(chopSrc, 0, 127, 0, 0);
			pads[chopSrc] = save;
			unlock();
			padFlash[k] = 1; topDirty = 1; padsDirty = 1;
		}
		return;
	}
	if (kHeld & KEY_B) { eraseCurrentPad(levels16 ? selPad : pi); return; }
	if (levels16) { trans = k; pi = selPad; }
	else if (kHeld & KEY_L) { selPad = pi; topDirty = padsDirty = 1; if (page == PG_CHOP) { chopSrc = pi; chopCompute(); } return; }
	else if (selPad != pi) { selPad = pi; padsDirty = 1; if (page == PG_PROG || page == PG_SMPL || page == PG_STEP) topDirty = 1; }
	Hold* h = &hold[slot];
	lock();
	h->on = 1; h->pad = pi; h->trans = trans; h->vel = vel; h->evTick = -1;
	h->lastRep = repeatOn ? (playState == 2 ? lastTick : (lastFree & ((1 << 20) - 1))) : -100000;
	voiceStart(pi, trans, vel, -1, slot + 1);
	recordHit(pi, trans, vel, 240, h);
	unlock();
	padFlash[k] = vel / 127.0f;
	if (page == PG_MAIN) topDirty = 1;
}

static void padUp(int slot) {
	Hold* h = &hold[slot];
	if (!h->on) return;
	lock();
	voiceReleaseHold(slot + 1);
	if (h->evTick >= 0 && playState == 2) {
		int L = seqLen();
		int len = (((int)songTick - h->pressTick) % L + L) % L;
		if (len < 30) len = 30; if (len > 65000) len = 65000;
		for (int i = lowerBound(curSeq, h->evTick); i < seqN[curSeq] && seqEv[curSeq][i].tick == (u32)h->evTick; i++)
			if (seqEv[curSeq][i].pad == h->pad && seqEv[curSeq][i].trans == h->trans) seqEv[curSeq][i].len = len;
	}
	h->on = 0;
	unlock();
}

static void chopConvert(void) {
	Pad* src = &pads[chopSrc];
	if (src->smp < 0) { say("NO SAMPLE TO CHOP"); return; }
	Pad base = *src;
	lock();
	for (int i = 0; i < chopN && i < 16; i++) {
		Pad* p = &pads[chopDest * 16 + i];
		*p = base;
		p->start = chopSt[i]; p->end = chopSt[i + 1]; p->reverse = 0; p->mgroup = 0;
	}
	smpGC(-1);
	unlock();
	bank = chopDest;
	char b[40]; snprintf(b, sizeof(b), "%d SLICES -> BANK %c", chopN, 'A' + chopDest); say(b);
	hdrDirty = padsDirty = 1;
}

static void chopBpm(void) {
	Pad* p = &pads[chopSrc];
	if (p->smp < 0) return;
	float secs = (float)(p->end - p->start) / SR;
	if (secs < 0.2f) return;
	float b = chopBars * 4 * 60.0f / secs;
	while (b > 200) b /= 2;
	while (b < 50) b *= 2;
	bpmX10 = (int)(b * 10 + 0.5f);
	char m[40]; snprintf(m, sizeof(m), "BPM %d.%d (%d BARS)", bpmX10 / 10, bpmX10 % 10, chopBars); say(m);
}

// ---- mic
static void micPoll(void) {
	if (!smplState) return;
	u32 off = micGetLastSampleOffset();
	float pk = 0;
	while (micLastOff != off) {
		s16 s = *(s16*)(micBuf + micLastOff);
		micLastOff += 2; if (micLastOff >= micDataSize) micLastOff = 0;
		float a = fabsf(s / 32768.0f); if (a > pk) pk = a;
		if (smplState == 1) {
			if (!recThresh || a >= recThresh * 0.02f) { smplState = 2; rawLen = 0; topDirty = 1; }
			else continue;
		}
		if (rawLen < rawMax) rawMic[rawLen++] = s;
	}
	micLevel = pk > micLevel ? pk : micLevel * 0.85f;
	static int lastSec = -1;
	if (smplState == 2) { int s10 = (int)(rawLen / MIC_RATE * 10); if (s10 != lastSec) { lastSec = s10; topDirty = 1; } }
}
static void smplStart(void) {
	if (!micOK) { say("MIC UNAVAILABLE"); return; }
	rawMax = (int)(recMaxS * MIC_RATE);
	free(rawMic); rawMic = (s16*)malloc(rawMax * 2);
	if (!rawMic) { say("OUT OF MEMORY"); return; }
	MICU_SetGain(MIC_GAINS[micGain]);
	if (R_FAILED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_32730, 0, micDataSize, true))) { say("MIC ERROR"); return; }
	micLastOff = micGetLastSampleOffset();
	rawLen = 0; smplState = recThresh ? 1 : 2; micLevel = 0;
	topDirty = 1;
}
static void smplStop(void) {
	if (!smplState) return;
	MICU_StopSampling();
	int st = smplState; smplState = 0; micLevel = 0; topDirty = 1;
	int skip = (st == 2 && !recThresh) ? 1200 : 0;   // drop the start-up click
	if (rawLen < skip + 800) { say("TOO SHORT"); free(rawMic); rawMic = NULL; return; }
	float ratio = MIC_RATE / SR;
	int n = (int)((rawLen - skip - 1) / ratio);
	static int take = 0; char nm[16]; snprintf(nm, sizeof(nm), "MIC %02d", ++take);
	lock();
	int s = smpAlloc(n, nm);
	if (s < 0) { unlock(); say("OUT OF MEMORY"); free(rawMic); rawMic = NULL; return; }
	float pk = 1;
	for (int i = 0; i < n; i++) {
		float p = skip + i * ratio; int p0 = (int)p; float fr = p - p0;
		float v = rawMic[p0] + (rawMic[p0 + 1] - rawMic[p0]) * fr;
		smp[s].d[i] = (s16)v; if (fabsf(v) > pk) pk = fabsf(v);
	}
	if (recNorm && pk > 64) { float g = 32000.0f / pk; for (int i = 0; i < n; i++) smp[s].d[i] = (s16)(smp[s].d[i] * g); }
	padReset(&pads[selPad]); padAssign(selPad, s);
	smpGC(-1);
	unlock();
	free(rawMic); rawMic = NULL;
	char b[40], pl[8]; padLabel(selPad, pl); snprintf(b, sizeof(b), "SAMPLED %.1fs -> %s", n / (float)SR, pl); say(b);
	padsDirty = 1;
}

// ---- disk
static void diskLoadFile(void) {
	if (!fileCount) return;
	char path[288];
	if (fileIsDir[fileSel]) {
		if (!strcmp(fileName[fileSel], "..")) {
			char* sl = strrchr(cwd, '/');
			if (sl && sl > cwd + 5) *sl = 0; else strcpy(cwd, "sdmc:/");
		} else {
			snprintf(path, sizeof(path), "%s%s%s", cwd, cwd[strlen(cwd) - 1] == '/' ? "" : "/", fileName[fileSel]);
			snprintf(cwd, sizeof(cwd), "%s", path);
		}
		fileSel = 0; scanDir(); topDirty = 1; return;
	}
	snprintf(path, sizeof(path), "%s/%s", cwd, fileName[fileSel]);
	char nm[16]; snprintf(nm, sizeof(nm), "%s", fileName[fileSel]);
	char* dot = strrchr(nm, '.'); if (dot) *dot = 0;
	const char* err;
	say("LOADING...");
	int s = wavLoad(path, nm, &err);
	if (s >= 0) { lock(); padReset(&pads[selPad]); padAssign(selPad, s); smpGC(-1); unlock(); }
	if (s < 0) say(err);
	else { char b[48], pl[8]; padLabel(selPad, pl); snprintf(b, sizeof(b), "%s -> %s", nm, pl); say(b); padsDirty = 1; }
}
static void diskSavePad(void) {
	int s = pads[selPad].smp;
	if (s < 0) { say("PAD IS EMPTY"); return; }
	ensureDirs();
	Pad* p = &pads[selPad];
	char path[96]; int n = 0; FILE* f;
	do { n++; snprintf(path, sizeof(path), PB_DIR "/samples/%.10s_%03d.wav", smp[s].name, n); for (char* c = path + strlen(PB_DIR) + 9; *c; c++) if (*c == ' ' || *c == '/') *c = '_'; f = fopen(path, "rb"); if (f) fclose(f); } while (f && n < 999);
	int a = p->start, b = p->end > smp[s].len ? smp[s].len : p->end;
	if (wavWrite(path, smp[s].d + a, b - a)) { say(strrchr(path, '/') + 1); if (!strcmp(cwd, PB_DIR "/samples")) scanDir(); }
	else say("SAVE FAILED");
}

// ================================================================ main
int main(int argc, char** argv) {
	gfxInitDefault();
	gfxSetDoubleBuffering(GFX_TOP, false);
	gfxSetDoubleBuffering(GFX_BOTTOM, false);
	fontGfx = consoleGetDefault()->font.gfx;
	osSetSpeedupEnable(true);
	hidSetRepeatParameters(18, 4);
	LightLock_Init(&alock);

	for (int i = 0; i < NPADS; i++) padReset(&pads[i]);
	for (int s = 0; s < NSEQ; s++) seqBars[s] = 2;
	buildKits();
	demoSeq();
	ensureDirs();
	scanDir();

	micBuf = (u8*)memalign(0x1000, 0x30000);
	if (micBuf && R_SUCCEEDED(micInit(micBuf, 0x30000))) { micOK = 1; micDataSize = micGetSampleDataSize(); }

	int audioOK = R_SUCCEEDED(ndspInit());
	Thread athr = NULL;
	if (audioOK) {
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		ndspChnReset(0);
		ndspChnSetInterp(0, NDSP_INTERP_NONE);
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
		LightEvent_Init(&audioEv, RESET_ONESHOT);
		ndspSetCallback(ndspCb, NULL);
		s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		athr = threadCreate(audioThread, NULL, 32 * 1024, prio - 1, -2, false);
	}
	if (!audioOK) say("NO AUDIO: RUN DSP1 ONCE (dspfirm.cdc)");

	int touchK = -1, touchSlot = 15;
	int tapCount = 0; u64 tapLast = 0; double tapAcc = 0;

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown(), kHeld = hidKeysHeld(), kUp = hidKeysUp(), kRep = hidKeysDownRepeat();
		if ((kHeld & KEY_SELECT) && (kDown & KEY_START) && !btnMode) break;
		if ((kDown & KEY_SELECT) && (kHeld & KEY_START) && !btnMode) break;

		circlePosition cp; hidCircleRead(&cp);
		circleY = cp.dy / 150.0f;
		repeatOn = !!(kHeld & KEY_R);

		micPoll();

		// ---------- buttons
		if (kDown & KEY_START) transportToggle();
		if (btnMode) {
			static const u32 MAP[9] = {KEY_DLEFT, KEY_DDOWN, KEY_DRIGHT, KEY_DUP, KEY_Y, KEY_B, KEY_A, KEY_X, KEY_L};
			for (int k = 0; k < 9; k++) {
				if (kDown & MAP[k]) padDown(k, 127, k, 0);
				if (kUp & MAP[k]) padUp(k);
			}
			if (kDown & KEY_SELECT) { recOn ^= 1; recPassArmed = 1; hdrDirty = topDirty = 1; }
		} else {
			if (kDown & KEY_SELECT) { page = (page + 1) % PG_COUNT; topDirty = hdrDirty = padsDirty = 1; if (page == PG_CHOP) { chopSrc = selPad; chopCompute(); } if (page == PG_DISK) scanDir(); }
			int big = !!(kHeld & KEY_L);
			if (page == PG_DISK) {
				if (kRep & KEY_DUP) { if (fileSel > 0) fileSel--; topDirty = 1; }
				if (kRep & KEY_DDOWN) { if (fileSel < fileCount - 1) fileSel++; topDirty = 1; }
				if (kRep & KEY_DLEFT) { projSlot = (projSlot + 15) % 16; confirmAct = 0; topDirty = 1; }
				if (kRep & KEY_DRIGHT) { projSlot = (projSlot + 1) % 16; confirmAct = 0; topDirty = 1; }
				if (kDown & KEY_A) diskLoadFile();
				if (kDown & KEY_B) diskSavePad();
				if (kDown & KEY_X) {
					if (projExists(projSlot) && confirmAct != 1) { confirmAct = 1; say("OVERWRITE? PRESS X AGAIN"); }
					else { confirmAct = 0; say("SAVING..."); drawTop(); gfxFlushBuffers(); int ok = projSave(projSlot); say(ok ? "PROJECT SAVED" : "SAVE FAILED"); }
				}
				if (kDown & KEY_Y) {
					if (!projExists(projSlot)) say("NOTHING SAVED IN THIS SLOT");
					else if (confirmAct != 2) { confirmAct = 2; say("LOAD? UNSAVED WORK IS LOST. Y AGAIN"); }
					else {
						confirmAct = 0; say("LOADING..."); drawTop(); gfxFlushBuffers();
						lock(); playState = 0; int ok = projLoad(projSlot); seqResync(); unlock();
						say(ok ? "PROJECT LOADED" : "LOAD FAILED"); padsDirty = hdrDirty = 1;
					}
				}
			} else if (big && (kRep & (KEY_DUP | KEY_DDOWN))) {
				bank = (bank + ((kRep & KEY_DUP) ? 1 : 3)) % 4; hdrDirty = padsDirty = topDirty = 1;
			} else {
				int n = fieldCount(page);
				if (kRep & KEY_DUP) { cursor[page] = (cursor[page] + n - 1) % n; topDirty = 1; }
				if (kRep & KEY_DDOWN) { cursor[page] = (cursor[page] + 1) % n; topDirty = 1; }
				if (kRep & KEY_DLEFT) { lock(); fieldChange(page, cursor[page], -1, big); unlock(); }
				if (kRep & KEY_DRIGHT) { lock(); fieldChange(page, cursor[page], 1, big); unlock(); }

				if (page == PG_SMPL && (kDown & KEY_A)) { if (smplState) smplStop(); else smplStart(); }
				else if (page == PG_CHOP && (kDown & KEY_A)) chopConvert();
				else if (kDown & KEY_A) {
					if (big) { metroSel = metroSel ? 0 : 1; say(metroSel ? "METRONOME ON" : "METRONOME OFF"); }
					else { recOn ^= 1; recPassArmed = 1; hdrDirty = 1; }
					topDirty = 1;
				}
				if (page == PG_CHOP && (kDown & KEY_X)) chopBpm();
				else if (kDown & KEY_X) {
					if (big) {   // tap tempo
						u64 now = osGetTime();
						if (now - tapLast > 2000) { tapCount = 0; tapAcc = 0; }
						else { tapAcc += now - tapLast; tapCount++; }
						tapLast = now;
						if (tapCount >= 2) { bpmX10 = clampi((int)(600000.0 / (tapAcc / tapCount) + 0.5), 300, 3000); }
						char b[24]; snprintf(b, sizeof(b), "TAP %d.%d", bpmX10 / 10, bpmX10 % 10); say(b);
					} else { lock(); int ok = undoSwap(); unlock(); say(ok ? "UNDO / REDO" : "NOTHING TO UNDO"); padsDirty = 1; }
					topDirty = 1;
				}
				if (kDown & KEY_Y) {
					if (big) { levels16 ^= 1; say(levels16 ? "16 LEVELS: TUNE" : "16 LEVELS OFF"); hdrDirty = padsDirty = 1; }
					else { fullLevel ^= 1; say(fullLevel ? "FULL LEVEL ON" : "FULL LEVEL OFF: TOUCH HEIGHT = VEL"); }
					topDirty = 1;
				}
				if ((kDown & KEY_B) && big) {
					lock(); undoSnap(); seqN[curSeq] = 0; seqResync(); unlock(); say("SEQUENCE CLEARED (X = UNDO)"); padsDirty = 1;
				}
			}
		}

		// ---------- touch
		touchPosition tp; hidTouchRead(&tp);
		int touching = !!(kHeld & KEY_TOUCH);
		if ((kDown & KEY_TOUCH) && tp.py < HDR_H) {
			int x = tp.px;
			if (x < 240) { int np = x / 40; if (np != page) { page = np; if (page == PG_CHOP) { chopSrc = selPad; chopCompute(); } if (page == PG_DISK) scanDir(); } }
			else if (x < 264) bank = (bank + 1) % 4;
			else if (x < 292) { btnMode ^= 1; say(btnMode ? "BTN MODE: D-PAD/ABXY/L = PADS 1-9" : "BTN MODE OFF"); }
			else { levels16 ^= 1; say(levels16 ? "16 LEVELS: TUNE" : "16 LEVELS OFF"); }
			topDirty = hdrDirty = padsDirty = 1;
		}
		int k = -1, relY = 0;
		if (touching && tp.py >= HDR_H) {
			int col = tp.px / 80, row = (tp.py - HDR_H) / 55;
			if (col > 3) col = 3; if (row > 3) row = 3;
			relY = (tp.py - HDR_H) % 55;
			k = (page == PG_STEP) ? row * 4 + col : (3 - row) * 4 + col;
		}
		if (k != touchK) {
			if (touchK >= 0) padUp(touchSlot);
			if (k >= 0 && (page != PG_STEP || (kDown & KEY_TOUCH))) {
				int vel = fullLevel ? 127 : clampi(127 - relY * 100 / 54, 20, 127);
				padDown(k, vel, touchSlot, kHeld);
			}
			touchK = k;
		}

		// ---------- pad flashes
		for (int i = 0; i < NPADS; i++) if (hitFlag[i]) {
			int v = hitFlag[i]; hitFlag[i] = 0;
			if (levels16 && i == selPad) { if (touchK >= 0) padFlash[touchK] = v / 127.0f; }
			else if (i / 16 == bank) padFlash[i % 16] = v / 127.0f;
		}
		if (page != PG_STEP) {
			for (int i = 0; i < 16; i++) {
				float f = padFlash[i];
				if (fabsf(f - padShown[i]) > 0.04f || (f == 0 && padShown[i] != 0)) { padShown[i] = f; if (!padsDirty) drawPad(i); }
				padFlash[i] *= 0.82f; if (padFlash[i] < 0.03f) padFlash[i] = 0;
			}
		}

		// ---------- draw
		if (hdrDirty) { drawHeader(); hdrDirty = 0; }
		if (padsDirty) { drawPads(); padsDirty = 0; }
		if (topDirty) { drawTop(); topDirty = 0; lastHdrT = -1; lastStepPh = -2; }
		drawTopLive();

		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}

	if (smplState) MICU_StopSampling();
	if (audioOK) {
		audioQuit = 1; LightEvent_Signal(&audioEv);
		if (athr) { threadJoin(athr, U64_MAX); threadFree(athr); }
		ndspChnWaveBufClear(0); ndspExit(); linearFree(audioMem);
	}
	if (micOK) micExit();
	gfxExit();
	(void)argc; (void)argv;
	return 0;
}
