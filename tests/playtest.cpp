// Headless playtest on the real map: C++ port of game/tests/playtest.gd, run over several seeds.
// The scripted player is the same as in playtest.gd:
//   passive  never gives orders
//   active   recon forward at 07:00 day 1 (deliberate, return fire), company road-marches after it
//            at 09:00, regroup on the HQ at 18:00 daily, rest & resupply at 06:30 from day 2
// Usage: playtest [map.murkz] [days] [mode] [first_seed] [seeds] [contract|three-way]
#include "murks/sim.hpp"

#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace murks;

static bool load_murkz(const char *path, std::vector<uint8_t> &raw) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> z((std::istreambuf_iterator<char>(f)), {});
    if (z.size() < 16 || std::memcmp(z.data(), "MURKSZ01", 8) != 0) return false;
    uint64_t n = 0;
    std::memcpy(&n, z.data() + 8, 8);
    raw.resize(n);
    uLongf len = (uLongf)n;
    return uncompress(raw.data(), &len, z.data() + 16, (uLong)(z.size() - 16)) == Z_OK && len == n;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "game/data/poland_1989.murkz";
    const int days = argc > 2 ? std::atoi(argv[2]) : 4;
    const std::string mode = argc > 3 ? argv[3] : "active";
    const uint32_t first = argc > 4 ? (uint32_t)std::atoi(argv[4]) : 1989;
    const int seeds = argc > 5 ? std::atoi(argv[5]) : 1;
    const int kind = argc > 6 && std::strcmp(argv[6], "three-way") == 0 ? SC_THREE_WAY : SC_CONTRACT;
    std::vector<uint8_t> raw;
    if (!load_murkz(path, raw)) {
        std::fprintf(stderr, "cannot read %s\n", path);
        return 2;
    }
    int wins = 0, defeats = 0;
    for (int si = 0; si < seeds; ++si) {
        World w;
        if (!w.load_map(raw.data(), raw.size())) return 2;
        w.seed = first + si;
        w.setup_scenario(24, kind);
        if (std::getenv("MURKS_NO_CIVILIANS")) w.civilians_on = false; // compare runs without civilians (MRK-34)
        std::vector<int> own;
        for (auto &u : w.units)
            if (u.side == SIDE_PLAYER) own.push_back(u.id);
        const HexCoord home = w.units[0].pos;
        std::string outcome = "none";
        int contact = 0, displaced = 0, deployed = 0, squad_min = 0, squads_max = 0;
        for (int m = 0; m < days * 1440; ++m) {
            if (mode == "active") {
                const int64_t tod = w.time_min % 1440, day = w.time_min / 1440;
                for (int id : own) {
                    const Unit &u = w.units[id];
                    if (!u.alive) continue;
                    if (tod == 7 * 60 && day == 0 && u.type == U_RECON) {
                        w.set_stance(id, ROE_RETURN, BC_LIGHT);
                        w.order_move(id, {home.col + 40, home.row}, MV_DELIBERATE);
                    }
                    if (tod == 9 * 60 && day == 0 && u.type != U_RECON) w.order_move(id, {home.col + 25, home.row}, MV_ROAD);
                    if (tod == 18 * 60) w.order_regroup(id, false);
                    if (tod == 6 * 60 + 30 && day >= 1) w.order_regroup(id, true);
                }
            }
            w.tick();
            if (w.player_in_contact()) ++contact;
            int out = 0;
            for (const auto &u : w.units)
                if (u.alive && u.carrier >= 0 && u.side != SIDE_PLAYER && w.formations[u.formation].hunts_player) ++out;
            if (out > 0) ++squad_min;
            squads_max = std::max(squads_max, out);
            for (auto &e : w.drain_events()) {
                if (e.text.find("displaces to a new firing") != std::string::npos) ++displaced;
                if (e.text.find("leaves road march") != std::string::npos) ++deployed;
            }
            if (outcome == "none" && w.outcome() != OUT_ONGOING) {
                outcome = w.time_string() + "  " + w.outcome_text();
                if (w.outcome() == OUT_VICTORY) ++wins;
                else ++defeats;
            }
        }
        int alive = 0;
        float str = 0;
        for (int id : own)
            if (w.units[id].alive) {
                ++alive;
                str += w.units[id].strength;
            }
        const int sf = w.strike_formation();
        std::printf("seed %u %s: %s | own alive %d (avg str %.0f), 7G fighting %d/%d, kills %d losses %d, "
                    "contact %d min, ammo %.1f t, displacements %d, deployments %d, 7G squads out %d min (max %d), "
                    "civilians %d, tick avg %.0f us max %.0f us\n",
                    w.seed, mode.c_str(), outcome.substr(0, 60).c_str(), alive, alive ? str / alive : 0.0f,
                    sf >= 0 ? w.effective_combat_units(sf) : -1, sf >= 0 ? w.formations[sf].start_combat : -1,
                    w.stats().kills, w.stats().losses, contact, w.stock.ammo_t, displaced, deployed, squad_min,
                    squads_max, w.stats().civilians,
                    w.stats().avg_tick_us, w.stats().max_tick_us);
        if (kind == SC_THREE_WAY) {
            const std::vector<int> hs = w.hunter_formations();
            const int rf = hs.size() > 1 ? hs[1] : -1;
            int hostile_dead = 0;
            for (const auto &u : w.units)
                if (!u.alive && !u.embarked && !u.removed && u.formation == sf) ++hostile_dead;
            std::printf("    three-way: FK fighting %d/%d, 7G units gone %d\n", rf >= 0 ? w.effective_combat_units(rf) : -1,
                        rf >= 0 ? w.formations[rf].start_combat : -1, hostile_dead);
        }
        std::fflush(stdout);
    }
    std::printf("%s over %d seeds: %d victories, %d defeats, %d undecided after %d days\n", mode.c_str(), seeds, wins,
                defeats, seeds - wins - defeats, days);
    return 0;
}
