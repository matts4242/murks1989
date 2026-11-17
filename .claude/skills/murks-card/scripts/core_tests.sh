#!/usr/bin/env bash
# Builds the engine-free core with CMake and runs every rules test registered in
# CMakeLists.txt (each add_executable named *_test; they exit non-zero on a failure). Saves
# each test's output to OUTDIR/<test>.txt so a later run can be diffed against it.
#
#   bash .claude/skills/murks-card/scripts/core_tests.sh OUTDIR
#   diff -r BASELINE_DIR OUTDIR     (what changed in the printed figures)
#
# Prints one line per test and exits non-zero if the build or any test fails.
set -u
cd "$(git rev-parse --show-toplevel)"
out="${1:?usage: core_tests.sh OUTDIR}"
mkdir -p "$out"
cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 2
if ! cmake --build build -j"$(nproc)" >"$out/_build.log" 2>&1; then
    grep -E "error" -A3 "$out/_build.log" | head -40
    echo "BUILD FAILED (log: $out/_build.log)"
    exit 2
fi
fail=0
for t in $(grep -oE 'add_executable\(([a-z_]+_test)' CMakeLists.txt | sed 's/add_executable(//'); do
    if ./build/"$t" >"$out/$t.txt" 2>&1; then
        echo "pass  $t"
    else
        echo "FAIL  $t   ($(grep -c '\[FAIL\]' "$out/$t.txt") failed checks; see $out/$t.txt)"
        fail=1
    fi
done
exit $fail
