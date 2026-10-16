/*
 * Damon DSP bass block: the linear-phase split every bass stage runs inside.
 *
 * The brief's hard rule for the sub-harmonic generator, the adaptive bass
 * controller and the Dolby-alike bass enhancer is that they must not bleed
 * into the mids and highs: stock ViPER Bass dulls the top and muddies vocals
 * because its harmonics spill upwards and its boost drives the master limiter
 * on the whole mix. So the stages never see the whole signal. This block:
 *
 *   1. Splits with ONE linear-phase FIR low-pass at BASSBLOCK_SPLIT_HZ. The
 *      high band is never computed as a filter at all: the dry signal is
 *      delayed by exactly the filter's group delay, so dry = low + high by
 *      construction and the recombine is sample-exact. No IIR anywhere on the
 *      dry path, so nothing is phase-smeared at the split.
 *   2. Hands the low band to the stages, which return what they want to ADD
 *      to it (a synthesised sub-octave, a shelf's worth of gain, harmonics).
 *      The dry signal itself is never altered, so a stage at neutral is a
 *      pure delay, and the null test above the split is passed before any
 *      stage is even written.
 *   3. Dips what the stages add, not the music, when adding it would push a
 *      sample past the master limiter's threshold. A few milliseconds of
 *      lookahead, a sliding-window minimum so the dip is in place before the
 *      peak, a short attack and a slow release. The master limiter should
 *      rarely engage because of bass; this is where that is enforced.
 *
 * Everything is designed from the runtime sample rate. The filter length
 * follows the rate so the transition band stays at the same frequencies, and
 * the delay in milliseconds is therefore the same at 44.1 kHz and 192 kHz.
 * Convolution is FFT-partitioned through the engine's own convolver, so the
 * cost is a few FFTs per block rather than thousands of taps per sample.
 *
 * One instance is shared by every bass stage; it is allocated when the first
 * stage is enabled and freed when the last one is disabled.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "../jdsp_header.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Kaiser windowed sinc: -6 dB at the split, 60 dB down one transition band
   above it (120 Hz -> 160 Hz). That leaves margin for the brief's null test,
   which is measured from 200 Hz up. */
#define BASSBLOCK_TRANSITION_HZ 80.0
#define BASSBLOCK_STOPBAND_DB 60.0
/* The dip on the added component. */
#define BASSBLOCK_LOOKAHEAD_MS 4.0f
#define BASSBLOCK_RELEASE_MS 150.0f
#define BASSBLOCK_CEILING 0.98f

static double bessel0(double x)
{
	double sum = 1.0, term = 1.0;
	const double hx = x * 0.5;
	for (int k = 1; k < 80; k++)
	{
		term *= hx / (double)k;
		const double t = term * term;
		sum += t;
		if (t < 1e-14 * sum)
			break;
	}
	return sum;
}

/* Kaiser's estimate of the order for the stopband and transition width, made
   odd so the group delay is a whole number of samples. */
static unsigned int bassBlockFirLength(float fs)
{
	const double dw = 2.0 * M_PI * BASSBLOCK_TRANSITION_HZ / (double)fs;
	unsigned int len = (unsigned int)ceil((BASSBLOCK_STOPBAND_DB - 8.0) / (2.285 * dw)) + 1;
	if (len < 15) len = 15;
	if (!(len & 1u)) len++;
	return len;
}

static void bassBlockDesignFir(float *h, unsigned int len, float fs)
{
	const double beta = 0.1102 * (BASSBLOCK_STOPBAND_DB - 8.7);
	const double i0b = bessel0(beta);
	const unsigned int m = (len - 1) / 2;
	/* Cutoff as a fraction of the sample rate, times two: the ideal lowpass is
	   wc * sinc(wc * k). */
	const double wc = 2.0 * (double)BASSBLOCK_SPLIT_HZ / (double)fs;
	double sum = 0.0;
	for (unsigned int n = 0; n < len; n++)
	{
		const double k = (double)n - (double)m;
		const double ideal = (k == 0.0) ? wc : sin(M_PI * wc * k) / (M_PI * k);
		const double r = k / (double)m;
		const double arg = 1.0 - r * r;
		const double w = bessel0(beta * sqrt(arg > 0.0 ? arg : 0.0)) / i0b;
		h[n] = (float)(ideal * w);
		sum += ideal * w;
	}
	/* Unity at DC, exactly, so a bass note's level is what the stages measure
	   and the complement really is the complement. */
	for (unsigned int n = 0; n < len; n++)
		h[n] = (float)((double)h[n] / sum);
}

