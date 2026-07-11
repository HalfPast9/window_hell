// rng.h — xorshift64*, the single PRNG owned by sim (D6). Header-only,
// seeded once at sim_init, advanced only inside sim in deterministic order.
#ifndef RNG_H
#define RNG_H

#include <stdint.h>

typedef struct { uint64_t state; } Rng;

static inline void rng_seed(Rng* r, uint64_t seed) {
    r->state = seed ? seed : 0x9E3779B97F4A7C15ull;  // avoid the all-zero state
}

static inline uint64_t rng_next_u64(Rng* r) {
    uint64_t x = r->state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

// uniform in [lo, hi] inclusive
static inline int rng_range_i(Rng* r, int lo, int hi) {
    uint32_t span = (uint32_t)(hi - lo + 1);
    return lo + (int)(rng_next_u64(r) % span);
}

// uniform in [0, 1)
static inline float rng_float01(Rng* r) {
    return (float)(rng_next_u64(r) >> 40) / (float)(1u << 24);
}

#endif // RNG_H
