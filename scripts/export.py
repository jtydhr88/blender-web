#!/usr/bin/env python3
"""Regenerate overlay/ and patches/ from a Blender development tree.

The development tree (a full Blender checkout with the feature branch checked
out) is the source of truth for everything that compiles. After working there,
run this to sync the patch-set repo:

    python scripts/export.py --blender H:\\blender

It diffs the tree's HEAD against the commit in UPSTREAM_COMMIT, copies added
files into overlay/ and writes modifications to patches/blender-web.patch.
Review `git diff` here afterwards and commit.

Files that live only in this repo (README, workflows, setup/export scripts,
comfyui/) are never touched. Paths listed in EXCLUDE are ignored even when
present in the development tree.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Present in the development tree but not part of the Blender overlay.
EXCLUDE = (
    "ComfyUI-BlenderWeb/",   # source of truth: comfyui/ in this repo
    "WEB_DISPLAY.md",        # superseded by README.md in this repo
    "web-input-detector",    # tools/ in this repo
)


def git(tree, *args, capture=True):
    result = subprocess.run(["git", "-C", str(tree), *args],
                            capture_output=capture, text=True, check=True)
    return result.stdout if capture else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blender", required=True,
                        help="path to the Blender development tree (feature branch checked out)")
    args = parser.parse_args()
    tree = pathlib.Path(args.blender).resolve()

    upstream = (ROOT / "UPSTREAM_COMMIT").read_text().strip()
    if not git(tree, "rev-parse", "--verify", "--quiet", f"{upstream}^{{commit}}").strip():
        sys.exit(f"error: upstream commit {upstream[:12]} not found in {tree}")

    dirty = git(tree, "status", "--porcelain").strip()
    if dirty:
        sys.exit("error: development tree has uncommitted changes, commit them first:\n" + dirty)

    entries = []
    for line in git(tree, "diff", "--name-status", f"{upstream}..HEAD").splitlines():
        status, path = line.split("\t", 1)
        if any(path.startswith(e) for e in EXCLUDE):
            continue
        entries.append((status, path))

    added = [p for s, p in entries if s == "A"]
    modified = [p for s, p in entries if s == "M"]
    other = [(s, p) for s, p in entries if s not in ("A", "M")]
    if other:
        sys.exit(f"error: unhandled statuses (extend this script): {other}")

    # Overlay: rebuild from scratch so deletions in the dev tree propagate.
    overlay = ROOT / "overlay"
    if overlay.exists():
        shutil.rmtree(overlay)
    for path in added:
        dst = overlay / path
        dst.parent.mkdir(parents=True, exist_ok=True)
        blob = subprocess.run(["git", "-C", str(tree), "show", f"HEAD:{path}"],
                              capture_output=True, check=True)
        dst.write_bytes(blob.stdout)
    print(f"overlay: {len(added)} files")

    # Patch for modified upstream files.
    patch = subprocess.run(
        ["git", "-C", str(tree), "diff", upstream, "HEAD", "--", *modified],
        capture_output=True, check=True)
    patch_file = ROOT / "patches" / "blender-web.patch"
    patch_file.parent.mkdir(exist_ok=True)
    patch_file.write_bytes(patch.stdout)
    print(f"patch: {len(modified)} files, {len(patch.stdout)} bytes")

    print("\nDone. Review with `git diff` and commit.")


if __name__ == "__main__":
    main()
