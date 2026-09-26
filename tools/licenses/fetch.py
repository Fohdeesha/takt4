#!/usr/bin/env python3
"""Fetch the licence texts that are nowhere in the source or build trees, at pinned revisions.

Three kinds of thing are built into takt4 without their licences arriving with them:

- Skia, which rust-skia ships to Slint as a prebuilt archive with no notices in it, and the
  libraries Skia compiles into that archive for the features Slint asks for (skia-bindings'
  build_support/skia/config.rs, with Slint 1.18's features d3d, gl, jpeg, pdf — textlayout,
  which brought HarfBuzz and ICU, is no longer among them): expat, libjpeg-turbo, libpng,
  SPIRV-Cross, Wuffs and zlib. Each is fetched from the revision rust-skia's Skia fork pins
  in its DEPS file, at the tag skia-bindings records (`[package.metadata] skia`).
- Eigen, which RTNeural vendors as headers only, without its COPYING files: MPL-2.0.
- Rust's standard library, which is compiled into Slint's static library and so into
  takt4.exe: its two licence files, from rust-lang/rust at the version rust-toolchain.toml
  pins.

Everything lands in tools/licenses/, with sources.json saying where each file came from.
tools/third_party_notices.py reads them from there. Run this again when skia-bindings,
RTNeural or the Rust pin moves, and commit what it writes. Which libraries skia.lib really
carries is checked by looking for their symbols in it (expat's XML_ParserCreate, HarfBuzz's
hb_shape and so on): rust-skia publishes no list.

    python tools/licenses/fetch.py
"""

import base64
import json
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent

# The Skia tag rust-skia 0.153.3 builds (skia-bindings' Cargo.toml, [package.metadata] skia):
# the skia-bindings Slint 1.18.1's Cargo.lock names. It said m150-0.98.1, 0.99.0's, until the
# audit of 2026-09-25 (B3).
SKIA_TAG = "m153-0.101.2"
SKIA_RAW = f"https://raw.githubusercontent.com/rust-skia/skia/{SKIA_TAG}"

# DEPS path under third_party/externals -> the licence files to take from that repository.
SKIA_BUNDLED = {
    "expat": ["expat/COPYING"],
    "libjpeg-turbo": ["LICENSE.md", "README.ijg"],
    "libpng": ["LICENSE"],
    "spirv-cross": ["LICENSE"],
    "wuffs": ["LICENSE"],
    "zlib": ["LICENSE"],
}

MPL_2_0 = "https://www.mozilla.org/media/MPL/2.0/index.txt"

# Rust's standard library is MIT OR Apache-2.0; these are the two files it is published with.
RUST_FILES = ["LICENSE-MIT", "LICENSE-APACHE"]


def rust_version():
    """The toolchain rust-toolchain.toml pins, which is the standard library Slint links."""
    text = (HERE.parent.parent / "rust-toolchain.toml").read_text(encoding="utf-8")
    match = re.search(r'^channel\s*=\s*"([0-9.]+)"', text, re.MULTILINE)
    if not match:
        raise SystemExit("rust-toolchain.toml pins no numbered channel")
    return match.group(1)


def get(url):
    # A 503 or a 429 is worth a second try a few seconds on.
    for attempt in range(5):
        try:
            with urllib.request.urlopen(url, timeout=60) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            if error.code not in (429, 500, 502, 503) or attempt == 4:
                raise
            time.sleep(5 * (attempt + 1))
    raise AssertionError("unreachable")


def googlesource(repo, commit, path):
    """A file at a commit from a googlesource repository: its ?format=TEXT is base64."""
    return base64.b64decode(get(f"{repo}/+/{commit}/{path}?format=TEXT"))


def main():
    sources = {}
    (HERE / "skia").mkdir(exist_ok=True)
    # A file at a commit never changes, so one already fetched from the same repository at the
    # same commit is kept rather than fetched again. Skia's tag moves more often than the
    # libraries it pins, and googlesource can refuse every request for a while: it answered
    # 503 to all of them on 2026-09-26, when m150 to m153 moved none of the libraries.
    try:
        fetched = json.loads((HERE / "sources.json").read_text(encoding="utf-8"))["files"]
    except (OSError, ValueError, KeyError):
        fetched = {}

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
            source = f"{repo}/+/{commit}/{path}"
            if fetched.get(out) == source and (HERE / out).is_file():
                sources[out] = source
                print(f"{out:40} {repo}@{commit[:12]} (already fetched at that commit)")
                continue
            (HERE / out).write_bytes(googlesource(repo, commit, path))
            sources[out] = source
            print(f"{out:40} {repo}@{commit[:12]}")

    (HERE / "MPL-2.0.txt").write_bytes(get(MPL_2_0))
    sources["MPL-2.0.txt"] = MPL_2_0

    rust = rust_version()
    (HERE / "rust").mkdir(exist_ok=True)
    for name in RUST_FILES:
        url = f"https://raw.githubusercontent.com/rust-lang/rust/{rust}/{name}"
        out = f"rust/{name}.txt"
        (HERE / out).write_bytes(get(url))
        sources[out] = url
        print(f"{out:40} rust-lang/rust@{rust}")

    (HERE / "sources.json").write_text(
        json.dumps({"skia_tag": SKIA_TAG, "rust": rust, "files": sources}, indent=2) + "\n",
        encoding="utf-8", newline="\n")
    print(f"{len(sources)} files -> {HERE}")


if __name__ == "__main__":
    sys.exit(main())
