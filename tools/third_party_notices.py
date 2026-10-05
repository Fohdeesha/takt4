#!/usr/bin/env python3
"""Write THIRD-PARTY-NOTICES.txt: the licences of everything built into takt4 that is not takt4's.

takt4 is GPLv3 (LICENSE). What is compiled into takt4.exe and takt4-cli.exe beside it keeps
its own terms, and most of those — BSD, MIT, Apache, MPL, the Unicode licence, zlib's — ask
for their notice to go wherever the binary goes (the audit's licensing finding). So this
collects every one of them, from where each already is:

- the C and C++ libraries takt4 builds: their licence files in third_party/ and in the build
  tree's FetchContent checkouts;
- the Rust crates inside Slint: every crate Slint links for Windows with the features takt4
  builds it with and no others (`cargo tree --no-default-features`, as the build runs cargo),
  and the licence files each published crate carries;
- Skia, which arrives prebuilt with no notices, the libraries compiled into it, Eigen, which
  arrives without its COPYING, and Rust's standard library, which is linked into Slint's
  static library: tools/licenses/, fetched at pinned revisions by tools/licenses/fetch.py.

The file is committed at the repository root, embedded in takt4.exe for the About box, and
attached to every release. Run this after any dependency moves, from a configured and built
full tree:

    python tools/third_party_notices.py [--build build/windows-msvc]
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LICENSES = ROOT / "tools" / "licenses"

# The features takt4's Slint build asks cargo for (Slint's api/cpp CMakeLists with the
# settings in cmake/deps.cmake). Checked against the build tree's own cargo command below.
SLINT_FEATURES = "backend-winit,renderer-skia,renderer-skia-opengl,renderer-software,accessibility,testing,std"
TARGET = "x86_64-pc-windows-msvc"

LICENSE_FILE = re.compile(r"(?i)^(licen[cs]e|copying|notice|unlicense|copyright)([-._].*)?$")


def component(name, url, licence, used_for, files, note=""):
    return {"name": name, "url": url, "licence": licence, "used_for": used_for, "files": files, "note": note}


def components(build):
    deps = build / "_deps"
    skia = LICENSES / "skia"
    return [
        component("PortAudio", "https://github.com/PortAudio/portaudio", "MIT", "audio input",
                  [ROOT / "third_party/portaudio/LICENSE.txt"]),
        component("Steinberg ASIO SDK", "https://www.steinberg.net/asiosdk",
                  "GPL-3.0 (the SDK's dual licence; takt4 takes the GPL option)", "the ASIO host API",
                  [ROOT / "third_party/asiosdk/LICENSE.txt"],
                  "ASIO is a trademark and software of Steinberg Media Technologies GmbH."),
        component("Ableton Link", "https://github.com/Ableton/link", "GPL-2.0-or-later", "tempo sync",
                  [ROOT / "third_party/link/LICENSE.md"]),
        component("asio (Christopher Kohlhoff), bundled by Link", "https://github.com/chriskohlhoff/asio",
                  "BSL-1.0", "networking for Link",
                  [ROOT / "third_party/link/modules/asio-standalone/asio/LICENSE_1_0.txt"]),
        component("r8brain-free-src", "https://github.com/avaneev/r8brain-free-src", "MIT", "resampling",
                  [deps / "r8brain-src/LICENSE"],
                  "Includes the FFT package by Takuya Ooura (fft4g), Copyright (C) 1996-2001 Takuya "
                  "OOURA, http://www.kurims.kyoto-u.ac.jp/~ooura/fft.html, modified by r8brain's "
                  "author as that package's licence permits: wrapped in a class, its tables made "
                  "once."),
        component("KissFFT", "https://github.com/mborgerding/kissfft", "BSD-3-Clause", "the STFT",
                  [deps / "kissfft-src/COPYING", deps / "kissfft-src/LICENSES/BSD-3-Clause"]),
        component("RTNeural", "https://github.com/jatinchowdhury18/RTNeural", "BSD-3-Clause",
                  "neural inference", [deps / "rtneural-src/LICENSE"]),
        component("Eigen, bundled by RTNeural", "https://eigen.tuxfamily.org", "MPL-2.0",
                  "RTNeural's maths", [LICENSES / "MPL-2.0.txt"],
                  "Eigen's source is available from https://gitlab.com/libeigen/eigen."),
        component("RtMidi", "https://github.com/thestk/rtmidi", "MIT-style", "MIDI",
                  [deps / "rtmidi-src/LICENSE"]),
        component("nlohmann/json", "https://github.com/nlohmann/json", "MIT", "settings and presets",
                  [deps / "nlohmann_json-src/LICENSE.MIT"]),
        component("pugixml", "https://pugixml.org", "MIT", "reading GDTF fixture definitions (their XML)",
                  [deps / "pugixml-src/LICENSE.md"]),
        component("miniz", "https://github.com/richgel999/miniz", "MIT",
                  "reading GDTF fixture definitions (the zip archive they come in)",
                  [deps / "miniz-src/LICENSE"]),
        component("BeatNet+", "https://github.com/mjhydri/BeatNet-Plus", "none stated upstream",
                  "the neural network: the built-in weights are its published model fine-tuned", [],
                  "The upstream project states no licence for its code or its trained models."),
        component("Slint", "https://slint.dev", "GPL-3.0-only (Slint's triple licence; takt4 takes the GPL option)",
                  "the user interface", [deps / "slint-src/LICENSE.md"]),
        # The typefaces the windows are set in (HANDOFF §0.5), embedded in the binary from
        # assets/fonts. The files are Google Fonts' own instances, fetched 2026-09-29 and — Chivo
        # Mono — 2026-09-30.
        component("Archivo", "https://github.com/Omnibus-Type/Archivo", "OFL-1.1",
                  "the windows' typeface (Medium, Bold and ExtraBold)",
                  [ROOT / "assets/fonts/archivo/OFL.txt"]),
        component("DM Mono", "https://github.com/googlefonts/dm-mono", "OFL-1.1",
                  "the main window's numbers and addresses (Medium)",
                  [ROOT / "assets/fonts/dm-mono/OFL.txt"]),
        component("Chivo Mono", "https://github.com/Omnibus-Type/Chivo", "OFL-1.1",
                  "the rule editor's numbers, addresses and codes (Medium)",
                  [ROOT / "assets/fonts/chivo-mono/OFL.txt"],
                  "Modified: takt4's copy has no 'liga' feature, so that \"ff\" in a colour code is "
                  "two letters; nothing else in the font is changed. Made by "
                  "tools/drop_font_feature.py from Google Fonts' static Medium instance."),
        component("Skia, prebuilt by rust-skia", "https://skia.org", "BSD-3-Clause", "drawing the user interface",
                  [skia / "skia-LICENSE.txt"]),
        component("expat, in Skia", "https://libexpat.github.io", "MIT", "", [skia / "expat-COPYING.txt"]),
        # HarfBuzz and ICU came in with Skia's textlayout, which Slint stopped enabling in 1.18.
        # Checked 2026-09-24: none of their symbols (hb_shape, hb_buffer_create, ubidi_open,
        # u_errorName) is in skia-bindings 0.153.3's skia.lib, where 0.99.0's archive had them.
        component("libjpeg-turbo, in Skia", "https://libjpeg-turbo.org", "IJG, BSD-3-Clause, zlib", "",
                  [skia / "libjpeg-turbo-LICENSE.md", skia / "libjpeg-turbo-README.ijg.txt"]),
        component("libpng, in Skia", "http://www.libpng.org", "libpng-2.0", "", [skia / "libpng-LICENSE.txt"]),
        component("SPIRV-Cross, in Skia", "https://github.com/KhronosGroup/SPIRV-Cross", "Apache-2.0", "",
                  [skia / "spirv-cross-LICENSE.txt"]),
        component("Wuffs, in Skia", "https://github.com/google/wuffs", "Apache-2.0 OR MIT", "",
                  [skia / "wuffs-LICENSE.txt"]),
        component("zlib, in Skia", "https://zlib.net", "Zlib", "", [skia / "zlib-LICENSE.txt"]),
        # Every Rust static library carries the parts of std it uses, and Slint is one. It was
        # missing from this list until the audit of 2026-09-25 (B3).
        component(f"The Rust standard library {rust_version()}", "https://www.rust-lang.org",
                  "MIT OR Apache-2.0", "compiled into Slint",
                  [LICENSES / "rust/LICENSE-MIT.txt", LICENSES / "rust/LICENSE-APACHE.txt"]),
    ]


def rust_version():
    """What rust-toolchain.toml pins, which tools/licenses/fetch.py fetched the texts for."""
    sources = json.loads((LICENSES / "sources.json").read_text(encoding="utf-8"))
    text = (ROOT / "rust-toolchain.toml").read_text(encoding="utf-8")
    match = re.search(r'^channel\s*=\s*"([0-9.]+)"', text, re.MULTILINE)
    if not match or sources.get("rust") != match.group(1):
        raise SystemExit("tools/licenses/ was not fetched for the Rust that rust-toolchain.toml pins: "
                         "run tools/licenses/fetch.py")
    return match.group(1)


def crates(build):
    """(name, version, licence, repository, [(file name, text)]) for every crate Slint links."""
    manifest = build / "_deps/slint-src/api/cpp/Cargo.toml"
    if not manifest.is_file():
        raise SystemExit(f"{manifest} not found: configure and build a full tree first")
    tree = subprocess.run(
        ["cargo", "tree", "--offline", "-e", "normal", "--target", TARGET, "--no-default-features",
         "--features", SLINT_FEATURES, "-p", "slint-cpp", "--prefix", "none", "-f", "{p}",
         "--manifest-path", str(manifest)],
        check=True, capture_output=True, text=True, encoding="utf-8").stdout
    wanted = set()
    for line in tree.splitlines():
        match = re.match(r"(\S+) v(\S+)", line.strip())
        if match:
            wanted.add((match.group(1), match.group(2)))
    meta = json.loads(subprocess.run(
        ["cargo", "metadata", "--offline", "--format-version", "1", "--filter-platform", TARGET,
         "--manifest-path", str(manifest)],
        check=True, capture_output=True, text=True, encoding="utf-8").stdout)
    found = []
    for package in meta["packages"]:
        if (package["name"], package["version"]) not in wanted:
            continue
        directory = Path(package["manifest_path"]).parent
        texts = []
        for entry in sorted(directory.iterdir()):
            if entry.is_file() and LICENSE_FILE.match(entry.name):
                texts.append((entry.name, entry.read_text(encoding="utf-8", errors="replace")))
        if package.get("license_file"):
            path = directory / package["license_file"]
            if path.is_file() and all(name != path.name for name, _ in texts):
                texts.append((path.name, path.read_text(encoding="utf-8", errors="replace")))
        found.append((package["name"], package["version"], package.get("license") or "see its files",
                      package.get("repository") or "", texts))
    missing = wanted - {(n, v) for n, v, *_ in found}
    if missing:
        raise SystemExit(f"cargo metadata lacks {sorted(missing)[:5]}")
    return sorted(found)


def check_features(build):
    """The build tree's cargo command has to ask for the features this lists, and no others."""
    for project in (build / "_deps/slint-build").glob("_cargo-build_slint_cpp.vcxproj"):
        text = project.read_text(encoding="utf-8", errors="replace")
        match = re.search(r"--features=([\w,-]+)", text)
        if match and match.group(1) != SLINT_FEATURES:
            raise SystemExit(f"the build asks cargo for {match.group(1)}, this lists {SLINT_FEATURES}")
        # Without it cargo tree also lists Slint's default features' crates — femtovg and four
        # more that are never built (the audit of 2026-09-25, B3).
        if match and "--no-default-features" not in text:
            raise SystemExit("the build asks cargo for Slint's default features too; cargo tree above "
                             "leaves them out")