static void bassBlockFree(BassBlock *b)
{
	if (b->allocated)
		FFTConvolver2x2Free(&b->conv);
	free(b->fir);
	for (int c = 0; c < 2; c++)
	{
		free(b->dly[c]); free(b->low[c]); free(b->add[c]);
		free(b->laDry[c]); free(b->laAdd[c]);
	}
	free(b->dqVal);
	free(b->dqIdx);
	memset(b, 0, sizeof(BassBlock));
}

static int bassBlockAlloc(JamesDSPLib *jdsp)
{
	BassBlock *b = &jdsp->bassBlock;
	const float fs = jdsp->fs > 0.0f ? jdsp->fs : 48000.0f;
	const unsigned int block = jdsp->blockSize > 0 ? (unsigned int)jdsp->blockSize : 256u;
	unsigned int scratch = (unsigned int)jdsp->blockSizeMax;
	if (scratch < block) scratch = block;

	bassBlockFree(b);
	b->fs = fs;
	b->blockSize = block;
	b->firLen = bassBlockFirLength(fs);
	b->delay = (b->firLen - 1) / 2;
	b->fir = (float *)malloc(b->firLen * sizeof(float));
	if (!b->fir) { bassBlockFree(b); return 0; }
	bassBlockDesignFir(b->fir, b->firLen, fs);

	FFTConvolver2x2Init(&b->conv);
	b->allocated = 1; /* so a failure below frees the convolver too */
	if (!FFTConvolver2x2LoadImpulseResponse(&b->conv, block, b->fir, b->fir, b->firLen))
	{
		bassBlockFree(b);
		return 0;
	}

	b->laLen = (unsigned int)(BASSBLOCK_LOOKAHEAD_MS * 0.001f * fs + 0.5f);
	if (b->laLen < 1) b->laLen = 1;
	b->dqCap = b->laLen + 2;
	b->scratchLen = scratch;
	for (int c = 0; c < 2; c++)
	{
		b->dly[c] = (float *)calloc(b->delay, sizeof(float));
		b->low[c] = (float *)calloc(scratch, sizeof(float));
		b->add[c] = (float *)calloc(scratch, sizeof(float));
		b->laDry[c] = (float *)calloc(b->laLen, sizeof(float));
		b->laAdd[c] = (float *)calloc(b->laLen, sizeof(float));
		if (!b->dly[c] || !b->low[c] || !b->add[c] || !b->laDry[c] || !b->laAdd[c])
		{
			bassBlockFree(b);
			return 0;
		}
	}
	b->dqVal = (float *)calloc(b->dqCap, sizeof(float));
	b->dqIdx = (unsigned long long *)calloc(b->dqCap, sizeof(unsigned long long));
	if (!b->dqVal || !b->dqIdx) { bassBlockFree(b); return 0; }

	b->gain = 1.0f;
	/* Reaches 98% of a cut within the lookahead, so the dip is in place when
	   the peak it was computed for comes out of the delay line. */
	b->attackCoef = 1.0f - expf(-4.0f / (float)b->laLen);
	b->releaseCoef = 1.0f - expf(-1.0f / (BASSBLOCK_RELEASE_MS * 0.001f * fs));
	return 1;
}

/* Called by a stage when it is switched on. Holds the engine lock already. */
int BassBlockAcquire(JamesDSPLib *jdsp)
{
	if (jdsp->bassBlock.allocated)
		return 1;
	return bassBlockAlloc(jdsp);
}

/* Which stages are on. Extended as the controller and enhancer arrive. */
int BassBlockActive(JamesDSPLib *jdsp)
{
	return jdsp->subHarmonicEnabled;
}

void BassBlockRelease(JamesDSPLib *jdsp)
{
	bassBlockFree(&jdsp->bassBlock);
}

/* Rate changed: redesign at the new rate. No lock, the caller holds it. */
void BassBlockRefresh(JamesDSPLib *jdsp)
{
	if (!jdsp->bassBlock.allocated)
		return;
	if (!bassBlockAlloc(jdsp))
		jdsp->subHarmonicEnabled = 0;
}

/* Latency the block adds, in samples: the split's group delay plus the dip's
   lookahead. Reported so a video sync mode can account for it later. */
unsigned int BassBlockLatency(JamesDSPLib *jdsp)
{
	const BassBlock *b = &jdsp->bassBlock;
	return b->allocated ? b->delay + b->laLen : 0;
}

