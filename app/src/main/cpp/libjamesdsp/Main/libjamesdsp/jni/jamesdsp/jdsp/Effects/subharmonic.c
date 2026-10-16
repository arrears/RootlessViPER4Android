/*
 * Sub-harmonic generator: an octave below the bass that is already there.
 *
 * What an AudioControl Epicenter does for a car, and what older mixes need:
 * a bass guitar's fundamentals sit at 40 to 120 Hz (low E is 41 Hz) and a
 * 1970s record has nothing underneath them, so on a system with real sub
 * extension the bottom is simply missing. This listens inside a user-set band
 * of the low band, locks onto the note, and adds a clean sine one octave down.
 *
 *   Mix       Level of the sub relative to the bass it came from, in dB.
 *             Conservative by default: the GR2 has real sub extension and the
 *             brief says start low. The bottom of the slider is off.
 *   Sweep     Centre of the band it listens to.
 *   Wide      Width of that band, in octaves. The default covers about
 *             50 to 120 Hz.
 *   Dynamic   On, the sub follows the bass cycle by cycle. Off, it follows a
 *             slow average of the note and sits at a steadier level, closer
 *             to a constant sub foundation.
 *
 * How the octave is found. The classic analogue method - a flip-flop toggled
 * by the fundamental's zero crossings, ring-modulating the input - puts a
 * component at three halves of the fundamental as well as at half of it,
 * and for a 50 Hz note that lands at 75 Hz: inside the sub band, so no
 * low-pass can remove it, and it is heard as mud. Here the zero crossings
 * only MEASURE the note. Each rising crossing of the detector band is timed
 * to a fraction of a sample, so the period is known to far better than the
 * sample grid even at 44.1 kHz; the peak of each cycle gives its amplitude.
 * An oscillator runs at half that frequency, and the phase error at each
 * crossing (a half-rate sine passes through zero on every other crossing of
 * the note, alternately rising and falling) is paid off by nudging the
 * FREQUENCY over the next cycle rather than stepping the phase. The output is
 * a sine at exactly half the frequency with no three-halves product, no steps
 * and nothing to click, and its level changes only at cycle boundaries,
 * smoothed, so there is no envelope ripple riding on it either.
 *
 * Everything it adds is low-passed at SUB_LP_HZ with an eighth-order
 * Butterworth (48 dB per octave), which is the brief's hard low-pass on
 * generated content with margin: 43 dB down by 150 Hz, over 60 dB by 200 Hz.
 * The whole stage runs inside the bass block, so the mids and highs are the
 * delayed dry signal and nothing else.
 *
 * Detection is on the mono sum of the low band and the same sub is added to
 * both channels. Bass is mono in practically every mix, and two trackers
 * disagreeing between the channels would be the one way to make it phasey.
 *
 * Double precision throughout the tracker. At 192 kHz a 40 Hz section is at
 * a normalised frequency of 0.0002; float coefficients there cannot hold the
 * stopband, and the detector band's selectivity is what keeps a sub from
 * being hung on the wrong note.
 */
#include <math.h>
#include <string.h>
#include "../jdsp_header.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SUB_LP_HZ 80.0
#define SUB_GATE_DB -50.0f
/* Output level smoothing: short enough to follow a note's decay cycle by
   cycle, long enough that a level change at a cycle boundary is a slope and
   not a step. */
#define SUB_LEVEL_MS 6.0
/* The envelope used only for the crossing detector's hysteresis. */
#define SUB_ENV_MS 5.0
/* Static mode's slow average of the cycle peaks. */
#define SUB_SLOW_ATTACK_MS 80.0
#define SUB_SLOW_RELEASE_MS 600.0
/* The detector arms below minus this fraction of the envelope and fires on
   the next zero crossing, so noise around zero cannot double-trigger. */
