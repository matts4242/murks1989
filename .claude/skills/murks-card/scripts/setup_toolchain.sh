#!/usr/bin/env bash
# Gets a Linux container ready to build and test Murks 1989: scons, godot-cpp, Godot 4.5,
# MinGW (for the Windows DLL), xvfb, and a first Godot import. Safe to run again: every step
# checks first and skips what is already there. Run from the repository root.
#
#   bash .claude/skills/murks-card/scripts/setup_toolchain.sh        (about 2-3 min fresh)
#
# godot-cpp takes ~5 min of 4 cores to build the first time. If another checkout of this
# repository on the machine (the main clone of a worktree, say) already built it, this copies
# that godot-cpp/ and its .sconsign.dblite, so scons only compiles the project's own files.
set -u
cd "$(git rev-parse --show-toplevel)"
say() { printf '[setup] %s\n' "$*"; }

command -v cmake >/dev/null && command -v g++ >/dev/null || say "WARNING: cmake or g++ missing (apt-get install -y cmake g++)"

if ! command -v scons >/dev/null; then
    say "installing scons"; pip install -q scons || say "WARNING: pip install scons failed"
fi

if [ ! -f godot-cpp/SConstruct ]; then
    donor=""
    while read -r path _; do
        [ "$path" != "$PWD" ] && [ -f "$path/godot-cpp/SConstruct" ] && [ -f "$path/.sconsign.dblite" ] && { donor="$path"; break; }
    done < <(git worktree list)
    if [ -n "$donor" ]; then
        say "copying the built godot-cpp and .sconsign.dblite from $donor"
        cp -a "$donor/godot-cpp" . && cp -a "$donor/.sconsign.dblite" .
    else
        say "cloning godot-cpp (godot-4.5-stable)"
        git clone -q --depth 1 --branch godot-4.5-stable https://github.com/godotengine/godot-cpp.git godot-cpp
    fi
fi

if ! command -v godot >/dev/null; then
    say "downloading Godot 4.5-stable"
    mkdir -p /opt/godot && (cd /opt/godot &&
        curl -sSLo g.zip https://github.com/godotengine/godot/releases/download/4.5-stable/Godot_v4.5-stable_linux.x86_64.zip &&
        python3 -m zipfile -e g.zip . && chmod +x Godot_v4.5-stable_linux.x86_64 &&
        ln -sf /opt/godot/Godot_v4.5-stable_linux.x86_64 /usr/local/bin/godot) || say "WARNING: Godot download failed"
fi

if ! command -v x86_64-w64-mingw32-g++-posix >/dev/null || ! command -v xvfb-run >/dev/null; then
    say "installing mingw-w64 and xvfb"
    (apt-get install -y -qq mingw-w64 xvfb >/dev/null 2>&1 ||
        (apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq mingw-w64 xvfb >/dev/null 2>&1)) ||
        say "WARNING: apt-get install mingw-w64 xvfb failed"
fi

if [ ! -d game/.godot ] && command -v godot >/dev/null; then
    # The first command-line import of a fresh project can crash on exit with this
    # godot-cpp/Godot pairing; the second one works (README, Known limits).
    say "importing the Godot project"
    timeout 300 godot --headless --path game --import >/dev/null 2>&1
    timeout 300 godot --headless --path game --import >/dev/null 2>&1
fi

for t in cmake scons godot x86_64-w64-mingw32-g++-posix xvfb-run; do
    command -v "$t" >/dev/null && say "ok: $t" || say "MISSING: $t"
done
[ -f godot-cpp/SConstruct ] && say "ok: godot-cpp" || say "MISSING: godot-cpp"
