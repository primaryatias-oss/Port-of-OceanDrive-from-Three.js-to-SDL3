// In-place iterative radix-2 complex FFT (the audio engine's convolver and wavetables).
#pragma once

#include <stddef.h>

typedef struct Fft {
  int n;              // power of two
  float *cos_t, *sin_t;
  int *rev;
} Fft;

void fft_init(Fft *f, int n);
void fft_free(Fft *f);
// forward: X[k] = sum x[j] e^(-2 pi i jk/n); inverse (unscaled): x[j] = sum X[k] e^(+2 pi i jk/n)
void fft_forward(const Fft *f, float *re, float *im);
void fft_inverse(const Fft *f, float *re, float *im);
