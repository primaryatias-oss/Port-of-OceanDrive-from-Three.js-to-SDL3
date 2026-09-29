#include "core/common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void fatal_at(const char *file, int line, const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s:%d: %s", file, line, msg);
  fflush(stderr);
  abort();
}
