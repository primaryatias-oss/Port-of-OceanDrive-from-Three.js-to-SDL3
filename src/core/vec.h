// Growable arrays (JS-array stand-in). A Vec(T) is { T *data; size_t len, cap; }, zero-initialized
// empty. Allocation failure is fatal.
#pragma once

#include <stdlib.h>
#include <string.h>

#include "core/common.h"

#define Vec(T) struct { T *data; size_t len, cap; }

void *vec_grow_(void *data, size_t *cap, size_t need, size_t elem);

#define vec_reserve(v, n) \
  do { if ((n) > (v)->cap) (v)->data = vec_grow_((v)->data, &(v)->cap, (n), sizeof *(v)->data); } while (0)
#define vec_push(v, ...) \
  do { vec_reserve((v), (v)->len + 1); (v)->data[(v)->len++] = (__VA_ARGS__); } while (0)
#define vec_pop(v) ((v)->data[--(v)->len])
#define vec_last(v) ((v)->data[(v)->len - 1])
#define vec_clear(v) ((v)->len = 0)
#define vec_free(v) do { free((v)->data); (v)->data = nullptr; (v)->len = (v)->cap = 0; } while (0)
// append n elements from src
#define vec_append(v, src, n) \
  do { size_t n_ = (n); vec_reserve((v), (v)->len + n_); \
       memcpy((v)->data + (v)->len, (src), n_ * sizeof *(v)->data); (v)->len += n_; } while (0)
#define vec_resize(v, n) do { vec_reserve((v), (n)); (v)->len = (n); } while (0)

// insert x at index i (Array.prototype.splice(i, 0, x); i = 0 is unshift)
#define vec_insert(v, i, ...) \
  do { size_t i_ = (i); vec_reserve((v), (v)->len + 1); \
       memmove((v)->data + i_ + 1, (v)->data + i_, ((v)->len - i_) * sizeof *(v)->data); \
       (v)->data[i_] = (__VA_ARGS__); (v)->len++; } while (0)
// remove the element at index i (splice(i, 1); i = 0 is shift)
#define vec_remove(v, i) \
  do { size_t i_ = (i); memmove((v)->data + i_, (v)->data + i_ + 1, ((v)->len - i_ - 1) * sizeof *(v)->data); (v)->len--; } while (0)

typedef Vec(double) DVec;
typedef Vec(float) FVec;
typedef Vec(uint32_t) U32Vec;
typedef Vec(int) IVec;

// in-place reverse (Array.prototype.reverse)
#define vec_reverse(v) \
  do { for (size_t i_ = 0, j_ = (v)->len; i_ + 1 < j_; i_++, j_--) { \
         typeof(*(v)->data) t_ = (v)->data[i_]; (v)->data[i_] = (v)->data[j_ - 1]; (v)->data[j_ - 1] = t_; } } while (0)

static inline void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) FATAL("out of memory (%zu bytes)", n);
  return p;
}
static inline void *xcalloc(size_t n, size_t sz) {
  void *p = calloc(n ? n : 1, sz ? sz : 1);
  if (!p) FATAL("out of memory (%zu x %zu bytes)", n, sz);
  return p;
}
static inline void *xrealloc(void *p, size_t n) {
  p = realloc(p, n ? n : 1);
  if (!p) FATAL("out of memory (%zu bytes)", n);
  return p;
}
