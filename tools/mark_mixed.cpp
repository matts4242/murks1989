// Marks mixed ground in a baked map: open ground and fields whose land cover has 20-45% trees
// (hedgerows, copses, scattered woods) become T_MIXED. The bake (tools/bake_region.py) already
// gives exactly those hexes a 6 m cover height, so the class is read from that, with no new
// land-cover data. Newer bakes classify mixed ground themselves; this converts a map baked
// before that, in place of a full re-bake.
//
// Usage: mark_mixed <in.murkz> <out.murkz>
// First checks that the map saves back byte for byte as it was read (so nothing else in the
// file can change), then converts and writes it, printing what changed.
#include "murks/map.hpp"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace murks;

static bool read_murkz(const char *path, std::vector<uint8_t> &raw) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> z((std::istreambuf_iterator<char>(f)), {});
    if (z.size() < 16 || std::memcmp(z.data(), "MURKSZ01", 8) != 0) return false;
    uint64_t n = 0;
    std::memcpy(&n, z.data() + 8, 8);
    raw.resize(n);
    uLongf len = (uLongf)n;
    return uncompress(raw.data(), &len, z.data() + 16, (uLong)(z.size() - 16)) == Z_OK && len == n;
}

static bool write_murkz(const char *path, const std::vector<uint8_t> &raw) {
    uLongf zn = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(zn);
    if (compress2(z.data(), &zn, raw.data(), (uLong)raw.size(), 9) != Z_OK) return false;
    std::ofstream f(path, std::ios::binary);
    const uint64_t n = raw.size();
    f.write("MURKSZ01", 8);
    f.write(reinterpret_cast<const char *>(&n), 8);
    f.write(reinterpret_cast<const char *>(z.data()), (std::streamsize)zn);
    return (bool)f;
}

static bool map_bytes(const Map &m, const std::string &tmp, std::vector<uint8_t> &out) {
    if (!m.save(tmp)) return false;
    std::ifstream f(tmp, std::ios::binary);
    out.assign((std::istreambuf_iterator<char>(f)), {});
    std::remove(tmp.c_str());
    return !out.empty();
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: mark_mixed <in.murkz> <out.murkz>\n");
        return 2;
    }
    std::vector<uint8_t> raw;
    if (!read_murkz(argv[1], raw)) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    Map m;
    if (!m.load_from_memory(raw.data(), raw.size())) {
        std::fprintf(stderr, "not a map: %s\n", argv[1]);
        return 1;
    }
    const std::string tmp = std::string(argv[2]) + ".tmp";
    std::vector<uint8_t> again;
    if (!map_bytes(m, tmp, again) || again != raw) {
        std::fprintf(stderr, "the map does not save back byte for byte (%zu vs %zu bytes): not converting\n", again.size(), raw.size());
        return 1;
    }
    long open_n = 0, fields_n = 0, already = 0;
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c) {
            Hex &h = m.at(c, r);
            if (h.terrain == T_MIXED) ++already;
            if (h.cover != 6 || (h.terrain != T_OPEN && h.terrain != T_FIELDS)) continue;
            (h.terrain == T_OPEN ? open_n : fields_n)++;
            h.terrain = T_MIXED;
        }
    std::vector<uint8_t> out;
    if (!map_bytes(m, tmp, out) || !write_murkz(argv[2], out)) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    std::printf("mixed ground: %ld hexes from open ground, %ld from fields (%ld already mixed); wrote %s\n", open_n, fields_n,
                already, argv[2]);
    return 0;
}
