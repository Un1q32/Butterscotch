#pragma once

#define LIKELY(cond)   __builtin_expect(!!(cond), 1)
#define UNLIKELY(cond) __builtin_expect(!!(cond), 0)

#define UNUSED __attribute__ ((unused))
#define FORCE_INLINE static inline __attribute__((always_inline))