void BassBlockProcess(JamesDSPLib *jdsp, size_t n)
{
	BassBlock *b = &jdsp->bassBlock;
	if (!b->allocated || n == 0)
		return;
	if (n > b->scratchLen)
	{
		/* The host grew its buffer after the block was built. */
		for (int c = 0; c < 2; c++)
		{
			float *nl = (float *)realloc(b->low[c], n * sizeof(float));
			float *na = (float *)realloc(b->add[c], n * sizeof(float));
			if (!nl || !na) { free(nl); free(na); return; }
			b->low[c] = nl; b->add[c] = na;
		}
		b->scratchLen = (unsigned int)n;
	}
	float *L = jdsp->tmpBuffer[0];
	float *R = jdsp->tmpBuffer[1];

	/* 1. The low band, linear phase. */
	FFTConvolver2x2Process(&b->conv, L, R, b->low[0], b->low[1], (unsigned int)n);

	/* 2. What the stages add to it. */
	if (jdsp->subHarmonicEnabled)
		SubHarmonicRender(jdsp, b->low[0], b->low[1], b->add[0], b->add[1], n);
	else
	{
		memset(b->add[0], 0, n * sizeof(float));
		memset(b->add[1], 0, n * sizeof(float));
	}

	/* 3. The dry signal, delayed by the filter's group delay. Swapped through
	   the ring buffer in place, so this is a copy and not an arithmetic
	   operation: at neutral the block is bit-exact. */
	unsigned int pos = b->dlyPos;
	for (int c = 0; c < 2; c++)
	{
		float *buf = jdsp->tmpBuffer[c];
		float *d = b->dly[c];
		pos = b->dlyPos;
		for (size_t i = 0; i < n; i++)
		{
			const float x = buf[i];
			buf[i] = d[pos];
			d[pos] = x;
			if (++pos == b->delay) pos = 0;
		}
	}
	b->dlyPos = pos;

	/* 4. Recombine through the dip. The ceiling follows the master limiter's
	   threshold, a hair under it, so bass alone never reaches it. */
	float ceiling = jdsp->limiter.threshold;
	if (!(ceiling > 0.05f) || ceiling > 1.0f) ceiling = 1.0f;
	ceiling *= BASSBLOCK_CEILING;

	const unsigned int la = b->laLen;
	unsigned int laPos = b->laPos;
	for (size_t i = 0; i < n; i++)
	{
		const float dryL = L[i], dryR = R[i];
		const float addL = b->add[0][i], addR = b->add[1][i];

		/* How much of the addition this sample can take. Conservative: the
		   addition is assumed to land in phase with the dry peak. */
		float req = 1.0f;
		{
			const float aL = fabsf(addL), aR = fabsf(addR);
			if (aL > 1e-9f)
			{
				const float room = ceiling - fabsf(dryL);
				const float r = room <= 0.0f ? 0.0f : room / aL;
				if (r < req) req = r;
			}
			if (aR > 1e-9f)
			{
				const float room = ceiling - fabsf(dryR);
				const float r = room <= 0.0f ? 0.0f : room / aR;
				if (r < req) req = r;
			}
		}

		/* Sliding-window minimum over the lookahead: a monotonic deque of
		   (index, requirement), so the dip for a peak is known la samples
		   before that peak leaves the delay line. */
		const unsigned long long idx = b->sampleIndex++;
		while (b->dqCount > 0)
		{
			const unsigned int back = (b->dqHead + b->dqCount - 1) % b->dqCap;
			if (b->dqVal[back] < req) break;
			b->dqCount--;
		}
		{
			const unsigned int slot = (b->dqHead + b->dqCount) % b->dqCap;
			b->dqVal[slot] = req;
			b->dqIdx[slot] = idx;
			b->dqCount++;
		}
		while (b->dqCount > 0 && b->dqIdx[b->dqHead] + la < idx)
		{
			b->dqHead = (b->dqHead + 1) % b->dqCap;
			b->dqCount--;
		}
		const float target = b->dqVal[b->dqHead];
		if (target < b->gain)
			b->gain += b->attackCoef * (target - b->gain);
		else
			b->gain += b->releaseCoef * (target - b->gain);

		/* Output the sample from la ago, with its addition dipped. */
		const float outDryL = b->laDry[0][laPos], outDryR = b->laDry[1][laPos];
		const float outAddL = b->laAdd[0][laPos], outAddR = b->laAdd[1][laPos];
		b->laDry[0][laPos] = dryL; b->laDry[1][laPos] = dryR;
		b->laAdd[0][laPos] = addL; b->laAdd[1][laPos] = addR;
		if (++laPos == la) laPos = 0;

		L[i] = outDryL + b->gain * outAddL;
		R[i] = outDryR + b->gain * outAddR;
	}
	b->laPos = laPos;
}
