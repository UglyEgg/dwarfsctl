#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Collect distro notices for headers embedded in the reader binary."""
import glob
from pathlib import Path
import shutil
import sys

stage = Path(sys.argv[1]) / "usr/share/licenses/dwarfsctl/headers"
packages = {
    "tomlplusplus": ("/usr/share/doc/libtomlplusplus-dev/copyright", "/usr/share/licenses/tomlplusplus-devel/*", "/usr/share/licenses/tomlplusplus/*"),
    "boost": ("/usr/share/doc/libboost*-dev/copyright", "/usr/share/licenses/boost-devel/*"),
    "json": ("/usr/share/doc/nlohmann-json3-dev/copyright", "/usr/share/licenses/json-devel/*"),
    "range-v3": ("/usr/share/doc/librange-v3-dev/copyright", "/usr/share/licenses/range-v3-devel/*"),
    "parallel-hashmap": ("/usr/share/doc/libparallel-hashmap-dev/copyright", "/usr/share/licenses/parallel-hashmap-devel/*"),
    "utf8cpp": ("/usr/share/doc/libutfcpp-dev/copyright", "/usr/share/licenses/utf8cpp-devel/*"),
}
for name, patterns in packages.items():
    paths = sorted({Path(p) for pattern in patterns for p in glob.glob(pattern) if Path(p).is_file()})
    if not paths:
        raise SystemExit(f"Missing {name} license notice; cannot stage reader")
    for path in paths:
        dest = stage / name / path.parent.name / path.name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, dest)
