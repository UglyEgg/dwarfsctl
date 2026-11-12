#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real-image contract tests. All writes live in a disposable private tree."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

cli, reader, mkdwarfs, dwarfsck = map(lambda p: str(Path(p).resolve()), sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="dwarfsctl-reader-") as temporary:
    root = Path(temporary)
    src = root / "source"
    src.mkdir()
    (src / "sub").mkdir()
    content = {"hello.txt": b"hello\n", "empty": b"", " file *.txt ": b"literal wildcard\x00\xff",
               "sub/[abc].bin": bytes(range(256)) * 8193}
    for name, data in content.items():
        (src / name).write_bytes(data)
    for i in range(110):
        (src / f"entry-{i:03}.txt").write_text(f"{i}\n")
    os.symlink("hello.txt", src / "link")
    os.link(src / "hello.txt", src / "hardlink")
    image = root / "test.dwarfs"
    subprocess.run([mkdwarfs, "-i", str(src), "-o", str(image), "-l", "1"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # No DwarFS tools can be resolved by the CLI. A successful read must use the library.
    env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / "config"),
               XDG_RUNTIME_DIR=str(root / "runtime"), XDG_DATA_HOME=str(root / "data"),
               PATH="/nonexistent", DWARFSCTL_ENGINE=reader)
    config = root / "config.toml"
    config.write_text('[tools]\n'
                      'mkdwarfs = "/absent/mkdwarfs"\ndwarfs = "/absent/dwarfs"\n'
                      'dwarfsck = "/absent/dwarfsck"\ndwarfsextract = "/absent/dwarfsextract"\n')

    def run(*args, ok=True, custom_env=None):
        result = subprocess.run([cli, "--config", str(config), *map(str, args)],
                                env=custom_env or env, capture_output=True)
        doc = json.loads(result.stdout)
        assert doc["ok"] is ok, (args, result.returncode, doc)
        assert (result.returncode == 0) is ok, (args, doc)
        return doc

    full = run("ls", image)["details"]["entries"]
    assert len(full) > 110
    paths = {e["path"]: e for e in full}
    usage = run("usage", image)["details"]
    assert usage["schema"] == "dwarfsctl.usage.v1"
    assert usage["total_size"] == sum(e.get("size") or 0 for e in full)
    assert usage["files"] == sum(not e["is_dir"] for e in full)
    assert usage["directories"] == sum(e["is_dir"] and e["path"] != "/" for e in full)
    assert len(usage["root"]["children"]) <= usage["max_children"]
    assert sum(e["size"] for e in usage["root"]["children"]) == usage["total_size"]
    assert paths["/ file *.txt "]["size"] == len(content[" file *.txt "])
    assert run("ls", image, "--path", "/sub", "--children-only")["details"]["entries"] == [paths["/sub/[abc].bin"]]
    assert run("ls", image, "--path", "/empty", "--entry-only")["details"]["entries"] == [paths["/empty"]]
    run("ls", image, "--path", "/missing", "--entry-only", ok=False)
    peek = run("peek", image)["details"]
    assert peek["counts"]["directories"] == 1
    reference = json.loads(subprocess.check_output([dwarfsck, "--json", "-d", "metadata_summary,directory_tree", "-i", str(image)]))
    reference.pop("root")
    assert peek["format"] == reference
    for name, data in content.items():
        dest = root / "read" / name
        run("cat", image, "/" + name, dest)
        assert dest.read_bytes() == data
        run("cat", image, "/" + name, dest, ok=False)
        dest.write_bytes(b"old")
        run("cat", image, "/" + name, dest, "--force")
        assert dest.read_bytes() == data
    bad_dest = root / "not-created"
    for path in ("/missing", "/sub", "/link", "/../hello.txt"):
        run("cat", image, path, bad_dest, ok=False)
        assert not bad_dest.exists()
    dangling = root / "dangling"
    os.symlink(root / "missing-target", dangling)
    run("cat", image, "/hello.txt", dangling, ok=False)
    assert dangling.is_symlink()
    run("cat", image, "/hello.txt", dangling, "--force")
    assert not dangling.is_symlink() and dangling.read_bytes() == b"hello\n"
    dest = root / "extract"
    run("extract", image, dest)
    for name, data in content.items():
        assert (dest / name).read_bytes() == data
    assert os.readlink(dest / "link") == "hello.txt"
    assert (dest / "hardlink").stat().st_ino == (dest / "hello.txt").stat().st_ino
    # A pre-existing output symlink must never redirect writes outside dest.
    outside = root / "outside"
    outside.mkdir()
    victim = outside / "[abc].bin"
    victim.write_bytes(b"keep")
    trapped = root / "trapped"
    trapped.mkdir()
    os.symlink(outside, trapped / "sub")
    result = subprocess.run([cli, "--config", str(config), "extract", str(image), str(trapped)],
                            env=env, capture_output=True)
    json.loads(result.stdout)  # either safe replacement or a structured refusal
    assert victim.read_bytes() == b"keep"
    corrupt = root / "corrupt.dwarfs"
    corrupt.write_bytes(b"not a filesystem")
    for command in ("ls", "peek"):
        run(command, corrupt, ok=False)
    run("cat", corrupt, "/hello.txt", bad_dest, ok=False)
    assert not bad_dest.exists()
    run("extract", corrupt, root / "corrupt-extract", ok=False)
    run("ls", image, ok=False, custom_env=dict(env, DWARFSCTL_ENGINE="/missing/reader"))
    doctor = run("doctor")["details"]["checks"]
    assert any(c["id"] == "native_reader" and c["ok"] for c in doctor)
    # The native bridge must not publish partial bytes, nor overwrite a file
    # that appears after the early check. These helpers deterministically create
    # those races at the same private protocol boundary as the C++ executable.
    fake = root / "fake-reader"
    fake.write_text(f"#!{sys.executable}\nimport sys\nsys.stdout.write('partial')\nsys.exit(9)\n")
    fake.chmod(0o700)
    failure_env = dict(env, DWARFSCTL_ENGINE=str(fake))
    run("cat", image, "/hello.txt", bad_dest, ok=False, custom_env=failure_env)
    assert not bad_dest.exists()
    raced = root / "raced"
    fake.write_text(f"#!{sys.executable}\nfrom pathlib import Path\nimport sys\n"
                    f"Path({str(raced)!r}).write_bytes(b'concurrent')\n"
                    "sys.stdout.write('new content')\n")
    run("cat", image, "/hello.txt", raced, ok=False, custom_env=failure_env)
    assert raced.read_bytes() == b"concurrent"
    # Empty archives are valid. A successful extraction must publish the
    # destination directory even though it contains no entries.
    empty_src = root / "empty-source"
    empty_src.mkdir()
    empty_image = root / "empty.dwarfs"
    subprocess.run([mkdwarfs, "-i", str(empty_src), "-o", str(empty_image), "-l", "1"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    empty_dest = root / "empty-dest"
    run("extract", empty_image, empty_dest)
    assert empty_dest.is_dir()
    assert list(empty_dest.iterdir()) == []
    # Discovery from an installed prefix works without an environment override.
    prefix = root / "prefix"
    (prefix / "bin").mkdir(parents=True)
    (prefix / "libexec").mkdir()
    shutil.copy2(cli, prefix / "bin/dwarfsctl")
    shutil.copy2(reader, prefix / "libexec/dwarfsctl-engine")
    install_env = dict(env)
    install_env.pop("DWARFSCTL_ENGINE")
    installed = subprocess.run([str(prefix / "bin/dwarfsctl"), "--config", str(config), "ls", str(image)],
                               env=install_env, capture_output=True)
    assert installed.returncode == 0, installed.stdout
    assert json.loads(installed.stdout)["details"]["entries"] == full
print("reader integration passed: listing, preview, exact/large/empty reads, publication, extraction, links, errors, installed discovery")
