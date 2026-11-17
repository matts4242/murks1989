#!/usr/bin/env python3
"""Compares a fresh --autotest screenshot run with the committed docs/screenshots.

    python3 .claude/skills/murks-card/scripts/shot_diff.py NEW_DIR [docs/screenshots]

Prints, for every committed screenshot, the share of pixels that changed noticeably and the
box they lie in, most-changed first. The autotest is deterministic apart from 06b_fire_effects
(muzzle flashes) and a stray pixel or two, so anything else that moved was moved by the change.
Exit code 0; it only reports.
"""
import os
import signal
import sys

signal.signal(signal.SIGPIPE, signal.SIG_DFL)  # quiet when piped into head

try:
    import numpy as np
    from PIL import Image, ImageChops
except ImportError:
    sys.exit("needs Pillow and numpy: pip install pillow numpy")


def main() -> None:
    new_dir = sys.argv[1]
    old_dir = sys.argv[2] if len(sys.argv) > 2 else "docs/screenshots"
    rows = []
    for name in sorted(os.listdir(old_dir)):
        if not name.endswith(".png"):
            continue
        new_path = os.path.join(new_dir, name)
        if not os.path.exists(new_path):
            rows.append((-1.0, name, "not in the new run"))
            continue
        a = Image.open(os.path.join(old_dir, name)).convert("RGB")
        b = Image.open(new_path).convert("RGB")
        if a.size != b.size:
            rows.append((1.0, name, f"size {a.size} -> {b.size}"))
            continue
        d = ImageChops.difference(a, b)
        share = float((np.asarray(d).sum(axis=2) > 30).mean())
        rows.append((share, name, f"box {d.getbbox()}"))
    new_only = sorted(set(f for f in os.listdir(new_dir) if f.endswith(".png")) - set(os.listdir(old_dir)))
    for share, name, note in sorted(rows, reverse=True):
        label = "missing" if share < 0 else f"{share * 100:7.3f}%"
        print(f"{label:>9}  {name:32} {note}")
    if new_only:
        print("not committed (the autotest makes them, docs does not keep them):", ", ".join(new_only))


if __name__ == "__main__":
    main()
