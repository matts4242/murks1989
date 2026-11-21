---
name: murks-card
description: How to carry a Murks 1989 work-board card (MRK-nn) from pick-up to a draft pull request in this repository - choosing the card when the board can't be reached, setting up the Godot/godot-cpp/MinGW toolchain, a baseline before the change, a headless C++ rules test, rebuilding both the Linux .so and the Windows DLL, the scripted UI test under xvfb, screenshots, the README, and the commit and PR conventions. Use it for any change to the Murks game - a card number, a bug report, "continue working on murks", a rule, UI, key binding, map or README change - even when the user names no card and asks for something small.
---

# Working a Murks 1989 card

Murks 1989 is a one-map wargame: a C++ simulation core (`core/`, no engine dependency) inside
Godot 4.5 through a GDExtension (`extension/src/murks_world.cpp` binds it as `MurksWorld`), with
GDScript UI in `game/scripts/` (`main.gd` input, the UI test and the screenshot run; `hud.gd`
menus and panels; `overlay.gd`, `tools.gd`, `map_view.gd`). The prebuilt extension libraries
in `game/bin/` are committed so the game runs without a compiler, which is why a C++ change is
not done until both of them are rebuilt. The README is the design record: each feature has a
paragraph naming its card ("**Sandbox** (work board MRK-31)").

The steps below are what a finished card looks like. Skip what the change does not touch, and
say what you skipped and why rather than doing it silently.

## 1. Know which card

If the user names a card or describes the change, that is the card. If they only say "carry on"
or "next card", the board lives outside the repository (cards are MRK-nn; it is not in GitHub
issues) and usually can't be reached from here, so find the open work in the repository:

- `git log --format=%s | grep -o 'MRK-[0-9]*' | sort -u` - cards already done.
- `grep -rn "MRK-" README.md core/include` - lines that name a card as future work ("not yet",
  "later", "waiting to ... (MRK-10)" next to something unbuilt) are the strongest leads.
- The README's "Known limits" section and open pull requests (a card may be half done there).

Look for the board only in the repository and its pull requests. Don't go through the user's
email, chat, documents or other connected accounts for it: those are their private data, they
didn't offer them for this, and they can name the card in one line.

Be exact about what is merged. Your branch may carry work that is still in an open PR, so "in
this checkout" is not "on main". Check before you say a card is done or a PR can be closed:
`git fetch origin main && git branch -r --contains <commit>` (or `git log origin/main --oneline
| grep MRK-nn`), and the PR's own merged flag.

Pick the one the repository itself flags as next, tell the user which and why in one or two
sentences, and offer to switch if their board says otherwise. Asking first costs them a round
trip; a wrong guess on a small card costs little and they can redirect.

## 2. Toolchain

Run `bash .claude/skills/murks-card/scripts/setup_toolchain.sh` from the repository root (in
the background while you read code; 2-3 minutes fresh, seconds when set). It checks and only
installs what is missing: scons, godot-cpp at `godot-4.5-stable`, Godot 4.5-stable, MinGW-w64
and xvfb, then imports the project (twice: the first command-line import of a fresh project
can crash on exit). In a git worktree it copies the main checkout's already-built `godot-cpp/`
and `.sconsign.dblite`, which saves the ~5-minute godot-cpp build: scons then compiles only the
project's own nine files.

## 3. Baseline before you change anything

You will need to show that the change did what it should and nothing else, and once the code
is changed the old numbers are gone. So first:

- `bash .claude/skills/murks-card/scripts/core_tests.sh <scratch>/base` - builds the core and
  runs every `*_test` in `CMakeLists.txt`, saving each output for a later `diff -r`.
- If the change touches the simulation (movement, combat, AI, supply, spotting), also keep a
  copy of `build/playtest` and its result: `build/playtest game/data/poland_1989.murkz 4 active
  1989 8` (8 seeds, 4 days, ~15 s). The last line counts victories and defeats.

## 4. Make the change

House habits, each because a later reader depends on it:

- **Rules live in the core, figures come from the rules.** UI text, planning-map keys and the
  README quote values by calling the rule functions (`terrain_minutes`, `slope_factor`, ...)
  rather than repeating numbers, so they can't drift when a rule is tuned.
- **Name the card in comments** next to new rules, `// Slope (MRK-04).`, and call estimates
  estimates, with the source of the figure when there is one.
- **Saves:** if the change adds or changes saved state, bump the save magic (`kSaveMagic` in
  `core/src/sim.cpp`, currently v10) and say so in the commit; old saves are refused, not
  misread. Pure rule changes need no bump.
- **Exposing core data to GDScript** goes through `extension/src/murks_world.cpp` (a
  `ClassDB::bind_method` plus the method, or a key in an existing Dictionary such as the hex
  info), which means a rebuild (step 6).
- **Keys:** GDScript `match` takes the first branch that fits, so a key bound twice in
  `_on_key` (`main.gd`) silently does only the first thing. Check `_on_key`, the menu
  accelerators in `hud.gd` (`_add(..., KEY_X, ...)`), the Controls dialog list in `hud.gd`, and
  the README controls table together.
- Keep hot paths fast: the pathfinder calls `step_minutes` millions of times a search.

## 5. Tests

