// Shared basics: fixed-width types, small helpers, logging and fatal errors.
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <SDL3/SDL.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define LOG(...) SDL_Log(__VA_ARGS__)

// Unrecoverable error: log with location and exit. Used for programmer errors and for
// resources the scene cannot run without (GPU device, pipelines).
[[noreturn]] void fatal_at(const char *file, int line, const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(3);
#define FATAL(...) fatal_at(__FILE__, __LINE__, __VA_ARGS__)

// Always-on check (not compiled out in release): the port relies on these to fail loudly.
#define CHECK(cond) do { if (!(cond)) FATAL("check failed: %s", #cond); } while (0)
// SDL call returning bool: on failure, report SDL_GetError().
#define SDL_CHECK(call) do { if (!(call)) FATAL("%s failed: %s", #call, SDL_GetError()); } while (0)

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
