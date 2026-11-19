# SPDX-License-Identifier: GPL-3.0-or-later
"""Selected extraction contracts using real image metadata and file data."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def check_selected_extraction(root, run, mkdwarfs, cli, config, env):
    source = root / "selected-source"
    source.mkdir()
    for name in ("a", "a/empty", "a/nested", "a/nested/empty", "a2", "empty"):
        (source / name).mkdir()
    content = {"a/file": b"selected", "a/nested/file": b"nested",
               "a2/file": b"unselected", "hard-first": b"hardlinked",
               "i:literal": b"case sensitive", "I:literal": b"different",
               ":literal": b"colon", "literal[*?{}\\]": b"metacharacters",
               "literalX": b"decoy", "a/line\nbreak": b"newline"}
    for name, data in content.items():
        (source / name).write_bytes(data)
    os.link(source / "hard-first", source / "hard-second")
    outside = root / "selected-outside"
    outside.mkdir()
    (outside / "victim").write_bytes(b"keep")
    os.symlink(str(outside), source / "external")
    os.symlink("a", source / "internal")
    image = root / "selected.dwarfs"
    subprocess.run([mkdwarfs, "-i", str(source), "-o", str(image), "-l", "1"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    serial = 0

    def extract(paths):
        nonlocal serial
        serial += 1
        dest = root / f"selected-output-{serial}"
        args = ["extract", image, dest]
        for path in paths:
            args += ["--path", path]
        details = run(*args)["details"]
        assert not list(root.glob(".dwarfsctl-extract-*"))
        return dest, details

    def inventory(dest):
        return {str(p.relative_to(dest)) for p in dest.rglob("*")}

    for name in ("i:literal", "I:literal", ":literal", "literal[*?{}\\]"):
        dest, _ = extract(["/" + name])
        assert inventory(dest) == {name}
        assert (dest / name).read_bytes() == content[name]
    dest, details = extract(["/a/nested/file", "/a/", "/a", "/a/empty"])
    assert details["selected_paths"] == ["/a"]
    assert inventory(dest) == {"a", "a/file", "a/empty", "a/nested",
                               "a/nested/file", "a/nested/empty", "a/line\nbreak"}
    assert (dest / "a/file").read_bytes() == content["a/file"]
    dest, _ = extract(["/empty"])
    assert inventory(dest) == {"empty"}
    dest, _ = extract(["/a/nested/empty"])
    assert inventory(dest) == {"a", "a/nested", "a/nested/empty"}
    dest, _ = extract(["/a/nested/file"])
    assert inventory(dest) == {"a", "a/nested", "a/nested/file"}
    dest, _ = extract(["/external"])
    assert inventory(dest) == {"external"}
    assert (dest / "external").is_symlink()
    assert os.readlink(dest / "external") == str(outside)
    assert (outside / "victim").read_bytes() == b"keep"
    dest, _ = extract(["/hard-first", "/hard-second"])
    assert inventory(dest) == {"hard-first", "hard-second"}
    assert (dest / "hard-first").stat().st_ino == (dest / "hard-second").stat().st_ino
    # Either alias alone must materialize its own data, regardless of inode order.
    for name in ("hard-first", "hard-second"):
        dest, _ = extract(["/" + name])
        assert inventory(dest) == {name}
        assert (dest / name).read_bytes() == b"hardlinked"
    dest, _ = extract(["/"])
    assert (dest / "a/empty").is_dir() and (dest / "empty").is_dir()
    for paths in (["/missing"], ["/a", "/a/missing"], ["/external/victim"],
                  ["/internal/file"], ["/a/../a/file"], ["/a/./file"], ["a/file"],
                  ["/a//file"], ["/a//"], [""]):
        serial += 1
        dest = root / f"selected-invalid-{serial}"
        args = ["extract", image, dest]
        for path in paths:
            args += ["--path", path]
        run(*args, ok=False)
        assert not dest.exists()
        assert not list(root.glob(".dwarfsctl-extract-*"))
    run("extract", image, root / "mixed", "--path", "/a", "--arg", "--help", ok=False)
    existing = root / "selected-existing"
    existing.mkdir()
    (existing / "keep").write_bytes(b"keep")
    run("extract", image, existing, "--path", "/a", ok=False)
    assert (existing / "keep").read_bytes() == b"keep"

    # Deterministic protocol-boundary races verify atomic publication and unwind.
    fake = root / "selected-fake-reader"
    fake_env = dict(env, DWARFSCTL_ENGINE=str(fake))
    raced = root / "selected-raced"
    fake.write_text(f"#!{sys.executable}\nimport sys\nfrom pathlib import Path\n"
                    f"Path({str(raced)!r}).mkdir()\n"
                    f"Path({str(raced / 'keep')!r}).write_bytes(b'concurrent')\n"
                    "Path(sys.argv[3], 'partial').write_bytes(b'partial')\n")
    fake.chmod(0o700)
    run("extract", image, raced, "--path", "/a", ok=False, custom_env=fake_env)
    assert (raced / "keep").read_bytes() == b"concurrent"
    assert not (raced / "partial").exists()
    assert not list(root.glob(".dwarfsctl-extract-*"))
    failed = root / "selected-failed"
    fake.write_text(f"#!{sys.executable}\nimport sys\nfrom pathlib import Path\n"
                    "Path(sys.argv[3], 'partial').write_bytes(b'partial')\nsys.exit(9)\n")
    run("extract", image, failed, "--path", "/a", ok=False, custom_env=fake_env)
    assert not failed.exists()
    assert not list(root.glob(".dwarfsctl-extract-*"))
    ready = root / "selected-ready"
    cancelled = root / "selected-cancelled"
    fake.write_text(f"#!{sys.executable}\nimport sys, time\nfrom pathlib import Path\n"
                    "Path(sys.argv[3], 'partial').write_bytes(b'partial')\n"
                    f"Path({str(ready)!r}).touch()\ntime.sleep(30)\n")
    child = subprocess.Popen([cli, "--config", str(config), "extract", str(image),
                              str(cancelled), "--path", "/a"],
                             env=fake_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 10
        while not ready.exists() and time.monotonic() < deadline:
            assert child.poll() is None
            time.sleep(0.02)
        assert ready.exists(), "extraction helper never became ready"
        child.send_signal(signal.SIGTERM)
        stdout, stderr = child.communicate(timeout=10)
        assert child.returncode != 0 and json.loads(stdout)["ok"] is False, (stdout, stderr)
    finally:
        if child.poll() is None:
            child.kill()
            child.communicate()
    assert not cancelled.exists()
    assert not list(root.glob(".dwarfsctl-extract-*"))