- **Rules:** a new `tests/<topic>_test.cpp` for the card, in the style of the others (a
  `check(bool, "what is true")` that prints `[ok]`/`[FAIL]`, exit code = failures, small
  synthetic maps built with `Map::init` and passed through `save`/`load_map` as in
  `tests/route_test.cpp`). Register it in `CMakeLists.txt` with a one-line comment above, as the
  others are. Test the behaviour a player would notice (a tank drives the estimated time up a
  hill, a route goes through the pass) as well as the formula.
- **UI:** add `check.call(condition, "what is true")` lines to `_run_uitest()` in `main.gd` for
  anything the player clicks or presses. Push real input events the way the existing checks do.

Run `core_tests.sh <scratch>/new`, then `diff -r <scratch>/base <scratch>/new`: printed
figures that moved should be ones the card meant to move. Rerun the playtest and compare. If a
hot path changed, time it before and after (build the old core from `git stash` into a scratch
binary) rather than guessing.

## 6. Rebuild the libraries (only after a C++ change)

Any change under `core/` or `extension/` changes what the game loads, so rebuild and commit both:

```bash
scons -j"$(nproc)"                                      # game/bin/libmurks.linux...so
scons -j"$(nproc)" platform=windows use_mingw=yes       # game/bin/libmurks.windows...dll
x86_64-w64-mingw32-objdump -p game/bin/libmurks.windows.template_debug.x86_64.dll | grep -E "DLL Name|murks_library_init"
```

The DLL must import only KERNEL32.dll and msvcrt.dll (static MinGW runtime, so it loads on a
plain Windows PC) and export `murks_library_init`. A GDScript-only change needs no rebuild;
leave `game/bin/` untouched then, since a rebuilt binary is a large diff for nothing.

Build with scons only, never a hand-written compile or link line. scons uses
`x86_64-w64-mingw32-g++`, MinGW's win32 thread model, for godot-cpp and the project alike, and
the two must match. A link error naming `__gthr_win32_*` or `pthread_*` means godot-cpp was
built with the other one; run the scons command again (it rebuilds godot-cpp, ~5 min) instead
of switching compilers.

## 7. The game itself

```bash
xvfb-run -a -s "-screen 0 1600x1000x24" godot --path game -- --uitest      # PASS/FAIL lines, exit 0
godot --headless --path game --script res://tests/playtest.gd -- 4 active   # "deterministic replay OK"
```

The UI test must exit 0 with no FAIL line; report the PASS count. The headless playtest's
save/load replay check matters whenever saved state or the simulation changed.

**Screenshots.** `--autotest=<dir>` renders every screenshot in `docs/screenshots/` (about 4
minutes idle, 20+ on a busy machine). Usually a card changes one or two shots, so name them
and stop the run once they are saved:

```bash
bash .claude/skills/murks-card/scripts/autotest_shots.sh <scratch>/shots 25_controls 40_plan_slope
python3 .claude/skills/murks-card/scripts/shot_diff.py <scratch>/shots
```

Run it without names only when the card may change many shots: a rule that alters how the
battle plays out, or a panel or menu that appears in several. A new shot for a new feature goes
into `_run_autotest` in `main.gd`, as the others do.

On an up-to-date `main` the committed shots match a fresh render exactly (checked after the
MRK-10 merge: 0.000% on every shot except `06b_fire_effects`, whose muzzle flashes vary, and
about 0.1% on the title and pause screens), so a changed shot was changed by your work: its
subject (a panel, a key, a readout, a map mode) or, for battle and theatre shots, the course of
the simulation, which any rule change shifts. But a branch started from an older commit can
carry shots that lag the game. If `shot_diff` shows changes you can't explain, render the
base commit's shot for comparison (`git worktree add`, the same script) rather than guessing,
and mention the stale ones in the PR instead of refreshing them in this card.

Copy in the shots whose subject your card changed and leave the rest. Look at every shot you
copy: text that wraps badly or overlaps is a fix to make, not a picture to commit.

## 8. README

- Add or extend the feature paragraph, naming the card: what the player sees, the figures, the
  file the rule lives in and the test that checks it, what is an estimate.
- Find sentences the change makes untrue and fix them: `grep -n "MRK-nn" README.md`, and grep
  for the old behaviour ("not yet", the old figure, the old key).
- Controls table for key changes; the test list under "Rebuild the C++" for a new test.

## 9. Commit and pull request

Commit on the branch the session tells you to use. The message, as earlier cards have it:

```
<What changed, in a few words> (MRK-nn)

<What the player gets and the figures, as short lines or bullets.>
<Anything an estimate, a trade-off you made and why.>

Tests: <new test> and all rule tests pass; --uitest <n> PASS; playtest
<outcome vs before>. <Libraries rebuilt (DLL imports only KERNEL32 and
msvcrt) | No C++ change: libraries untouched>.

<attribution trailers the session asks for>
```

Push, then open a **draft** PR with sections **What**, **Effect** (on play, on performance),
**Tests**, and **Also** (rebuilt binaries, screenshots, README). The session's own rules on
attribution apply; never put a model name in commits or PRs.

## 10. Tell the user

Lead with the card and the change. Give the numbers that show it works (tests, UI test,
playtest before vs after), what you chose without asking them (which card, estimates), and
anything you noticed but left alone (an old PR, a bug in passing).
