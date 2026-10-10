/* The two FFT table builders the engine's convolver needs, copied from
   generalFdesign.c so an effect harness that convolves can link without the
   whole filter-design library behind that file. */
#include <math.h>
static unsigned int fhtIntegerLog2(unsigned int v)
{
	unsigned int i = 0;
	while (v > 1) { ++i; v >>= 1; }
	return i;
}
static unsigned int fhtRevBits(unsigned int x, unsigned int bits)
{
	unsigned int y = 0;
	while (bits--)
	{
		y = (y + y) + (x & 1);
		x >>= 1;
	}
	return y;
}
void fhtbitReversalTbl(unsigned *dst, unsigned int n)
{
	unsigned int bits = fhtIntegerLog2(n);
	for (unsigned int i = 0; i < n; ++i)
		dst[i] = fhtRevBits(i, bits);
}
void fhtsinHalfTblFloat(float *dst, unsigned int n)
{
	const double twopi_over_n = 6.283185307179586476925286766559 / n;
	for (unsigned int i = 0; i < n; ++i)
		dst[i] = (float)sin(twopi_over_n * i);
}
