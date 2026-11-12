#!/usr/bin/env python3
"""Ensure checkout timestamps cannot change the corresponding-source archive."""
from pathlib import Path
import os
import subprocess
import tarfile
import tempfile

root = Path(__file__).resolve().parents[1]
version = (root / "VERSION").read_text().strip()
archive_name = f"dwarfs-dolphin-{version}-source.tar.gz"
with tempfile.TemporaryDirectory(prefix="dwarfsctl-source-check-") as temporary:
    work = Path(temporary)
    environment = dict(os.environ, SOURCE_DATE_EPOCH="1700000000")
    subprocess.run(["bash", str(root / "packaging/source-release.sh"), str(work / "first")],
                   env=environment, check=True)
    first = work / "first" / archive_name
    with tarfile.open(first) as archive:
        assert all(member.mtime == 1700000000 and member.uid == 0 and member.gid == 0
                   for member in archive.getmembers())
        archive.extractall(work / "extracted", filter="data")
    extracted = work / "extracted" / f"dwarfs-dolphin-{version}"
    os.utime(extracted / "README.md", (1800000000, 1800000000))
    environment.pop("SOURCE_DATE_EPOCH")
    subprocess.run(["bash", str(extracted / "packaging/source-release.sh"), str(work / "second")],
                   env=environment, check=True)
    assert first.read_bytes() == (work / "second" / archive_name).read_bytes(), \
        "Source archives differ after extraction and checkout timestamp changes"
print("Corresponding-source archive reproducibility passed")
