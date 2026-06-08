// Math compat for old bionic (Android API 9/10, NDK r10e).
//
// Several C99 math functions were only added to Android's libm in
// later API levels:
//
//   * log2 / log2f   — added in API 18.
//   * exp2 / exp2f   — added in API 18.
//   * trunc / truncf — present, but provide guards just in case.
//
// The Butterscotch VM exposes a GML `log2()` builtin (src/vm_builtins.c
// via GMLReal_log2 in src/real_type.h), so the symbol must resolve at
// link time. We provide portable definitions guarded so they never
// collide on platforms where libm already supplies them.
//
// Defined with default visibility so they satisfy references from every
// translation unit linked into libbutterscotch.so.

#include <math.h>

#ifndef M_LN2
#define M_LN2 0.69314718055994530942
#endif

// API 9 libm lacks log2/log2f/exp2/exp2f. We can't easily detect the
// API level at compile time from the .c, so we define our own under
// distinct names and alias via the preprocessor only when targeting the
// old platform. Simplest robust approach: provide them unconditionally
// under a guard macro set by the build (BS_NEED_MATH_COMPAT).

#ifdef BS_NEED_MATH_COMPAT

double log2(double x)  { return log(x) / (double) M_LN2; }
float  log2f(float x)  { return (float)(log((double) x) / (double) M_LN2); }
double exp2(double x)  { return pow(2.0, x); }
float  exp2f(float x)  { return (float) pow(2.0, (double) x); }

#endif // BS_NEED_MATH_COMPAT
