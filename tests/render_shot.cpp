// Murks 1989 — draws the map geometry to an image without Godot, and times the chunk builds.
//
//   build/render_shot game/data/poland_1989.murkz shot.ppm <col> <row> <px per hex> [width height]
//   build/render_shot game/data/poland_1989.murkz bench
//
// The picture is what the Godot layer shows: the chunk mesh (terrain, rivers, roads, rail),
// and from 16 px per hex the close-zoom detail layer on top. Used to check map styling
// (work board MRK-24) and that chunk build times stay inside the frame budget.
#include "murks/render_data.hpp"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace murks;

static bool load_murkz(const char *path, std::vector<uint8_t> &raw) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> z((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (z.size() < 16 || std::memcmp(z.data(), "MURKSZ01", 8) != 0) return false;
    uint64_t n = 0;
    std::memcpy(&n, z.data() + 8, 8);
    raw.resize(n);
    uLongf len = (uLongf)n;
    return uncompress(raw.data(), &len, z.data() + 16, (uLong)(z.size() - 16)) == Z_OK && len == n;
}

struct Canvas {
    int w = 0, h = 0;
    std::vector<float> px; // rgb
    double ox = 0, oy = 0, scale = 1; // world -> pixel
    void tri(const ChunkGeometry &g, int i0, int i1, int i2) {
        double x[3], y[3];
        const int id[3] = {i0, i1, i2};
        for (int k = 0; k < 3; ++k) {
            x[k] = (g.origin_x + g.xy[id[k] * 2] - ox) * scale;
            y[k] = (g.origin_y + g.xy[id[k] * 2 + 1] - oy) * scale;
        }
        int xa = std::max(0, (int)std::floor(std::min({x[0], x[1], x[2]}))), xb = std::min(w - 1, (int)std::ceil(std::max({x[0], x[1], x[2]})));
        int ya = std::max(0, (int)std::floor(std::min({y[0], y[1], y[2]}))), yb = std::min(h - 1, (int)std::ceil(std::max({y[0], y[1], y[2]})));
        if (xa > xb || ya > yb) return;
        double area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
        if (std::fabs(area) < 1e-9) return;
        uint32_t c = g.rgba[i0];
        float r = (float)(c & 255), gg = (float)((c >> 8) & 255), b = (float)((c >> 16) & 255), a = (float)((c >> 24) & 255) / 255.0f;
        for (int py = ya; py <= yb; ++py) {
            for (int pxl = xa; pxl <= xb; ++pxl) {
                double sx = pxl + 0.5, sy = py + 0.5;
                double w0 = ((x[1] - sx) * (y[2] - sy) - (x[2] - sx) * (y[1] - sy)) / area;
                double w1 = ((x[2] - sx) * (y[0] - sy) - (x[0] - sx) * (y[2] - sy)) / area;
                double w2 = 1.0 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float *p = &px[((size_t)py * w + pxl) * 3];
                p[0] += (r - p[0]) * a;
                p[1] += (gg - p[1]) * a;
                p[2] += (b - p[2]) * a;
            }
        }
    }
    void draw(const ChunkGeometry &g) {
        for (size_t i = 0; i + 2 < g.indices.size(); i += 3) tri(g, g.indices[i], g.indices[i + 1], g.indices[i + 2]);
    }
};

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: render_shot map.murkz (bench | out.ppm col row px_per_hex [w h])\n");
        return 2;
    }
    std::vector<uint8_t> raw;
    Map map;
    if (!load_murkz(argv[1], raw) || !map.load_from_memory(raw.data(), raw.size())) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    using clk = std::chrono::steady_clock;
    if (std::string(argv[2]) == "bench") {
        ChunkGeometry g;
        double base_ms = 0, det_ms = 0, det_worst = 0;
        size_t base_v = 0, det_v = 0, det_v_worst = 0;
        int n = 0;
        for (int cy = 0; cy < map.chunks_y(); cy += 2) {
            for (int cx = 0; cx < map.chunks_x(); cx += 2, ++n) {
                auto t0 = clk::now();
                build_chunk_geometry(map, cx, cy, false, g);
                auto t1 = clk::now();
                base_v += g.xy.size() / 2;
                build_chunk_detail(map, cx, cy, g);
                auto t2 = clk::now();
                double d = std::chrono::duration<double, std::milli>(t2 - t1).count();
                base_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
                det_ms += d;
                det_worst = std::max(det_worst, d);
                det_v += g.xy.size() / 2;
                det_v_worst = std::max(det_v_worst, g.xy.size() / 2);
            }
        }
        std::printf("%d chunks: chunk mesh %.2f ms avg (%zu vertices avg); detail %.2f ms avg, %.2f ms worst (%zu vertices avg, %zu worst)\n",
                    n, base_ms / n, base_v / n, det_ms / n, det_worst, det_v / n, det_v_worst);
        return 0;
    }
    if (argc < 6) return 2;
    const int col = std::atoi(argv[3]), row = std::atoi(argv[4]);
    const double pxhex = std::atof(argv[5]);
    Canvas cv;
    cv.w = argc > 6 ? std::atoi(argv[6]) : 1280;
    cv.h = argc > 7 ? std::atoi(argv[7]) : 800;
    cv.scale = pxhex / HEX_WIDTH;
    Vec2 ctr = hex_center({col, row});
    cv.ox = ctr.x - cv.w * 0.5 / cv.scale;
    cv.oy = ctr.y - cv.h * 0.5 / cv.scale;
    cv.px.assign((size_t)cv.w * cv.h * 3, 20.0f);
    const double cw = CHUNK * HEX_WIDTH, ch = CHUNK * ROW_SPACING;
    int cx0 = std::max(0, (int)std::floor(cv.ox / cw) - 1), cx1 = std::min(map.chunks_x() - 1, (int)std::floor((cv.ox + cv.w / cv.scale) / cw) + 1);
    int cy0 = std::max(0, (int)std::floor(cv.oy / ch) - 1), cy1 = std::min(map.chunks_y() - 1, (int)std::floor((cv.oy + cv.h / cv.scale) / ch) + 1);
    ChunkGeometry g;
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1 && pxhex < 16.0) break;
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx) {
                if (pass == 0) build_chunk_geometry(map, cx, cy, false, g);
                else build_chunk_detail(map, cx, cy, g);
                cv.draw(g);
            }
    }
    std::ofstream out(argv[2], std::ios::binary);
    out << "P6\n" << cv.w << " " << cv.h << "\n255\n";
    std::vector<uint8_t> row8((size_t)cv.w * 3);
    for (int y = 0; y < cv.h; ++y) {
        for (int i = 0; i < cv.w * 3; ++i) row8[i] = (uint8_t)std::min(255.0f, std::max(0.0f, cv.px[(size_t)y * cv.w * 3 + i]));
        out.write((const char *)row8.data(), (std::streamsize)row8.size());
    }
    std::printf("wrote %s (%d x %d, %.0f px per hex, centre %d,%d)\n", argv[2], cv.w, cv.h, pxhex, col, row);
    return 0;
}
