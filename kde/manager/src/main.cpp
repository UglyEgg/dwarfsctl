// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "managerwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QIcon>

#include <KLocalizedString>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("dwarfsctl-manager"));
    app.setApplicationDisplayName(QObject::tr("DwarFS Utility"));
    app.setDesktopFileName(QStringLiteral("org.quest.entropy.dwarfsctl.Manager"));
    app.setOrganizationDomain(QStringLiteral("entropy.quest"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("dwarfsctl")));
    KLocalizedString::setApplicationDomain("dwarfsctl");

    QCommandLineParser parser;
    parser.setApplicationDescription(QObject::tr("Manage DwarFS filesystem images"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("images"), QObject::tr("DwarFS images to add"), QStringLiteral("[images…]"));
    app.setApplicationVersion(QStringLiteral(DWARFSCTL_MANAGER_VERSION));
    parser.process(app);

    DwarfsManagerWindow window;
    window.openImages(parser.positionalArguments());
    window.show();
    return app.exec();
}