#define SUB_HYST 0.12
/* How far outside the band a period may fall before the lock is dropped. */
#define SUB_PERIOD_MARGIN 1.6
/* Fraction of the phase error paid off over the following cycle. */
#define SUB_LOCK_GAIN 0.7

static double subOnePole(double ms, double fs)
{
	return 1.0 - exp(-1.0 / (ms * 0.001 * fs));
}

static double subClampHz(double f, float fs)
{
	if (f < 10.0) f = 10.0;
	if (f > (double)fs * 0.45) f = (double)fs * 0.45;
	return f;
}

static inline double subBiqRun(SubBiquad *s, double x)
{
	const double y = s->b0 * x + s->z1;
	s->z1 = s->b1 * x - s->a1 * y + s->z2;
	s->z2 = s->b2 * x - s->a2 * y;
	return y;
}

static void subBiqReset(SubBiquad *s) { s->z1 = s->z2 = 0.0; }

static void subDesignLowpass(SubBiquad *s, double f, float fs, double q)
{
	const double w0 = 2.0 * M_PI * subClampHz(f, fs) / (double)fs;
	const double cw = cos(w0);
	const double alpha = sin(w0) / (2.0 * q);
	const double a0 = 1.0 + alpha;
	s->b0 = ((1.0 - cw) * 0.5) / a0;
	s->b1 = (1.0 - cw) / a0;
	s->b2 = s->b0;
	s->a1 = (-2.0 * cw) / a0;
	s->a2 = (1.0 - alpha) / a0;
}

static void subDesignHighpass(SubBiquad *s, double f, float fs, double q)
{
	const double w0 = 2.0 * M_PI * subClampHz(f, fs) / (double)fs;
	const double cw = cos(w0);
	const double alpha = sin(w0) / (2.0 * q);
	const double a0 = 1.0 + alpha;
	s->b0 = ((1.0 + cw) * 0.5) / a0;
	s->b1 = (-(1.0 + cw)) / a0;
	s->b2 = s->b0;
	s->a1 = (-2.0 * cw) / a0;
	s->a2 = (1.0 - alpha) / a0;
}

/* Everything that depends on the sample rate, derived from the settings held,
   so a rate change redoes the design without replaying SetParam. */
static void subDesign(SubHarmonic *s, float fs)
{
	s->fs = fs;
	const float half = s->wideOct * 0.5f;
	s->bandLoHz = s->sweepHz * powf(2.0f, -half);
	s->bandHiHz = s->sweepHz * powf(2.0f, half);
	if (s->bandLoHz < 20.0f) s->bandLoHz = 20.0f;
	if (s->bandHiHz > 250.0f) s->bandHiHz = 250.0f;
	if (s->bandHiHz < s->bandLoHz * 1.1f) s->bandHiHz = s->bandLoHz * 1.1f;

	/* Fourth-order Butterworth edges on the detector: the two section Qs. */
	subDesignHighpass(&s->bpHp[0], s->bandLoHz, fs, 0.54119610);
	subDesignHighpass(&s->bpHp[1], s->bandLoHz, fs, 1.30656296);
	subDesignLowpass(&s->bpLp[0], s->bandHiHz, fs, 0.54119610);
	subDesignLowpass(&s->bpLp[1], s->bandHiHz, fs, 1.30656296);

	/* Eighth-order Butterworth on what is generated. */
	static const double q8[4] = { 0.50979558, 0.60134489, 0.89997622, 2.56291545 };
	for (int k = 0; k < 4; k++)
		subDesignLowpass(&s->lp[k], SUB_LP_HZ, fs, q8[k]);

	s->envCoef = subOnePole(SUB_ENV_MS, fs);
	s->attSlow = subOnePole(SUB_SLOW_ATTACK_MS, fs);
	s->relSlow = subOnePole(SUB_SLOW_RELEASE_MS, fs);
	s->levelCoef = subOnePole(SUB_LEVEL_MS, fs);

	s->minPeriod = (double)fs / ((double)s->bandHiHz * SUB_PERIOD_MARGIN);
	s->maxPeriod = (double)fs * SUB_PERIOD_MARGIN / (double)s->bandLoHz;
	if (s->periodSm < s->minPeriod || s->periodSm > s->maxPeriod)
	{
		s->periodSm = (double)fs / (double)s->sweepHz;
		s->tracking = 0;
	}
	s->phaseInc = M_PI / s->periodSm;
}

