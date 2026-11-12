// Murks 1989 — contour lines and spot heights baked from the 90 m elevation data
// (tools/bake_contours.py, work board MRK-23). Drawing data only: the simulation never
// reads it, and a map without a contour file simply has no topographic mode.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace murks {

struct ContourLabel {
    float x, y;   // chunk-local metres
    float angle;  // radians, direction of the line there, turned to read left to right
    int level;    // metres
};

struct SpotHeight {
    float x, y;   // chunk-local metres
    int elev;     // metres
};

class ContourSet {
public:
    // Takes a copy of the unpacked file. False (and empty) if it is not a contour file.
    bool load(const uint8_t *data, size_t size);
    void clear();
    bool loaded() const { return !raw_.empty(); }
    int chunks_x() const { return chunks_x_; }
    int chunks_y() const { return chunks_y_; }
    int interval() const { return interval_; } // metres between baked lines

    // Line segments of one chunk as pairs of points (x0, y0, x1, y1, ...) in chunk-local
    // metres, with one colour per point. Only levels that are multiples of `step` are
    // returned; multiples of `index_step` are index contours and drawn stronger.
    // `part` picks which of them: 0 all, 1 only the intermediate lines, 2 only the index
    // lines (the game draws those twice, a pixel apart, to make them heavier).
    void build_lines(int cx, int cy, int step, int index_step, int part, std::vector<float> &xy,
                     std::vector<uint32_t> &rgba) const;
    // Height labels for the index contours of a chunk: one per line long enough to carry it.
    void labels(int cx, int cy, int index_step, std::vector<ContourLabel> &out) const;
    void spots(int cx, int cy, std::vector<SpotHeight> &out) const;

private:
    struct Entry {
        uint32_t offset, lines, spots;
    };
    bool entry(int cx, int cy, Entry &e) const;
    std::vector<uint8_t> raw_;
    int chunks_x_ = 0, chunks_y_ = 0, interval_ = 20;
};

} // namespace murks
