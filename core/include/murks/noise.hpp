// Murks 1989 — deterministic random numbers and value noise.
#pragma once

#include <cmath>
#include <cstdint>

namespace murks {

// xorshift64* — small, fast, deterministic across platforms.
class Rng {
public:
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) { reseed(seed); }
    void reseed(uint64_t seed) { s_ = seed ? seed : 0x9E3779B97F4A7C15ull; next(); next(); }
    uint64_t next() {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 0x2545F4914F6CDD1Dull;
    }
    uint64_t state() const { return s_; }
    void set_state(uint64_t s) { s_ = s ? s : 0x9E3779B97F4A7C15ull; }
    double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); } // [0,1)
    double range(double lo, double hi) { return lo + (hi - lo) * uniform(); }
    int range_int(int lo, int hi_inclusive) { return lo + (int)(next() % (uint64_t)(hi_inclusive - lo + 1)); }
    bool chance(double p) { return uniform() < p; }

private:
    uint64_t s_;
};

inline uint32_t hash2(int x, int y, uint32_t seed) {
    uint32_t h = seed ^ ((uint32_t)x * 374761393u) ^ ((uint32_t)y * 668265263u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

inline double lattice(int x, int y, uint32_t seed) {
    return (hash2(x, y, seed) & 0xFFFFFFu) * (2.0 / 16777215.0) - 1.0;
}

inline double value_noise(double x, double y, uint32_t seed) {
    double fx0 = std::floor(x), fy0 = std::floor(y);
    int xi = (int)fx0, yi = (int)fy0;
    double fx = x - fx0, fy = y - fy0;
    double u = fx * fx * (3.0 - 2.0 * fx), v = fy * fy * (3.0 - 2.0 * fy);
    double a = lattice(xi, yi, seed), b = lattice(xi + 1, yi, seed);
    double c = lattice(xi, yi + 1, seed), d = lattice(xi + 1, yi + 1, seed);
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}

// Fractal noise, roughly in [-1, 1] with most values inside [-0.6, 0.6].
inline double fbm(double x, double y, uint32_t seed, int octaves) {
    double sum = 0, amp = 1, norm = 0, freq = 1;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * value_noise(x * freq, y * freq, seed + (uint32_t)i * 1013u);
        norm += amp;
        amp *= 0.5;
        freq *= 2.03;
    }
    return sum / norm;
}

inline double smoothstep(double e0, double e1, double x) {
    double t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t * t * (3 - 2 * t);
}

} // namespace murks
