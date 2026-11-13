// Murks 1989 — relief data for the topographic mode, baked from the 90 m elevation data
// (tools/bake_contours.py, work board MRK-23): a height raster the map layer draws as
// stepped height bands, height figures for the 100 m contours, and spot heights.
// Drawing data only: the simulation never reads it, and a map without the file simply
// has no topographic mode.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace murks {

struct ContourLabel {
    float x, y;   // chunk-local metres
    float angle;  // radians, direction of the contour there, turned to read left to right
    int level;    // metres
};

struct SpotHeight {
    float x, y;   // chunk-local metres
    int elev;     // metres
};

class ContourSet {
public:
    // Takes what it needs from the unpacked file. False (and empty) if it is not a relief file.
    bool load(const uint8_t *data, size_t size);
    void clear();
    bool loaded() const { return !heights_.empty(); }
    int chunks_x() const { return chunks_x_; }
    int chunks_y() const { return chunks_y_; }

    // Height raster: metres, row by row from the map's north-west corner. Pixel (i, j) has
    // its centre at ((j + 0.5) * pixel, (i + 0.5) * pixel) metres.
    int raster_w() const { return raster_w_; }
    int raster_h() const { return raster_h_; }
    float raster_pixel() const { return pixel_; }
    const std::vector<float> &heights() const { return heights_; }
    // The same raster as IEEE half floats (for a 16-bit float texture).
    void heights_half(std::vector<uint16_t> &out) const;

    // Height figures of a chunk: only levels that are multiples of `step` metres.
    void labels(int cx, int cy, int step, std::vector<ContourLabel> &out) const;
    void spots(int cx, int cy, std::vector<SpotHeight> &out) const;

private:
    struct Entry {
        uint32_t offset, labels, spots;
    };
    bool entry(int cx, int cy, Entry &e) const;
    std::vector<uint8_t> index_;   // header, chunk table and chunk blocks (not the raster)
    std::vector<float> heights_;
    int chunks_x_ = 0, chunks_y_ = 0, raster_w_ = 0, raster_h_ = 0;
    float pixel_ = 180.0f;
};

} // namespace murks
