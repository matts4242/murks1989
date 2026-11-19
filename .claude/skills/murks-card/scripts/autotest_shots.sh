#!/usr/bin/env bash
# Renders the --autotest screenshots into OUTDIR. With shot names after OUTDIR, stops the run
# as soon as all of those are saved, which saves most of the run when a card touches one or
# two panels (the whole run takes ~4 min on an idle machine and 20+ when it is busy).
#
#   bash .claude/skills/murks-card/scripts/autotest_shots.sh OUTDIR                    # all shots
#   bash .claude/skills/murks-card/scripts/autotest_shots.sh OUTDIR 25_controls 40_plan_slope
#
# A new shot you add to _run_autotest in main.gd is named the same way (its file name without
# .png). Exit code: 0 when every named shot (or the whole run) is saved, 1 otherwise. Set
# XDG_DATA_HOME first if other runs share the machine, so quick saves don't collide.
set -u
cd "$(git rev-parse --show-toplevel)"
out="${1:?usage: autotest_shots.sh OUTDIR [SHOT_NAME ...]}"
shift
mkdir -p "$out"
log="$out/_autotest.log"
xvfb-run -a -s "-screen 0 1920x1080x24" godot --path game -- --autotest="$out" >"$log" 2>&1 &
pid=$!

have_all() {
    for n in "$@"; do [ -f "$out/$n.png" ] || return 1; done
    return 0
}

if [ $# -eq 0 ]; then
    wait "$pid"; rc=$?
    echo "autotest finished (exit $rc): $(ls "$out"/*.png 2>/dev/null | wc -l) shots in $out"
    exit $rc
fi

while kill -0 "$pid" 2>/dev/null; do
    if have_all "$@"; then
        sleep 2 # let the last PNG finish writing
        pkill -P "$pid" 2>/dev/null; kill "$pid" 2>/dev/null; pkill -f -- "--autotest=$out" 2>/dev/null
        echo "saved: $*  (run stopped early; log $log)"
        exit 0
    fi
    sleep 3
done
if have_all "$@"; then echo "saved: $*"; exit 0; fi
echo "autotest ended without: $(for n in "$@"; do [ -f "$out/$n.png" ] || printf '%s ' "$n"; done)(log $log)"
exit 1
