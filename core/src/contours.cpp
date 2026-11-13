#include "murks/contours.hpp"

#include <cmath>
#include <cstring>

namespace murks {

namespace {

const size_t kHeader = 8 + 4 * 4 + 3 * 4 + 4;
const float kUnit = 4.0f; // metres per coordinate step of labels and spot heights

template <typename T> T rd(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

// IEEE 754 single to half precision, round to nearest; heights never overflow a half.
uint16_t to_half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t man = x & 0x7FFFFFu;
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        man |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t half = man >> shift;
        if ((man >> (shift - 1)) & 1u) ++half;
        return (uint16_t)(sign | half);
    }
    if (exp >= 31) return (uint16_t)(sign | 0x7C00u);
    uint32_t half = sign | ((uint32_t)exp << 10) | (man >> 13);
    if (man & 0x1000u) ++half;
    return (uint16_t)half;
}

} // namespace

void ContourSet::clear() {
    index_.clear();
    heights_.clear();
    chunks_x_ = chunks_y_ = raster_w_ = raster_h_ = 0;
}

bool ContourSet::load(const uint8_t *data, size_t size) {
    clear();
    if (!data || size < kHeader || std::memcmp(data, "MURKSCT2", 8) != 0) return false;
    const uint32_t cx = rd<uint32_t>(data + 8), cy = rd<uint32_t>(data + 12);
    const uint32_t w = rd<uint32_t>(data + 16), h = rd<uint32_t>(data + 20);
    const float pixel = rd<float>(data + 24), unit = rd<float>(data + 28), offset = rd<float>(data + 32);
    const uint32_t raster_at = rd<uint32_t>(data + 36);
    if (cx == 0 || cy == 0 || cx > 4096 || cy > 4096 || w < 2 || h < 2 || w > 32768 || h > 32768) return false;
    if (!(pixel > 0.0f) || !(unit > 0.0f)) return false;
    if (raster_at < kHeader + (size_t)cx * cy * 12 || (size_t)raster_at + (size_t)w * h * 2 > size) return false;

    // Undo the plane prediction: value = residual + left + up - upper-left (modulo 65536).
    std::vector<uint16_t> q((size_t)w * h);
    const uint8_t *src = data + raster_at;
    for (uint32_t i = 0; i < h; ++i) {
        for (uint32_t j = 0; j < w; ++j) {
            const size_t k = (size_t)i * w + j;
            uint16_t v = rd<uint16_t>(src + k * 2);
            if (j > 0) v = (uint16_t)(v + q[k - 1]);
            if (i > 0) v = (uint16_t)(v + q[k - w]);
            if (i > 0 && j > 0) v = (uint16_t)(v - q[k - w - 1]);
            q[k] = v;
        }
    }
    // To metres, with a light 3 x 3 blur: the file holds whole height steps, and without
    // it the band edges would follow those steps on nearly level ground.
    heights_.resize((size_t)w * h);
    for (uint32_t i = 0; i < h; ++i) {
        const uint32_t i0 = i > 0 ? i - 1 : 0, i1 = i + 1 < h ? i + 1 : i;
        for (uint32_t j = 0; j < w; ++j) {
            const uint32_t j0 = j > 0 ? j - 1 : 0, j1 = j + 1 < w ? j + 1 : j;
            float s = 4.0f * q[(size_t)i * w + j] +
                      2.0f * (q[(size_t)i * w + j0] + q[(size_t)i * w + j1] + q[(size_t)i0 * w + j] + q[(size_t)i1 * w + j]) +
                      q[(size_t)i0 * w + j0] + q[(size_t)i0 * w + j1] + q[(size_t)i1 * w + j0] + q[(size_t)i1 * w + j1];
            heights_[(size_t)i * w + j] = s / 16.0f * unit - offset;
        }
    }
    index_.assign(data, data + raster_at);
    chunks_x_ = (int)cx;
    chunks_y_ = (int)cy;
    raster_w_ = (int)w;
    raster_h_ = (int)h;
    pixel_ = pixel;
    return true;
}

void ContourSet::heights_half(std::vector<uint16_t> &out) const {
    out.resize(heights_.size());
    for (size_t i = 0; i < heights_.size(); ++i) out[i] = to_half(heights_[i]);
}

bool ContourSet::entry(int cx, int cy, Entry &e) const {
    if (index_.empty() || cx < 0 || cy < 0 || cx >= chunks_x_ || cy >= chunks_y_) return false;
    const uint8_t *p = index_.data() + kHeader + ((size_t)cy * chunks_x_ + cx) * 12;
    e.offset = rd<uint32_t>(p);
    e.labels = rd<uint32_t>(p + 4);
    e.spots = rd<uint32_t>(p + 8);
    return (size_t)e.offset + (size_t)e.labels * 8 + (size_t)e.spots * 6 <= index_.size();
}

void ContourSet::labels(int cx, int cy, int step, std::vector<ContourLabel> &out) const {
    out.clear();
    Entry e;
    if (!entry(cx, cy, e) || step <= 0) return;
    const uint8_t *p = index_.data() + e.offset;
    for (uint32_t i = 0; i < e.labels; ++i, p += 8) {
        const int level = rd<int16_t>(p + 4);
        if (level % step != 0) continue;
        out.push_back({rd<int16_t>(p) * kUnit, rd<int16_t>(p + 2) * kUnit, rd<int16_t>(p + 6) / 100.0f, level});
    }
}

void ContourSet::spots(int cx, int cy, std::vector<SpotHeight> &out) const {
    out.clear();
    Entry e;
    if (!entry(cx, cy, e)) return;
    const uint8_t *p = index_.data() + e.offset + (size_t)e.labels * 8;
    for (uint32_t i = 0; i < e.spots; ++i, p += 6)
        out.push_back({rd<int16_t>(p) * kUnit, rd<int16_t>(p + 2) * kUnit, rd<int16_t>(p + 4)});
}

} // namespace murks
