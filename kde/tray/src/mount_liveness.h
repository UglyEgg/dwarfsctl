// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

namespace dwarfsctl_tray {

/// How long a liveness probe may take before the mountpoint is called
/// unresponsive. A daemon that is alive but wedged blocks any request
/// indefinitely, so the probe always runs with a bound.
inline constexpr int kMountLivenessProbeTimeoutMs = 2000;

/// Extra time allowed for a killed probe child to be reaped. A child stuck
/// inside the kernel cannot be killed until its request returns, and the caller
/// must not block waiting for that.
inline constexpr int kMountLivenessReapGraceMs = 200;

/// What a probe learned about a mountpoint.
enum class MountLiveness {
    /// A request was served: the mount is usable.
    Live,
    /// The daemon that owned the mount is gone (ENOTCONN). The mount table
    /// still lists the mountpoint, but nothing can be read through it.
    Dead,
    /// No answer within the bound. Treated as unusable: the path serves no
    /// requests, and mounting over it fails until the entry is cleared.
    Unresponsive,
    /// The probe itself could not run (fork failed, an error unrelated to the
    /// daemon, a path that vanished). Says nothing about the mount.
    Unknown,
};

/// True when `mountpoint` is listed in the kernel's mount table.
///
/// This is a *table* match only. A mount can be listed and still be unusable
/// (see `MountLiveness`), so callers deciding whether a path works must use
/// `mountsUsable()` or `probeMountLiveness()` instead.
bool isMounted(const QString& mountpoint);

/// Classify a raw `statfs(2)` outcome. Pure, so the meaning of each errno is
/// testable without a FUSE mount: only ENOTCONN is evidence of a dead daemon.
MountLiveness classifyStatfsResult(int rc, int err);

/// Decide whether a mount table entry may be treated as a working mount.
///
/// Pure, taking the two halves as arguments so the decision table is testable
/// without a FUSE mount. This is the rule the whole fix rests on: being listed
/// is not the same as being usable, so `listed` alone must never be read as
/// success.
bool mountEntryIsUsable(bool listed, MountLiveness probed);

/// A name for a liveness result, for diagnostics and JSON details.
QString mountLivenessName(MountLiveness liveness);

/// Ask the filesystem whether the mountpoint serves requests.
///
/// `statfs()` is the cheapest call that always round-trips to a FUSE daemon.
/// `stat()` answers from the attribute cache even after the daemon dies, so it
/// cannot decide this. Because a wedged daemon makes the call block forever,
/// it runs in a forked child that the parent bounds and kills.
MountLiveness probeMountLiveness(const QString& mountpoint);

/// True only when the mountpoint is both listed in the mount table and usable.
///
/// A `MountLiveness::Unknown` result keeps the answer the mount table alone
/// gives: a probe that could not run is no evidence that a mount is gone.
bool mountsUsable(const QString& mountpoint);

} // namespace dwarfsctl_tray
