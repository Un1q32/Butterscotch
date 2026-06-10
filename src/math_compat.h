#pragma once

#include <math.h>
#include "real_type.h"

/* this header is for compatibility with systems that don't have full C99 math libraries */

#ifdef NO_FMIN

#undef GMLReal_fmin
static GMLReal GMLReal_fmin(GMLReal a, GMLReal b) {
    if (a != a) return b;
    if (b != b) return a;
    return a < b ? a : b;
}

#endif

#ifdef NO_FMAX

#undef GMLReal_fmax
static GMLReal GMLReal_fmax(GMLReal a, GMLReal b) {
    if (a != a) return b;
    if (b != b) return a;
    return a > b ? a : b;
}

#endif

#ifdef NO_ROUND

#undef GMLReal_round
static GMLReal GMLReal_round(GMLReal x) {
    if (x >= 9007199254740992.0 || x <= -9007199254740992.0) return x;
    if (x >= 0.0) return (GMLReal)((int64_t)(x + 0.5));
    else          return (GMLReal)((int64_t)(x - 0.5));
}

#endif

#ifdef NO_LROUND

static long lround(double x) {
    if (x >= 9007199254740992.0 || x <= -9007199254740992.0) return (long)x;
    if (x >= 0.0) return (long)((int64_t)(x + 0.5));
    else          return (long)((int64_t)(x - 0.5));
}

#endif

#ifdef NO_SQRTF

static float sqrtf(float x) {
    return sqrt(x);
}

#endif

#ifdef NO_FABSF

static float fabsf(float x) {
    return fabs(x);
}

#endif

#ifdef NO_FMODF

static float fmodf(float x, float y) {
    return fmod(x, y);
}

#endif
