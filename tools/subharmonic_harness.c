// Does the sub-harmonic generator add an octave below the bass and nothing
// above it, and is the bass block it runs inside exact where it must be?
//
//   J=app/src/main/cpp/libjamesdsp/Main/libjamesdsp/jni/jamesdsp/jdsp
//   $NDK/aarch64-linux-android29-clang -O2 -w -ffp-contract=off \
//       -I $J -I $J/Effects/eel2 -I $J/Effects/eel2/numericSys \
//       tools/subharmonic_harness.c $J/Effects/subharmonic.c $J/Effects/bassblock.c \
//       $J/Effects/eel2/numericSys/FFTConvolver.c $J/Effects/eel2/numericSys/codelet.c \
//       $J/Effects/eel2/fft.c tools/lockstub.c -lm -lpthread -o subharmonic
//
// The same line builds with a host gcc, which is how it is run in CI-less
// sessions. Every probe tone sits on an even FFT bin of the analysis window so
// its sub-octave lands on a bin too; unwindowed Goertzel on an off-bin tone
// leaks about 50 dB, which is louder than most of what is measured here.
//
// Everything runs at 48 kHz and again at 192 kHz: the brief's rule is that
// nothing may assume a rate, and the sub must land at the same frequency and
// the block at the same delay in milliseconds at both.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "jdsp_header.h"

#define SECONDS 4
#define NWIN 65536

static JamesDSPLib g_lib;
static float *bufL, *bufR, *dryL, *dryR;
static int N;
static int failures = 0;

static void prepare(float fs, int block)
{
	memset(&g_lib, 0, sizeof(g_lib));
	g_lib.fs = fs;
	g_lib.blockSize = block;
	/* A whole number of blocks, so the analysis window at the end never
	   straddles unprocessed samples. */
	N = ((int)fs * SECONDS / block) * block;
	g_lib.blockSizeMax = block;
	g_lib.tmpBuffer[0] = bufL;
	g_lib.tmpBuffer[1] = bufR;
	g_lib.limiter.threshold = 1.0f;
	SubHarmonicSetParam(&g_lib, -12.0f, 77.0f, 1.25f, 1);
}

static double binFreq(double want, double fs)
{
	double k = floor(want * NWIN / fs + 0.5);
	if (k < 2.0) k = 2.0;
	if (fmod(k, 2.0) != 0.0) k += 1.0;
	return k * fs / NWIN;
}

static void clearSignal(void)
{
	memset(bufL, 0, N * sizeof(float));
	memset(bufR, 0, N * sizeof(float));
}

static void addTone(double freq, double fs, float amp)
{
	for (int i = 0; i < N; i++)
	{
		const float v = (float)(amp * sin(2.0 * M_PI * freq * (double)i / fs));
		bufL[i] += v;
		bufR[i] += v;
	}
}

static void snapshotDry(void)
{
	memcpy(dryL, bufL, N * sizeof(float));
	memcpy(dryR, bufR, N * sizeof(float));
}

/* Through the block in host-sized pieces, as the engine would. */
static void run(int block)
{
	for (int i = 0; i + block <= N; i += block)
	{
		g_lib.tmpBuffer[0] = bufL + i;
		g_lib.tmpBuffer[1] = bufR + i;
		BassBlockProcess(&g_lib, (size_t)block);
	}
	g_lib.tmpBuffer[0] = bufL;
	g_lib.tmpBuffer[1] = bufR;
}

/* Amplitude of a sine at freq over the last NWIN samples. */
static double toneAmp(const float *b, double freq, double fs)
{
	const int start = N - NWIN;
	const double w = 2.0 * M_PI * freq / fs;
	const double c = 2.0 * cos(w);
	double s0 = 0.0, s1 = 0.0, s2 = 0.0;
	for (int i = 0; i < NWIN; i++)
	{
		s0 = (double)b[start + i] + c * s1 - s2;
		s2 = s1; s1 = s0;
	}
	const double re = s1 - s2 * cos(w), im = s2 * sin(w);
	return 2.0 * sqrt(re * re + im * im) / (double)NWIN;
}