def rule(title):
    return f"\n{'=' * 78}\n{title}\n{'=' * 78}\n\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", type=Path, default=ROOT / "build" / "windows-msvc")
    parser.add_argument("--out", type=Path, default=ROOT / "THIRD-PARTY-NOTICES.txt")
    args = parser.parse_args()
    check_features(args.build)

    parts = [
        "takt4 — third-party notices\n\n",
        "takt4 is free software under the GNU General Public License version 3; its text is\n"
        "LICENSE, beside this file in the source and in every release. You are entitled to\n"
        "takt4's complete source code under that licence.\n\n",
        "This file lists the software built into takt4.exe and takt4-cli.exe that is not\n"
        "takt4's own, with the licence each is used under and that licence's text. It is\n"
        "written by tools/third_party_notices.py; do not edit it by hand.\n\n",
        "ASIO is a trademark and software of Steinberg Media Technologies GmbH.\n",
    ]

    listed = components(args.build)
    rust = crates(args.build)
    parts.append(rule("Contents"))
    for item in listed:
        parts.append(f"  {item['name']} — {item['licence']}\n")
    parts.append(f"  and {len(rust)} Rust crates inside Slint, listed with their licences at the end\n")

    for item in listed:
        parts.append(rule(item["name"]))
        parts.append(f"{item['url']}\nLicence: {item['licence']}\n")
        if item["used_for"]:
            parts.append(f"Used for: {item['used_for']}\n")
        if item["note"]:
            parts.append(f"{item['note']}\n")
        for path in item["files"]:
            if not Path(path).is_file():
                raise SystemExit(f"{path} not found (build tree not configured, or tools/licenses/fetch.py not run)")
            parts.append(f"\n--- {Path(path).name} ---\n\n{Path(path).read_text(encoding='utf-8', errors='replace').strip()}\n")

    # The crates: each licence text once, followed by every crate it came with.
    parts.append(rule(f"Rust crates inside Slint ({len(rust)})"))
    parts.append("Each crate Slint links for Windows, with the licence its Cargo.toml states and the\n"
                 "licence files it is published with. Identical files are printed once, after the list\n"
                 "of crates they came with.\n\n")
    by_text = {}
    for name, version, licence, repository, texts in rust:
        where = f" ({repository})" if repository else ""
        parts.append(f"  {name} {version} — {licence}{where}\n")
        if not texts:
            parts.append("      (no licence file in the published crate; the standard text of its licence\n"
                         "       is among those below)\n")
        for file_name, text in texts:
            key = hashlib.sha256(text.strip().encode("utf-8")).hexdigest()
            by_text.setdefault(key, {"text": text.strip(), "crates": [], "name": file_name})
            by_text[key]["crates"].append(f"{name} {version}")
    for entry in by_text.values():
        crates_list = ", ".join(entry["crates"])
        parts.append(rule(f"{entry['name']} — for {crates_list}"[:600]))
        parts.append(entry["text"] + "\n")

    args.out.write_text("".join(parts), encoding="utf-8", newline="\n")
    print(f"{args.out}: {len(listed)} libraries, {len(rust)} crates, {len(by_text)} distinct crate licence files, "
          f"{args.out.stat().st_size // 1024} KB")


if __name__ == "__main__":
    sys.exit(main())
