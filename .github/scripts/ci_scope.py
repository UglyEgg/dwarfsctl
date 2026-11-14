#!/usr/bin/env python3
"""Skip expensive CI only when the complete change is known to be documentation."""
import json
import os
from pathlib import Path, PurePosixPath
import subprocess

DOCUMENTS = {
    "README.md", "CONTRIBUTING.md", "SECURITY.md", "packaging/README.md",
    "docs/DBUS_API.md",
}
IMAGE_SUFFIXES = {".svg", ".png", ".jpg", ".jpeg", ".webp"}


def documentation_only(paths):
    if not paths:
        return False
    for name in paths:
        path = PurePosixPath(name)
        if name in DOCUMENTS:
            continue
        if (path.parts[:2] in {("docs", "screenshots"), ("docs", "badges")}
                and path.suffix.lower() in IMAGE_SUFFIXES):
            continue
        return False
    return True


def git(*args):
    return subprocess.check_output(["git", *args], stderr=subprocess.DEVNULL)


def changed_files(event_name, event):
    if event_name == "pull_request":
        base = event["pull_request"]["base"]["sha"]
        head = event["pull_request"]["head"]["sha"]
        base = git("merge-base", base, head).decode().strip()
    elif event_name == "push":
        base, head = event["before"], event["after"]
    else:
        # Manual and scheduled runs always perform the complete validation.
        return None
    if not base or set(base) == {"0"} or not head or set(head) == {"0"}:
        return None
    # Report renames as deletion + addition so moving source into docs cannot
    # hide the source change. NUL separators preserve unusual filenames.
    names = git("diff", "--name-only", "--no-renames", "-z", base, head)
    return [name.decode("utf-8", errors="surrogateescape")
            for name in names.split(b"\0") if name]


def main():
    try:
        event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
        paths = changed_files(os.environ.get("GITHUB_EVENT_NAME"), event)
    except (KeyError, OSError, ValueError, subprocess.CalledProcessError):
        paths = None
    full = not documentation_only(paths)
    output = f"full_ci={'true' if full else 'false'}\n"
    print(output, end="")
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as target:
            target.write(output)
    reason = ("Run full CI: runtime, build, workflow, unclassified, or unavailable change information."
              if full else "Documentation-only change: native package builds and CodeQL are not required.")
    print(reason)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as target:
            target.write(reason + "\n")


if __name__ == "__main__":
    main()
