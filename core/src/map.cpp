#include "murks/map.hpp"

#include <cstdio>
#include <cstring>

namespace murks {

const char *terrain_name(uint8_t t) {
    switch (t) {
        case T_WATER: return "Water";
        case T_OPEN: return "Open ground";
        case T_FIELDS: return "Farmland";
        case T_FOREST: return "Forest";
        case T_WETLAND: return "Marsh";
        case T_URBAN: return "Built-up";
        case T_MIXED: return "Mixed (hedgerows, copses)";
        default: return "Unknown";
    }
}

void Map::init(int chunks_x, int chunks_y) {
    chunks_x_ = chunks_x;
    chunks_y_ = chunks_y;
    hexes_.assign((size_t)chunks_x * chunks_y * CHUNK_AREA, Hex{});
    settlements.clear();
}

int Map::nearest_settlement(HexCoord h, int max_dist) const {
    int best = -1, best_d = max_dist + 1;
    for (int i = 0; i < (int)settlements.size(); ++i) {
        int d = hex_distance(h, settlements[i].pos);
        // Bigger places win ties within a few km, so reports name real towns.
        if (d < best_d || (best >= 0 && d <= best_d + 4 && settlements[i].population > settlements[best].population * 4)) {
            best = i;
            best_d = d;
        }
    }
    return best;
}

void Map::derive_edges_from_flags() {
    for (int r = 0; r < rows(); ++r)
        for (int c = 0; c < cols(); ++c)
            for (int net = 0; net < 3; ++net) {
                if (!(at(c, r).features & kNetFlag[net])) continue;
                for (int d = 0; d < 3; ++d) {
                    HexCoord n = neighbor({c, r}, d);
                    if (in_bounds(n) && (at(n).features & kNetFlag[net])) link({c, r}, n, (Network)net);
                }
            }
}

// File layout (little-endian):
//   "MURKSMAP" magic, u32 version (3; 2 had no network edges; 1 had no georeference), i32 chunks_x, i32 chunks_y,
//   v2 only: i32 epsg, f64 origin_e, f64 origin_n, u16 name length, name bytes,
//   raw hexes in chunk-major order (8 bytes each),
//   u32 settlement count, then per settlement: i32 col, i32 row, i32 population,
//   i32 radius, u16 name length, UTF-8 name bytes.
bool Map::save(const std::string &path) const {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const char magic[8] = {'M', 'U', 'R', 'K', 'S', 'M', 'A', 'P'};
    uint32_t version = 3;
    std::fwrite(magic, 1, 8, f);
    std::fwrite(&version, 4, 1, f);
    std::fwrite(&chunks_x_, 4, 1, f);
    std::fwrite(&chunks_y_, 4, 1, f);
    int32_t e = epsg;
    std::fwrite(&e, 4, 1, f);
    std::fwrite(&geo_origin_e, 8, 1, f);
    std::fwrite(&geo_origin_n, 8, 1, f);
    uint16_t nlen = (uint16_t)name.size();
    std::fwrite(&nlen, 2, 1, f);
    std::fwrite(name.data(), 1, nlen, f);
    std::fwrite(hexes_.data(), sizeof(Hex), hexes_.size(), f);
    uint32_t n = (uint32_t)settlements.size();
    std::fwrite(&n, 4, 1, f);
    for (const auto &s : settlements) {
        int32_t v[4] = {s.pos.col, s.pos.row, s.population, s.radius};
        std::fwrite(v, 4, 4, f);
        uint16_t len = (uint16_t)s.name.size();
        std::fwrite(&len, 2, 1, f);
        std::fwrite(s.name.data(), 1, len, f);
    }
    std::fclose(f);
    return true;
}

bool Map::load(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> buf;
    uint8_t tmp[65536];
    size_t n;
    while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    std::fclose(f);
    return load_from_memory(buf.data(), buf.size());
}

namespace {
struct Reader {
    const uint8_t *p;
    size_t left;
    bool ok = true;
    bool read(void *dst, size_t n) {
        if (!ok || n > left) return ok = false;
        std::memcpy(dst, p, n);
        p += n;
        left -= n;
        return true;
    }
};
} // namespace

bool Map::load_from_memory(const uint8_t *data, size_t size) {
    Reader r{data, size};
    char magic[8];
    uint32_t version = 0;
    int32_t cx = 0, cy = 0;
    if (!r.read(magic, 8) || std::memcmp(magic, "MURKSMAP", 8) != 0) return false;
    if (!r.read(&version, 4) || version < 1 || version > 3) return false;
    if (!r.read(&cx, 4) || !r.read(&cy, 4) || cx <= 0 || cy <= 0 || cx > 4096 || cy > 4096) return false;
    int32_t e = 0;
    double oe = 0, on = 0;
    std::string nm = "Unnamed";
    if (version >= 2) {
        uint16_t nlen = 0;
        if (!r.read(&e, 4) || !r.read(&oe, 8) || !r.read(&on, 8) || !r.read(&nlen, 2)) return false;
        nm.resize(nlen);
        if (nlen && !r.read(&nm[0], nlen)) return false;
    }
    init(cx, cy);
    epsg = e;
    geo_origin_e = oe;
    geo_origin_n = on;
    name = nm;
    if (!r.read(hexes_.data(), hexes_.size() * sizeof(Hex))) return false;
    uint32_t count = 0;
    if (!r.read(&count, 4) || count > 0xFFFE) return false;
    settlements.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        int32_t v[4];
        uint16_t len = 0;
        if (!r.read(v, 16) || !r.read(&len, 2)) return false;
        Settlement s;
        s.pos = {v[0], v[1]};
        s.population = v[2];
        s.radius = v[3];
        s.name.resize(len);
        if (len && !r.read(&s.name[0], len)) return false;
        settlements.push_back(s);
    }
    if (r.ok && version < 3) derive_edges_from_flags();
    return r.ok;
}

} // namespace murks
