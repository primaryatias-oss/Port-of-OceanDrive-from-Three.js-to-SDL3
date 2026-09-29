#include "core/vec.h"

void *vec_grow_(void *data, size_t *cap, size_t need, size_t elem) {
  size_t c = *cap ? *cap : 8;
  while (c < need) c *= 2;
  data = xrealloc(data, c * elem);
  *cap = c;
  return data;
}