static void subReset(SubHarmonic *s)
{
	for (int k = 0; k < 2; k++) { subBiqReset(&s->bpHp[k]); subBiqReset(&s->bpLp[k]); }
	for (int k = 0; k < 4; k++) subBiqReset(&s->lp[k]);
	s->prevBp = 0.0;
	s->sinceCross = 0.0;
	s->cyclePeak = s->cycleAmp = s->ampSlow = s->env = 0.0;
	s->armed = 0;
	s->parity = 0;
	s->tracking = 0;
	s->phase = 0.0;
	s->level = 0.0;
}

void SubHarmonicRefresh(JamesDSPLib *jdsp)
{
	subDesign(&jdsp->subHarmonic, jdsp->fs > 0.0f ? jdsp->fs : 48000.0f);
}

void SubHarmonicSetParam(JamesDSPLib *jdsp, float mixDb, float sweepHz, float wideOct, int dynamic)
{
	/* Held for the whole update: Render runs on the audio thread under this
	   same lock, and half-new filter coefficients are a burst, not a glitch. */
	jdsp_lock(jdsp);
	SubHarmonic *s = &jdsp->subHarmonic;
	const float fs = jdsp->fs > 0.0f ? jdsp->fs : 48000.0f;

	if (mixDb < -40.0f) mixDb = -40.0f;
	if (mixDb > 6.0f) mixDb = 6.0f;
	if (sweepHz < 30.0f) sweepHz = 30.0f;
	if (sweepHz > 200.0f) sweepHz = 200.0f;
	if (wideOct < 0.25f) wideOct = 0.25f;
	if (wideOct > 4.0f) wideOct = 4.0f;

	s->mixDb = mixDb;
	s->sweepHz = sweepHz;
	s->wideOct = wideOct;
	s->dynamic = dynamic ? 1 : 0;
	/* The bottom of the slider is off, not merely quiet. */
	s->mixGain = mixDb <= -39.9f ? 0.0f : powf(10.0f, mixDb / 20.0f);
	s->gate = powf(10.0f, SUB_GATE_DB / 20.0f);
	subDesign(s, fs);
	s->transparent = s->mixGain <= 0.0f;
	jdsp_unlock(jdsp);
}

/* A rising zero crossing of the detector band, frac samples before the
   current sample. Measure the cycle that just ended and steer the oscillator
   through the one about to start. */
static inline void subCrossing(SubHarmonic *s, double frac)
{
	const double period = s->sinceCross - frac;
	s->sinceCross = frac;
	const double amp = s->cyclePeak;
	s->cyclePeak = 0.0;

	if (period < s->minPeriod || period > s->maxPeriod)
	{
		/* Not a note in the band, or the first crossing after silence, whose
		   "period" is the length of the silence. Wait for the next one. */
		s->tracking = 0;
		s->parity = 0;
		return;
	}
	s->cycleAmp = amp;
	if (!s->tracking)
	{
		s->periodSm = period;
		s->tracking = 1;
		s->parity = 0;
		/* Start the sub rising through zero exactly where the note did. */
		s->phase = frac * (M_PI / period);
		s->phaseInc = M_PI / period;
	}
	else
	{
		s->periodSm += 0.5 * (period - s->periodSm);
		/* Where the sub was at the crossing instant, and where a half-rate
		   sine should have been: zero, rising on even crossings and falling
		   on odd ones. */
		const double atCross = s->phase - frac * s->phaseInc;
		const double target = s->parity ? M_PI : 0.0;
		double err = target - atCross;
		err -= 2.0 * M_PI * floor((err + M_PI) / (2.0 * M_PI));
		/* Paid off as a frequency offset over the coming cycle: continuous
		   phase, so no step and no sideband at the note's frequency. */
		s->phaseInc = (M_PI + SUB_LOCK_GAIN * err) / s->periodSm;
		if (s->phaseInc < 0.5 * M_PI / s->maxPeriod) s->phaseInc = 0.5 * M_PI / s->maxPeriod;
	}
	s->parity ^= 1;
}

