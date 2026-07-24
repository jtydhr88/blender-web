#!/usr/bin/env python3
"""Set up a Blender source tree with the Web Display backend.

Usage:
    python scripts/setup.py --blender <path-to-blender-source>

Copies the overlay files (new sources) into the tree and applies the patch set
(small modifications to upstream files). The tree should be at the commit
recorded in UPSTREAM_COMMIT; other nearby commits usually work since the patch
surface is tiny, but that is not what releases are tested against.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def run(args):
    print("+", " ".join(str(a) for a in args))
    subprocess.run([str(a) for a in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blender", required=True, help="path to a Blender source checkout")
    parser.add_argument("--force", action="store_true",
                        help="apply even when the checkout is not at UPSTREAM_COMMIT")
    args = parser.parse_args()

    blender = pathlib.Path(args.blender).resolve()
    if not (blender / "source" / "blender").is_dir():
        sys.exit(f"error: {blender} does not look like a Blender source tree")

    pinned = (ROOT / "UPSTREAM_COMMIT").read_text().strip()
    head = subprocess.run(["git", "-C", str(blender), "rev-parse", "HEAD"],
                          capture_output=True, text=True, check=True).stdout.strip()
    if head != pinned:
        message = (f"Blender tree is at {head[:12]}, "
                   f"this patch set is pinned to {pinned[:12]}")
        if not args.force:
            sys.exit(f"error: {message}\nUse --force to apply anyway.")
        print(f"warning: {message}")

    overlay = ROOT / "overlay"
    copied = 0
    for src in sorted(overlay.rglob("*")):
        if src.is_file():
            dst = blender / src.relative_to(overlay)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            copied += 1
    print(f"copied {copied} overlay files")

    for patch in sorted((ROOT / "patches").glob("*.patch")):
        run(["git", "-C", blender, "apply", "--check", patch])
        run(["git", "-C", blender, "apply", patch])
        print(f"applied {patch.name}")

    print("\nDone. Configure and build:")
    print('  cmake -B build -G "Visual Studio 17 2022" -A x64 '
          "-DWITH_GHOST_WEB=ON -DWITH_WEB_NVENC=ON")
    print("  cmake --build build --config Release --target INSTALL")


if __name__ == "__main__":
    main()