static double peakOf(const float *b)
{
	double p = 0.0;
	for (int i = N / 2; i < N; i++) if (fabs((double)b[i]) > p) p = fabs((double)b[i]);
	return p;
}

static double db(double x) { return 20.0 * log10(x > 1e-12 ? x : 1e-12); }

static void check(const char *what, double got, double lo, double hi)
{
	const int ok = got >= lo && got <= hi;
	printf("  %-56s %10.4f  [%9.4f..%9.4f]  %s\n", what, got, lo, hi, ok ? "ok" : "FAIL");
	if (!ok) failures++;
}

static unsigned int latencyAt(float fs, int block)
{
	prepare(fs, block);
	SubHarmonicEnable(&g_lib);
	const unsigned int l = BassBlockLatency(&g_lib);
	SubHarmonicDisable(&g_lib);
	return l;
}

static void suite(float fs, int block)
{
	printf("\n==== %.0f Hz, %d-sample blocks ====\n", fs, block);
	const double f100 = binFreq(100.0, fs), f50 = f100 / 2.0;

	printf("neutral is a pure delay\n");
	{
		prepare(fs, block);
		SubHarmonicSetParam(&g_lib, -40.0f, 77.0f, 1.25f, 1); /* bottom of the slider: off */
		SubHarmonicEnable(&g_lib);
		const unsigned int lat = BassBlockLatency(&g_lib);
		clearSignal();
		addTone(f100, fs, 0.4f); addTone(binFreq(1000.0, fs), fs, 0.2f); addTone(binFreq(7000.0, fs), fs, 0.1f);
		for (int i = 0; i < N; i++) bufR[i] *= 0.5f; /* distinct channels */
		snapshotDry();
		run(block);
		int same = 1;
		for (int i = (int)lat; i < N - block; i++)
			if (bufL[i] != dryL[i - (int)lat] || bufR[i] != dryR[i - (int)lat]) { same = 0; break; }
		check("output is the input delayed, bit-exact", same, 1, 1);
		check("block latency, ms", 1000.0 * lat / fs, 15.0, 40.0);
		SubHarmonicDisable(&g_lib);
		check("buffers released when the last stage is off", g_lib.bassBlock.allocated, 0, 0);
	}

	double subDefault = 0.0;
	printf("an octave below, and nothing above\n");
	{
		prepare(fs, block);
		SubHarmonicEnable(&g_lib);
		clearSignal();
		addTone(f100, fs, 0.5f);
		run(block);
		subDefault = toneAmp(bufL, f50, fs);
		/* -12 dB of a 0.5 note is 0.126; the sine's own 80 Hz low-pass and the
		   detector's band edges cost a little. */
		check("100 Hz note grows a 50 Hz sub (dB re note)", db(subDefault / 0.5), -16.0, -9.0);
		check("left and right carry the same sub", toneAmp(bufR, f50, fs) / subDefault, 0.999, 1.001);
		check("the note itself is untouched", toneAmp(bufL, f100, fs) / 0.5, 0.99, 1.01);
		check("nothing at three halves (dB re sub)", db(toneAmp(bufL, f50 * 3.0, fs) / subDefault), -400.0, -40.0);
		check("nothing at 200 Hz (dB re note)", db(toneAmp(bufL, f100 * 2.0, fs) / 0.5), -400.0, -70.0);
		check("nothing at 300 Hz (dB re note)", db(toneAmp(bufL, f100 * 3.0, fs) / 0.5), -400.0, -70.0);
		check("no DC", fabs(toneAmp(bufL, 0.0, fs)) * 0.5, 0.0, 1e-3);
	}

	printf("static mode still finds the note\n");
	{
		prepare(fs, block);
		SubHarmonicSetParam(&g_lib, -12.0f, 77.0f, 1.25f, 0);
		SubHarmonicEnable(&g_lib);
		clearSignal();
		addTone(f100, fs, 0.5f);
		run(block);
		check("50 Hz sub in static mode (dB re dynamic)", db(toneAmp(bufL, f50, fs) / subDefault), -3.0, 1.0);
	}

	printf("the band chooses the note\n");
	{
		prepare(fs, block);
		/* 38 to 53 Hz: the 100 Hz note is an octave above the band. */
		SubHarmonicSetParam(&g_lib, -12.0f, 45.0f, 0.5f, 1);
		SubHarmonicEnable(&g_lib);
		clearSignal();
		addTone(f100, fs, 0.5f);
		run(block);
		check("note outside the band gets far less sub (dB re in-band)", db(toneAmp(bufL, f50, fs) / subDefault), -400.0, -12.0);
	}

	printf("null above the split at full mix\n");
	{
		prepare(fs, block);
		SubHarmonicSetParam(&g_lib, 0.0f, 77.0f, 1.25f, 1);
		SubHarmonicEnable(&g_lib);
		const unsigned int lat = BassBlockLatency(&g_lib);
		clearSignal();
		const double probes[5] = { binFreq(240.0, fs), binFreq(300.0, fs), binFreq(1000.0, fs), binFreq(3000.0, fs), binFreq(8000.0, fs) };
		addTone(binFreq(80.0, fs), fs, 0.3f);
		for (int k = 0; k < 5; k++) addTone(probes[k], fs, 0.08f);
		snapshotDry();
		run(block);
		/* Compare against the dry signal at the same delay, so the window
		   holds the same cycles of each tone. */
		memmove(dryL + lat, dryL, (N - lat) * sizeof(float));
		for (int k = 0; k < 5; k++)
		{
			char what[80];
			snprintf(what, sizeof(what), "%.0f Hz within 0.5 dB of bypass (dB)", probes[k]);
			check(what, db(toneAmp(bufL, probes[k], fs) / toneAmp(dryL, probes[k], fs)), -0.5, 0.5);
		}
		check("and a sub was actually added (dB re 80 Hz note)", db(toneAmp(bufL, binFreq(80.0, fs) / 2.0, fs) / 0.3), -6.0, 2.0);
	}

	printf("the dip keeps bass out of the limiter\n");
	{
		prepare(fs, block);
		SubHarmonicSetParam(&g_lib, 0.0f, 77.0f, 1.25f, 1);
		SubHarmonicEnable(&g_lib);
		clearSignal();
		addTone(binFreq(60.0, fs), fs, 0.95f);
		run(block);
		/* Undipped this would peak near 1.9. The ceiling is 0.98 of the
		   limiter threshold, with a little room for the attack ramp. */
		check("peak with a hot note and a full sub", peakOf(bufL), 0.9, 1.0);
		check("the sub is still there (dB re note)", db(toneAmp(bufL, binFreq(60.0, fs) / 2.0, fs) / 0.95), -40.0, 0.0);
	}
}

int main(void)
{
	const int maxN = 192000 * SECONDS;
	bufL = (float *)calloc(maxN, sizeof(float));
	bufR = (float *)calloc(maxN, sizeof(float));
	dryL = (float *)calloc(maxN, sizeof(float));
	dryR = (float *)calloc(maxN, sizeof(float));

	printf("== sub-harmonic generator ==\n");
	suite(48000.0f, 1024);
	suite(192000.0f, 4096);
	suite(44100.0f, 192);

	printf("\nrate independence\n");
	{
		const double ms48 = 1000.0 * latencyAt(48000.0f, 1024) / 48000.0;
		const double ms192 = 1000.0 * latencyAt(192000.0f, 4096) / 192000.0;
		check("same delay in ms at 48k and 192k", fabs(ms48 - ms192), 0.0, 0.05);
	}

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
