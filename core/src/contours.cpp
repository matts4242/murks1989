#include "murks/contours.hpp"

#include <cmath>
#include <cstring>

namespace murks {

namespace {

const size_t kHeader = 8 + 16;
const float kUnit = 4.0f; // metres per coordinate step

template <typename T> T rd(const std::vector<uint8_t> &raw, size_t at) {
    T v;
    std::memcpy(&v, raw.data() + at, sizeof(T));
    return v;
}

// One line's points: zigzag varint steps, the first from the chunk origin. Returns false
// if the data runs out. `at` is left behind the line.
bool read_points(const std::vector<uint8_t> &raw, size_t &at, size_t n, std::vector<float> &pts) {
    pts.resize(n * 2);
    int64_t acc[2] = {0, 0};
    for (size_t k = 0; k < n * 2; ++k) {
        uint64_t v = 0;
        int shift = 0;
        for (;;) {
            if (at >= raw.size() || shift > 56) return false;
            uint8_t b = raw[at++];
            v |= (uint64_t)(b & 0x7F) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
        }
        acc[k & 1] += (int64_t)(v >> 1) ^ -(int64_t)(v & 1);
        pts[k] = (float)acc[k & 1] * kUnit;
    }
    return true;
}

inline uint32_t rgba8(int r, int g, int b, int a) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

} // namespace

void ContourSet::clear() {
    raw_.clear();
    chunks_x_ = chunks_y_ = 0;
}

bool ContourSet::load(const uint8_t *data, size_t size) {
    clear();
    if (!data || size < kHeader || std::memcmp(data, "MURKSCT1", 8) != 0) return false;
    uint32_t h[4];
    std::memcpy(h, data + 8, 16);
    if (h[0] == 0 || h[1] == 0 || h[0] > 4096 || h[1] > 4096 || h[2] == 0) return false;
    if (size < kHeader + (size_t)h[0] * h[1] * 12) return false;
    raw_.assign(data, data + size);
    chunks_x_ = (int)h[0];
    chunks_y_ = (int)h[1];
    interval_ = (int)h[2];
    return true;
}

bool ContourSet::entry(int cx, int cy, Entry &e) const {
    if (raw_.empty() || cx < 0 || cy < 0 || cx >= chunks_x_ || cy >= chunks_y_) return false;
    size_t at = kHeader + ((size_t)cy * chunks_x_ + cx) * 12;
    e.offset = rd<uint32_t>(raw_, at);
    e.lines = rd<uint32_t>(raw_, at + 4);
    e.spots = rd<uint32_t>(raw_, at + 8);
    return e.offset <= raw_.size();
}

void ContourSet::build_lines(int cx, int cy, int step, int index_step, int part, std::vector<float> &xy,
                             std::vector<uint32_t> &rgba) const {
    xy.clear();
    rgba.clear();
    Entry e;
    if (!entry(cx, cy, e)) return;
    // Brown, as on a printed map: index contours dark, the others lighter.
    const uint32_t minor = rgba8(134, 86, 46, 205), index = rgba8(104, 58, 26, 255);
    size_t at = e.offset;
    std::vector<float> pts;
    for (uint32_t i = 0; i < e.lines; ++i) {
        if (at + 4 > raw_.size()) return;
        int level = rd<int16_t>(raw_, at);
        size_t n = rd<uint16_t>(raw_, at + 2);
        at += 4;
        if (!read_points(raw_, at, n, pts)) return;
        if (step <= 0 || level % step != 0) continue;
        const bool is_index = index_step > 0 && level % index_step == 0;
        if ((part == 1 && is_index) || (part == 2 && !is_index)) continue;
        const uint32_t col = is_index ? index : minor;
        for (size_t k = 1; k < n; ++k) {
            xy.push_back(pts[k * 2 - 2]);
            xy.push_back(pts[k * 2 - 1]);
            xy.push_back(pts[k * 2]);
            xy.push_back(pts[k * 2 + 1]);
            rgba.push_back(col);
            rgba.push_back(col);
        }
    }
}

void ContourSet::labels(int cx, int cy, int index_step, std::vector<ContourLabel> &out) const {
    out.clear();
    Entry e;
    if (!entry(cx, cy, e) || index_step <= 0) return;
    const float kMinLength = 3000.0f; // a label needs a line long enough to be worth naming
    size_t at = e.offset;
    std::vector<float> pts;
    for (uint32_t i = 0; i < e.lines; ++i) {
        if (at + 4 > raw_.size()) return;
        int level = rd<int16_t>(raw_, at);
        size_t n = rd<uint16_t>(raw_, at + 2);
        at += 4;
        if (!read_points(raw_, at, n, pts)) return;
        if (level % index_step != 0 || n < 2) continue;
        float total = 0;
        for (size_t k = 1; k < n; ++k) total += std::hypot(pts[k * 2] - pts[k * 2 - 2], pts[k * 2 + 1] - pts[k * 2 - 1]);
        if (total < kMinLength) continue;
        // Walk to the middle of the line and take its direction there.
        float run = 0, half = total * 0.5f;
        for (size_t k = 1; k < n; ++k) {
            float ax = pts[k * 2 - 2], ay = pts[k * 2 - 1], bx = pts[k * 2], by = pts[k * 2 + 1];
            float seg = std::hypot(bx - ax, by - ay);
            if (run + seg >= half && seg > 0) {
                float t = (half - run) / seg;
                size_t f = k >= 2 ? k - 2 : 0, g = k + 1 < n ? k + 1 : n - 1;
                float ang = std::atan2(pts[g * 2 + 1] - pts[f * 2 + 1], pts[g * 2] - pts[f * 2]);
                if (ang > 1.5708f) ang -= 3.14159265f;
                if (ang < -1.5708f) ang += 3.14159265f;
                out.push_back({ax + (bx - ax) * t, ay + (by - ay) * t, ang, level});
                break;
            }
            run += seg;
        }
    }
}

void ContourSet::spots(int cx, int cy, std::vector<SpotHeight> &out) const {
    out.clear();
    Entry e;
    if (!entry(cx, cy, e) || e.spots == 0) return;
    size_t at = e.offset;
    std::vector<float> pts;
    for (uint32_t i = 0; i < e.lines; ++i) { // spot heights follow the chunk's lines
        if (at + 4 > raw_.size()) return;
        size_t n = rd<uint16_t>(raw_, at + 2);
        at += 4;
        if (!read_points(raw_, at, n, pts)) return;
    }
    for (uint32_t i = 0; i < e.spots; ++i) {
        if (at + 6 > raw_.size()) return;
        out.push_back({rd<int16_t>(raw_, at) * kUnit, rd<int16_t>(raw_, at + 2) * kUnit, rd<int16_t>(raw_, at + 4)});
        at += 6;
    }
}

} // namespace murks
