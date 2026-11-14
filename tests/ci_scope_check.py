#!/usr/bin/env python3
"""Regression coverage for CI path decisions and complete PR change ranges."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ci_scope", ROOT / ".github/scripts/ci_scope.py")
scope = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scope)


class ScopeTests(unittest.TestCase):
    def test_public_docs_and_artwork_skip_full_ci(self):
        self.assertTrue(scope.documentation_only([
            "README.md", "SECURITY.md", "docs/DBUS_API.md", "packaging/README.md",
            "docs/badges/opendesktop.svg", "docs/screenshots/a picture\n.png",
        ]))

    def test_code_build_and_embedded_documents_require_full_ci(self):
        for path in ["cpp/cli/main.cpp", "packaging/targets.sh", "CMakeLists.txt",
                     ".github/workflows/ci.yml", ".github/scripts/ci_scope.py",
                     "tests/ci_scope_check.py", "VERSION", "CHANGELOG.md",
                     "kde/tray/resources/about/CREDITS.md", "LICENSE",
                     "docs/badges/script.js", "docs/new-unclassified-file"]:
            with self.subTest(path=path):
                self.assertFalse(scope.documentation_only(["README.md", path]))

    def test_unknown_or_empty_information_requires_full_ci(self):
        for paths in [None, []]:
            self.assertFalse(scope.documentation_only(paths))
        self.assertIsNone(scope.changed_files("schedule", {}))
        self.assertIsNone(scope.changed_files("workflow_dispatch", {}))
        self.assertIsNone(scope.changed_files("push", {"before": "0" * 40, "after": "a" * 40}))

    def test_pr_includes_earlier_code_changes_and_rename_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            old = Path.cwd()
            try:
                os.chdir(directory)
                def git(*args):
                    return subprocess.check_output(["git", *args], stderr=subprocess.DEVNULL).decode().strip()
                git("init", "-q")
                git("config", "user.name", "CI test")
                git("config", "user.email", "ci@example.invalid")
                Path("README.md").write_text("initial\n")
                Path("source.cpp").write_text("int value;\n")
                git("add", "."); git("commit", "-qm", "initial")
                base = git("rev-parse", "HEAD")
                Path("source.cpp").write_text("int changed;\n")
                git("add", "."); git("commit", "-qm", "code")
                previous = git("rev-parse", "HEAD")
                Path("README.md").write_text("updated\n")
                git("add", "."); git("commit", "-qm", "docs")
                head = git("rev-parse", "HEAD")
                event = {"pull_request": {"base": {"sha": base}, "head": {"sha": head}}}
                self.assertFalse(scope.documentation_only(scope.changed_files("pull_request", event)))
                self.assertTrue(scope.documentation_only(scope.changed_files("push", {"before": previous, "after": head})))
                git("mv", "source.cpp", "CONTRIBUTING.md")
                git("commit", "-qm", "rename")
                names = scope.changed_files("push", {"before": head, "after": git("rev-parse", "HEAD")})
                self.assertIn("source.cpp", names)
                self.assertFalse(scope.documentation_only(names))
                with self.assertRaises(subprocess.CalledProcessError):
                    scope.changed_files("push", {"before": "f" * 40, "after": head})
            finally:
                os.chdir(old)


if __name__ == "__main__":
    unittest.main()
