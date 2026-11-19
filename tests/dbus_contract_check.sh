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

# Check execution arguments, not JSON command names in service responses.
# The tray legitimately returns envelopes with command="mount" or "status".
python3 - "$root/kde/tray/src" <<'PYEOF'
import pathlib, re, sys

calls = re.compile(r"(?:\brunDwarfsctl|\brunTool|DwarfsctlRunner::run|\b\w+\.(?:start|startDetached|setArguments))\s*\(")
verbs = {"mount", "unmount", "status", "list"}
def violations(text):
    found = []
    for call in calls.finditer(text):
        depth, quote, escaped = 1, None, False
        end = call.end()
        while end < len(text) and depth:
            char = text[end]
            if quote:
                if escaped: escaped = False
                elif char == "\\": escaped = True
                elif char == quote: quote = None
            elif char in ('"', "'"): quote = char
            elif char == "(": depth += 1
            elif char == ")": depth -= 1
            end += 1
        tokens = re.findall(r'QStringLiteral\("([a-z-]+)"\)', text[call.end():end])
        # Only the command position is meaningful: config ... list is valid.
        if tokens and tokens[0] == "dwarfsctl": tokens.pop(0)
        command = next((token for token in tokens if not token.startswith("--")), None)
        if command in verbs: found.append(text.count("\n", 0, call.start()) + 1)
    return found

assert violations('runDwarfsctl({QStringLiteral("mount"), image}, true);')
assert violations('p.start(dwarfsctlExe(), {QStringLiteral("status"), image});')
assert not violations('executorBusyReply(QStringLiteral("mount"), image);')
assert not violations('runDwarfsctl({QStringLiteral("config"), QStringLiteral("mkdwarfs-profiles"), QStringLiteral("list")}, true);')
for path in sorted(pathlib.Path(sys.argv[1]).rglob("*")):
    if path.suffix not in {".cpp", ".h"} or path.name.startswith("mount_executor."): continue
    bad = violations(path.read_text())
    if bad: raise SystemExit(f"tray must use its mount executor, not mount-control CLI arguments: {path}:{bad}")
print("OK: tray mount control stays in its executor")
PYEOF
