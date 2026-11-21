// Headless check of wages (work board MRK-07, first slice): the men on the rolls are paid
// WAGE_DM_PER_MAN_DAY each from the company's cash at 06:00, a company that cannot pay in full
// pays what it has, loses UNPAID_MORALE_HIT morale on every own non-militia unit and says so,
// the days of cash follow the bill, and save/load stays deterministic across a pay day.
// Exit code 0 = all passed. Usage: wages_test
#include "murks/sim.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace murks;

static int failures = 0;
static void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

static int count_events(World &w, const std::string &needle) {
    int n = 0;
    for (auto &e : w.drain_events())
        if (e.text.find(needle) != std::string::npos) ++n;
    return n;
}

// A scenario with no fighting in the way: the hostiles are gone, so the headcount and morale
// stay put and only the wages move the cash.
static World quiet_world() {
    World w;
    w.generate(1989, 24, 22);
    w.setup_scenario(0); // 06:00
    for (auto &u : w.units)
        if (u.side != SIDE_PLAYER) u.alive = false;
    w.tick();
    w.drain_events();
    return w;
}

// Ticks until the clock reads `minute_of_day` (the next time it does, not now).
static void advance_to(World &w, int minute_of_day) {
    do w.tick();
    while (w.time_min % 1440 != minute_of_day);
}

// Runs to the minute before the next pay day, so a test can look at the state just before it.
static void to_eve_of_pay_day(World &w) { advance_to(w, 6 * 60 - 1); }

int main() {
    std::printf("The wage bill:\n");
    {
        World w = quiet_world();
        check(w.stock.personnel > 100, "the company has men on the rolls");
        check(std::fabs(w.wage_bill() - w.stock.personnel * WAGE_DM_PER_MAN_DAY) < 1e-9, "the bill is the men times the daily wage");
        w.stock.cash = w.wage_bill() * 3.0;
        check(std::fabs(w.days_of_cash() - 3.0) < 1e-9, "days of cash = cash over the bill");
        w.stock.personnel = 0;
        check(w.days_of_cash() > 1e8 && w.wage_bill() == 0, "no men, no bill, no limit on the days");
        World f = quiet_world();
        std::printf("     %d men cost %.0f DM a day; the starting %.0f DM lasts %.1f days\n", f.stock.personnel, f.wage_bill(),
                    f.stock.cash, f.days_of_cash());
        check(f.days_of_cash() > 4.0, "the starting cash outlasts the four-day contract");
    }

    std::printf("Paying at 06:00:\n");
    {
        World w = quiet_world();
        to_eve_of_pay_day(w);
        double bill = w.wage_bill(), before = w.stock.cash;
        w.tick();
        check(w.time_min % 1440 == 6 * 60, "the pay tick is 06:00");
        check(bill > 0 && std::fabs((before - w.stock.cash) - bill) < 1e-6, "exactly the bill is paid");
        check(count_events(w, "cannot pay") == 0, "no complaint when it is paid");
        // Paid every day, not once. (The holdings' income comes in an hour later, so look at the pay tick only.)
        to_eve_of_pay_day(w);
        bill = w.wage_bill();
        before = w.stock.cash;
        w.tick();
        check(w.time_min % 1440 == 6 * 60 && std::fabs((before - w.stock.cash) - bill) < 1e-6, "and again the next day");
        // And not in between.
        before = w.stock.cash;
        w.tick();
        check(w.stock.cash == before, "nothing is paid at 06:01");
    }

    std::printf("Short of cash:\n");
    {
        World w = quiet_world();
        w.stock.cash = 100;
        to_eve_of_pay_day(w);
        w.stock.cash = 100; // the holdings' income in between would have added to it
        std::vector<float> morale;
        for (auto &u : w.units) morale.push_back(u.morale);
        const double bill = w.wage_bill();
        w.tick();
        check(bill > 100 && w.stock.cash == 0, "what there is goes, the rest is not owed");
        int own = 0, dropped = 0;
        for (size_t i = 0; i < w.units.size(); ++i) {
            const Unit &u = w.units[i];
            if (!u.alive || u.side != SIDE_PLAYER || u.militia) continue;
            ++own;
            if (std::fabs(u.morale - std::max(0.0f, morale[i] - UNPAID_MORALE_HIT)) < 0.2f) ++dropped; // a minute's rest is within 0.2
        }
        check(own > 0 && dropped == own, "every own unit loses the unpaid morale hit");
        check(count_events(w, "cannot pay") == 1, "and the company is told, once");
        // An empty purse the next day is reported again.
        to_eve_of_pay_day(w);
        w.stock.cash = 0;
        w.drain_events();
        w.tick();
        check(count_events(w, "cannot pay") == 1, "an empty purse is reported on the next pay day too");
    }
    {
        World w = quiet_world();
        to_eve_of_pay_day(w);
        w.stock.cash = w.wage_bill() + 1;
        w.drain_events();
        w.tick();
        check(w.stock.cash >= 0 && w.stock.cash < 2 && count_events(w, "cannot pay") == 0, "cash just covering the bill pays in full without complaint");
    }

    std::printf("Save and load across a pay day:\n");
    {
        World a = quiet_world();
        a.stock.cash = 5000; // short: the pay day is a morale event too
        for (int i = 0; i < 300; ++i) a.tick();
        const std::vector<uint8_t> bytes = a.save_state();
        World b = quiet_world();
        std::string err;
        check(b.load_state(bytes.data(), bytes.size(), &err), "a save loads");
        check(b.stock.cash == a.stock.cash, "the cash is saved");
        const int64_t t0 = a.time_min;
        for (int i = 0; i < 1500; ++i) {
            a.tick();
            b.tick();
        }
        check(b.time_min == a.time_min && a.time_min > t0 + 1400, "both ran past a pay day");
        check(a.save_state() == b.save_state(), "the replay after loading is byte-identical, wages included");
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
