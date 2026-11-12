#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

api_doc="$root/docs/DBUS_API.md"

tray_main="$root/kde/tray/src/main.cpp"
plugin_hdr="$root/kde/kf6-plugin/src/traydbus.h"
cli_app="$root/cpp/cli/diagnostics.cpp"
plugin_src="$root/kde/kf6-plugin/src"

need_file() {
  [[ -f "$1" ]] || { echo "ERROR: missing file: $1" >&2; exit 1; }
}

need_file "$api_doc"
need_file "$tray_main"
need_file "$plugin_hdr"
need_file "$cli_app"
[[ -d "$plugin_src" ]] || { echo "ERROR: missing dir: $plugin_src" >&2; exit 1; }

svc="org.quest.entropy.dwarfsctl.Tray"
path="/org/quest/entropy/dwarfsctl/Mounts"
iface="org.quest.entropy.dwarfsctl.Mounts1"

# Docs must contain canonical identifiers
grep -F "$svc" "$api_doc" >/dev/null
grep -F "$path" "$api_doc" >/dev/null
grep -F "$iface" "$api_doc" >/dev/null

# Code must contain canonical identifiers
grep -F "$svc" "$tray_main" >/dev/null
grep -F "$path" "$tray_main" >/dev/null
grep -F "$iface" "$tray_main" >/dev/null

grep -F "$svc" "$plugin_hdr" >/dev/null
grep -F "$path" "$plugin_hdr" >/dev/null
grep -F "$iface" "$plugin_hdr" >/dev/null

# CLI must route mount list/status/mount/unmount through D-Bus by default.
grep -F '"ListMounts"' "$cli_app" >/dev/null
grep -F '"Status"' "$cli_app" >/dev/null
grep -F '"Mount"' "$cli_app" >/dev/null
# CLI must not rely on any recursion escape hatch.
if grep -F "DWARFSCTL_DBUS_DISABLE" "$cli_app" >/dev/null; then
  echo "ERROR: CLI references DWARFSCTL_DBUS_DISABLE (escape hatch removed)" >&2
  exit 1
fi

# Tray must disable D-Bus in its dwarfsctl subprocesses to avoid recursion.
# Tray must not set DWARFSCTL_DBUS_DISABLE (tray no longer shells out to dwarfsctl mount/unmount).
if grep -F "DWARFSCTL_DBUS_DISABLE" "$tray_main" >/dev/null; then
  echo "ERROR: tray sets DWARFSCTL_DBUS_DISABLE (escape hatch removed)" >&2
  exit 1
fi

# Plugin must not shell out for mount control in normal operation.
# (Mount/unmount/status are D-Bus controlled; subprocess calls here reintroduce drift.)
#
# This checks the INVARIANT rather than a string shape: a DwarfsctlRunner::run call
# whose argv contains a mount-control verb. Grepping for the bare literal is wrong in
# both directions -- it misses a verb assembled into a variable, and it false-positives
# on `env.command = QStringLiteral("status")`, which is a JSON envelope field and not a
# CLI argument. The previous version of this check managed neither: its pattern was
# double-escaped (`QStringLiteral\\(\\"status\\"\\)`), so grep could never match it, and
# the check passed unconditionally from the day it was written.
python3 - "$plugin_src" <<'PYEOF'
import pathlib, re, sys

# Verbs that must reach the tray over Mounts1, never a subprocess.
verbs = {"mount", "unmount", "status", "list"}
bad = []

root = pathlib.Path(sys.argv[1])
sources = sorted(root.rglob("*.cpp")) + sorted(root.rglob("*.h"))
for path in sources:
    text = path.read_text(errors="replace")
    # Only the argv expression of a runner call: `DwarfsctlRunner::run({ ... }, ...)`.
    for m in re.finditer(r"DwarfsctlRunner::run\(\s*\{([^}]*)\}", text, re.S):
        found = set(re.findall(r'QStringLiteral\("([a-z-]+)"\)', m.group(1)))
        hit = found & verbs
        if hit:
            line = text[: m.start()].count("\n") + 1
            bad.append("%s:%d: %s" % (path, line, sorted(hit)))

if bad:
    print("ERROR: the plugin shells out for mount control; use the Mounts1 D-Bus API.", file=sys.stderr)
    for b in bad:
        print("  " + b, file=sys.stderr)
    sys.exit(1)
print("OK: plugin invokes no mount-control CLI verb")
PYEOF
[ $? -eq 0 ] || exit 1

echo "OK: D-Bus Mounts1 contract static checks passed"

# Ensure CLI mount control does not reintroduce local mount ops paths
if grep -F "mount_ops::" "$cli_app" >/dev/null || grep -F "mountinfo::" "$cli_app" >/dev/null; then
  echo "ERROR: CLI app/mod.rs references local mount ops/mountinfo; mount control must be D-Bus-only"
  exit 1
fi

# Ensure tray does not shell out to dwarfsctl for mount control (prevents recursion and split-brain)
tray_src="$root/kde/tray/src"

# We allow the word 'dwarfsctl' in About/version text and in docs, but tray must not invoke dwarfsctl with mount/unmount/status/list.
# Fail if any non-mount-executor source file contains both 'dwarfsctl' and a CLI verb token.
bad_tray=$(grep -RIn --include='*.cpp' --include='*.h' -E 'dwarfsctl' "$tray_src"   | grep -v 'mount_executor\.'   | cut -d: -f1 | sort -u || true)

if [[ -n "$bad_tray" ]]; then
  # For each file that references dwarfsctl, ensure it does not also contain verb literals.
  while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    if grep -E 'QStringLiteral\("(mount|unmount|status)"\)' "$f" >/dev/null; then
      echo "ERROR: tray source invokes dwarfsctl and contains mount-control verb literals: $f" >&2
      exit 1
    fi
    if grep -E 'QStringLiteral\("mount"\).*QStringLiteral\("list"\)' "$f" >/dev/null; then
      echo "ERROR: tray source contains dwarfsctl mount list invocation: $f" >&2
      exit 1
    fi
  done <<< "$bad_tray"
fi

