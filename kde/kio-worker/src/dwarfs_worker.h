// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// KIO worker: dwarfs:/path/to/image.dwarfs/inner/path
//
// Makes a DwarFS image browsable in Dolphin and every other KIO consumer without
// mounting it. Mount control stays with the tray's Mounts1 D-Bus service; this
// worker never mounts anything and never becomes a second authority for mount
// state (docs/DBUS_API.md).
//
// Image access goes through the public dwarfsctl interface. Its packaged
// private engine supplies DwarFS, keeping format-specific behavior out of this
// worker process.
//
// NOTE on the API shape, which is easy to get wrong: KIO::WorkerBase is NOT a
// QObject and has no Q_OBJECT macro. Registration happens through a
// KIO::WorkerFactory subclass (see worker_factory.cpp), not by making the worker
// itself a plugin. A worker also has no Qt event loop of its own.

#pragma once

#include <kio/workerbase.h>

class DwarfsWorker : public KIO::WorkerBase
{
public:
    DwarfsWorker(const QByteArray &poolSocket, const QByteArray &appSocket);
    ~DwarfsWorker() override;

    KIO::WorkerResult listDir(const QUrl &url) override;
    KIO::WorkerResult stat(const QUrl &url) override;
    KIO::WorkerResult get(const QUrl &url) override;
    KIO::WorkerResult copy(const QUrl &src,
                           const QUrl &dest,
                           int permissions,
                           KIO::JobFlags flags) override;

    // Write operations, refused explicitly.
    //
    // A DwarFS image is read-only by design, so these can never succeed. They are
    // overridden rather than left to KIO's defaults so the refusal is a decision
    // made here, in one visible place, instead of depending on a default that
    // could change under us -- and so Dolphin greys the actions out rather than
    // attempting them and failing obscurely.
    KIO::WorkerResult put(const QUrl &url, int permissions, KIO::JobFlags flags) override;
    KIO::WorkerResult mkdir(const QUrl &url, int permissions) override;
    KIO::WorkerResult del(const QUrl &url, bool isfile) override;
    KIO::WorkerResult rename(const QUrl &src, const QUrl &dest, KIO::JobFlags flags) override;
    KIO::WorkerResult symlink(const QString &target, const QUrl &dest, KIO::JobFlags flags) override;
    KIO::WorkerResult chmod(const QUrl &url, int permissions) override;
};
