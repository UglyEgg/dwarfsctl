# Mount service API

The DwarFSctl tray application exposes mount control on the session D-Bus. The
Dolphin integration and `dwarfsctl` CLI use this service so one process owns the
mount registry and emits state changes.

Mounts1 is a stable public interface. Additive methods, signals and optional JSON
fields may be introduced without changing its major version. Removing a method,
changing a signature or changing an existing field's meaning requires a new
interface such as `Mounts2`.

## Address

| Item | Value |
|---|---|
| Bus | Session bus |
| Service | `org.quest.entropy.dwarfsctl.Tray` |
| Object | `/org/quest/entropy/dwarfsctl/Mounts` |
| Interface | `org.quest.entropy.dwarfsctl.Mounts1` |
| API major | `1` |

The service is D-Bus activatable. A client may make its first call while the tray
is not running. Mount operations fail when no session bus is available.

## Return values

`ApiMajor` returns an integer. Every other method returns a UTF-8 JSON string.

Successful response:

```json
{
  "command": "mount",
  "ok": true,
  "details": {},
  "error": null
}
```

Failed response:

```json
{
  "command": "mount",
  "ok": false,
  "details": null,
  "error": {
    "kind": "cli",
    "code": 6,
    "message": "The image is not mounted"
  }
}
```

Clients must ignore unknown fields in `details` and `error`.

While the service is checking or changing a volume, concurrent executor requests
may return a `Busy` error. Wait for that operation to finish before retrying. A request
timeout does not cancel an operation already dispatched to the service; its
outcome remains unconfirmed until the mount state is checked.

## Methods

### `int ApiMajor()`

Returns `1` for Mounts1.

### `string ListMounts()`

Returns the current managed mounts:

```json
{
  "command": "mount_list",
  "ok": true,
  "details": {
    "mounts": [
      {
        "image": "/home/user/archive.dwarfs",
        "mountpoint": "/run/user/1000/dwarfs/archive-a1b2c3d4"
      }
    ]
  },
  "error": null
}
```

### `string Status(string imagePath)`

Returns the current state for an image or recorded mountpoint. Source paths
can remain recorded after the image is moved or removed. Optional
`source_available`, `generation_verified` and `source_changed` flags may be null
when a bounded metadata check cannot establish their value. A missing source
does not unmount an existing volume.

### `string Mount(string imagePath)`

Mounts an image read-only. Calling it for an already mounted image succeeds and
returns the existing state. A successful state change emits `MountsChanged`.

### `string UnmountByMountpoint(string mountpoint)`

Unmounts the image at a managed mountpoint.

### `string UnmountByImage(string imagePath)`

Unmounts a managed image by path.

### `string UnmountAll()`

Unmounts every image managed by the current session.

### `string UnmountLast()`

Unmounts the most recently registered image.

## Signal

### `MountsChanged(string mountsJson)`

Emitted after the managed mount set changes. The argument is a complete snapshot,
not a diff:

```json
[
  {
    "image": "/home/user/archive.dwarfs",
    "mountpoint": "/run/user/1000/dwarfs/archive-a1b2c3d4"
  }
]
```

## Client behavior

Check `ApiMajor` before relying on the interface. A client that does not support
the reported major should stop and explain the mismatch. Do not fall back to
mounting locally; doing so creates a second mount authority that cannot keep the
tray, Dolphin Places and other clients synchronized.

The `dwarfs:` KIO worker is not a Mounts1 client. It reads image contents without
mounting and does not consume mount state.
