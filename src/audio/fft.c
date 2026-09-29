#include "audio/fft.h"

#include <math.h>

#include "core/common.h"
#include "core/vec.h"

void fft_init(Fft *f, int n) {
  CHECK(n >= 2 && (n & (n - 1)) == 0);
  f->n = n;
  f->cos_t = xmalloc((size_t)n / 2 * sizeof(float));
  f->sin_t = xmalloc((size_t)n / 2 * sizeof(float));
  f->rev = xmalloc((size_t)n * sizeof(int));
  for (int i = 0; i < n / 2; i++) {
    double a = 2 * 3.141592653589793 * i / n;
    f->cos_t[i] = (float)cos(a);
    f->sin_t[i] = (float)sin(a);
  }
  int bits = 0;
  while ((1 << bits) < n) bits++;
  for (int i = 0; i < n; i++) {
    int r = 0;
    for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
    f->rev[i] = r;
  }
}

void fft_free(Fft *f) {
  free(f->cos_t);
  free(f->sin_t);
  free(f->rev);
  *f = (Fft){};
}

static void transform(const Fft *f, float *re, float *im, float sign) {
  int n = f->n;
  for (int i = 0; i < n; i++) {
    int j = f->rev[i];
    if (j > i) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    int half = len >> 1, step = n / len;
    for (int i = 0; i < n; i += len)
      for (int k = 0; k < half; k++) {
        float wr = f->cos_t[k * step], wi = sign * f->sin_t[k * step];
        int a = i + k, b = a + half;
        float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - xr; im[b] = im[a] - xi;
        re[a] += xr; im[a] += xi;
      }
  }
}

void fft_forward(const Fft *f, float *re, float *im) { transform(f, re, im, -1); }
void fft_inverse(const Fft *f, float *re, float *im) { transform(f, re, im, 1); }
