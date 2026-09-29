// imgdiff: compares two BMP images (the C port's --shot against a converted reference).
//   imgdiff A.bmp B.bmp [DIFF.bmp]
// Prints max / mean channel difference, PSNR and how many pixels differ by more than 2 and 8
// levels; writes an amplified difference image when asked. Exit 1 if sizes differ.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

static SDL_Surface *load(const char *path) {
  SDL_Surface *s = SDL_LoadBMP(path);
  if (!s) { fprintf(stderr, "imgdiff: %s: %s\n", path, SDL_GetError()); exit(2); }
  SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(s);
  return c;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: imgdiff A.bmp B.bmp [DIFF.bmp]\n"); return 2; }
  SDL_Surface *a = load(argv[1]), *b = load(argv[2]);
  if (a->w != b->w || a->h != b->h) { printf("size differs: %dx%d vs %dx%d\n", a->w, a->h, b->w, b->h); return 1; }
  int w = a->w, h = a->h, maxd = 0;
  long over2 = 0, over8 = 0;
  double sum = 0, sq = 0;
  SDL_Surface *d = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
  for (int y = 0; y < h; y++) {
    const uint8_t *pa = (const uint8_t *)a->pixels + y * a->pitch, *pb = (const uint8_t *)b->pixels + y * b->pitch;
    uint8_t *pd = (uint8_t *)d->pixels + y * d->pitch;
    for (int x = 0; x < w; x++) {
      int m = 0;
      for (int c = 0; c < 3; c++) {
        int v = abs((int)pa[x * 4 + c] - (int)pb[x * 4 + c]);
        sum += v;
        sq += (double)v * v;
        if (v > m) m = v;
        int amp = v * 16;
        pd[x * 4 + c] = (uint8_t)(amp > 255 ? 255 : amp);
      }
      pd[x * 4 + 3] = 255;
      if (m > maxd) maxd = m;
      over2 += m > 2;
      over8 += m > 8;
    }
  }
  double n = (double)w * h * 3, mse = sq / n;
  double psnr = mse > 0 ? 10 * log10(255.0 * 255.0 / mse) : INFINITY;
  printf("max %d  mean %.3f  PSNR %.2f dB  pixels >2: %.3f%%  >8: %.3f%%\n", maxd, sum / n, psnr,
         100.0 * over2 / ((double)w * h), 100.0 * over8 / ((double)w * h));
  if (argc > 3) SDL_SaveBMP(d, argv[3]);
  return 0;
}
