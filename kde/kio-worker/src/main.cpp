// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dwarfs_worker.h"

#include <QCoreApplication>
#include <KLocalizedString>

// KIO discovers protocols from plugin metadata, then launches third-party
// workers out of process through their exported kdemain entry point. A
// KPluginFactory object is only used for the small set of workers KIO runs in
// process, so exporting one here makes the plugin visible as a Qt plugin but
// leaves the dwarfs: protocol unknown and unlaunchable.
class KIOPluginForMetaData : public QObject
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.kde.kio.worker.dwarfs" FILE DWARFSCTL_KIO_JSON)
};

extern "C" Q_DECL_EXPORT int kdemain(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("kio_dwarfs"));
    KLocalizedString::setApplicationDomain("dwarfsctl");

    if (argc != 4) {
        return -1;
    }

    DwarfsWorker worker(argv[2], argv[3]);
    worker.dispatchLoop();
    return 0;
}

#include "main.moc"