void SubHarmonicRender(JamesDSPLib *jdsp, const float *lowL, const float *lowR,
                       float *addL, float *addR, size_t n)
{
	SubHarmonic *s = &jdsp->subHarmonic;
	if (s->transparent)
	{
		memset(addL, 0, n * sizeof(float));
		memset(addR, 0, n * sizeof(float));
		return;
	}
	const double twoPi = 2.0 * M_PI;
	const double lost = 2.0 * s->maxPeriod;
	const double gate = (double)s->gate;
	for (size_t i = 0; i < n; i++)
	{
		double bp = 0.5 * ((double)lowL[i] + (double)lowR[i]);
		bp = subBiqRun(&s->bpHp[0], bp);
		bp = subBiqRun(&s->bpHp[1], bp);
		bp = subBiqRun(&s->bpLp[0], bp);
		bp = subBiqRun(&s->bpLp[1], bp);

		const double a = fabs(bp);
		if (a > s->cyclePeak) s->cyclePeak = a;
		s->env += s->envCoef * (a - s->env);

		double h = SUB_HYST * s->env;
		if (h < gate * 0.5) h = gate * 0.5;
		s->sinceCross += 1.0;
		if (s->armed)
		{
			if (bp >= 0.0)
			{
				/* Where between the previous sample and this one it crossed. */
				const double d = bp - s->prevBp;
				const double frac = d > 0.0 ? bp / d : 0.0;
				s->armed = 0;
				subCrossing(s, frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac));
			}
		}
		else if (bp < -h)
			s->armed = 1;
		s->prevBp = bp;
		if (s->sinceCross > lost)
			s->tracking = 0;

		/* Amplitude: the last cycle's peak, or a slow average of it. */
		s->ampSlow += (s->cycleAmp > s->ampSlow ? s->attSlow : s->relSlow) * (s->cycleAmp - s->ampSlow);
		const double amp = s->dynamic ? s->cycleAmp : s->ampSlow;
		const double target = (s->tracking && amp > gate) ? amp * (double)s->mixGain : 0.0;
		s->level += s->levelCoef * (target - s->level);

		s->phase += s->phaseInc;
		if (s->phase >= twoPi) s->phase -= twoPi;
		double y = s->level * sin(s->phase);
		y = subBiqRun(&s->lp[0], y);
		y = subBiqRun(&s->lp[1], y);
		y = subBiqRun(&s->lp[2], y);
		y = subBiqRun(&s->lp[3], y);
		addL[i] = (float)y;
		addR[i] = (float)y;
	}
}

void SubHarmonicEnable(JamesDSPLib *jdsp)
{
	jdsp_lock(jdsp);
	if (!jdsp->subHarmonicEnabled)
	{
		SubHarmonic *s = &jdsp->subHarmonic;
		subDesign(s, jdsp->fs > 0.0f ? jdsp->fs : 48000.0f);
		subReset(s);
		if (BassBlockAcquire(jdsp))
			jdsp->subHarmonicEnabled = 1;
	}
	jdsp_unlock(jdsp);
}

void SubHarmonicDisable(JamesDSPLib *jdsp)
{
	jdsp_lock(jdsp);
	jdsp->subHarmonicEnabled = 0;
	/* The block's buffers are only held while a stage needs them. */
	if (!BassBlockActive(jdsp))
		BassBlockRelease(jdsp);
	jdsp_unlock(jdsp);
}
