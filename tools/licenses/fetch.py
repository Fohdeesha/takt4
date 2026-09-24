#!/usr/bin/env python3
"""Fetch the licence texts that are nowhere in the source or build trees, at pinned revisions.

Two kinds of thing are built into takt4 without their licences arriving with them:

- Skia, which rust-skia ships to Slint as a prebuilt archive with no notices in it, and the
  libraries Skia compiles into that archive for the features Slint asks for (skia-bindings'
  build_support/skia/config.rs, with Slint's features d3d, gl, jpeg, pdf, textlayout):
  expat, HarfBuzz, ICU, libjpeg-turbo, libpng, SPIRV-Cross, Wuffs and zlib. Each is fetched
  from the revision rust-skia's Skia fork pins in its DEPS file, at the tag skia-bindings
  records (`[package.metadata] skia`).
- Eigen, which RTNeural vendors as headers only, without its COPYING files: MPL-2.0.

Everything lands in tools/licenses/, with sources.json saying where each file came from.
tools/third_party_notices.py reads them from there. Run this again when skia-bindings or
RTNeural moves, and commit what it writes.

    python tools/licenses/fetch.py
"""

import base64
import json
import re
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent

# The Skia tag rust-skia 0.99.0 builds (skia-bindings' Cargo.toml, [package.metadata] skia).
SKIA_TAG = "m150-0.98.1"
SKIA_RAW = f"https://raw.githubusercontent.com/rust-skia/skia/{SKIA_TAG}"

# DEPS path under third_party/externals -> the licence files to take from that repository.
SKIA_BUNDLED = {
    "expat": ["expat/COPYING"],
    "harfbuzz": ["COPYING"],
    "icu": ["LICENSE"],
    "libjpeg-turbo": ["LICENSE.md", "README.ijg"],
    "libpng": ["LICENSE"],
    "spirv-cross": ["LICENSE"],
    "wuffs": ["LICENSE"],
    "zlib": ["LICENSE"],
}

MPL_2_0 = "https://www.mozilla.org/media/MPL/2.0/index.txt"


def get(url):
    with urllib.request.urlopen(url, timeout=60) as response:
        return response.read()


def googlesource(repo, commit, path):
    """A file at a commit from a googlesource repository: its ?format=TEXT is base64."""
    return base64.b64decode(get(f"{repo}/+/{commit}/{path}?format=TEXT"))


def main():
    sources = {}
    (HERE / "skia").mkdir(exist_ok=True)

    (HERE / "skia" / "skia-LICENSE.txt").write_bytes(get(f"{SKIA_RAW}/LICENSE"))
    sources["skia/skia-LICENSE.txt"] = f"{SKIA_RAW}/LICENSE"

    deps = get(f"{SKIA_RAW}/DEPS").decode("utf-8")
    for name, files in SKIA_BUNDLED.items():
        match = re.search(rf'"third_party/externals/{re.escape(name)}"\s*:\s*"([^"@]+)@([0-9a-f]+)"', deps)
        if not match:
            raise SystemExit(f"{name} is not in Skia's DEPS at {SKIA_TAG}")
        repo, commit = match.group(1).removesuffix(".git"), match.group(2)
        for path in files:
            out = f"skia/{name}-{Path(path).name}"
            if not out.endswith(".txt") and not out.endswith(".md"):
                out += ".txt"
            (HERE / out).write_bytes(googlesource(repo, commit, path))
            sources[out] = f"{repo}/+/{commit}/{path}"
            print(f"{out:40} {repo}@{commit[:12]}")

    (HERE / "MPL-2.0.txt").write_bytes(get(MPL_2_0))
    sources["MPL-2.0.txt"] = MPL_2_0

    (HERE / "sources.json").write_text(json.dumps({"skia_tag": SKIA_TAG, "files": sources}, indent=2) + "\n",
                                       encoding="utf-8", newline="\n")
    print(f"{len(sources)} files -> {HERE}")


if __name__ == "__main__":
    sys.exit(main())
