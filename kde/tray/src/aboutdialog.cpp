// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "aboutdialog.h"
#include "dwarfsctlrunner.h"

#include <KLocalizedString>
#include <KPageWidgetItem>

#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QFile>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QScrollArea>
#include <QTextBrowser>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#ifndef DWARFSCTL_PLUGIN_VERSION
#define DWARFSCTL_PLUGIN_VERSION "(unknown)"
#endif

namespace {

QString resourceText(const QString &path, const QString &fallback)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : fallback;
}

QString dwarfsctlVersionString()
{
    const auto result = DwarfsctlRunner::run({QStringLiteral("--version")},
                                              DwarfsctlRunner::defaultTimeoutMs());
    if (result.crashed) {
        return i18n("(unavailable)");
    }
    QString output = QString::fromUtf8(result.stdout_bytes).trimmed();
    if (output.isEmpty()) {
        output = QString::fromUtf8(result.stderr_bytes).trimmed();
    }
    return output.isEmpty() ? i18n("(unknown)")
                            : output.section(QLatin1Char('\n'), 0, 0).trimmed();
}

QLabel *wrappedLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setOpenExternalLinks(true);
    return label;
}

QFrame *separator(QWidget *parent)
{
    auto *line = new QFrame(parent);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

QScrollArea *scrollPage(QWidget *content, QWidget *parent)
{
    auto *scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    return scroll;
}

void showAsciiEasterEgg(QWidget *parent)
{
    static const QString art = QStringLiteral(
        "     ___                  ___ ___\n"
        "    |   \\__ __ ____ _ _ _| __/ __|\n"
        "    | |) \\ V  V / _` | '_| _|\\__ \\ \n"
        "    |___/ \\_/\\_/\\__,_|_| |_| |___/\n"
        "\n"
        "         Deduplicating Warp-speed\n"
        "      Advanced Read-only File System\n"
        "         by Marcus Holland-Moritz\n");

    auto *window = new QWidget(parent, Qt::Tool);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(i18n("DwarFS"));

    auto *layout = new QVBoxLayout(window);
    auto *label = new QLabel(window);
    label->setTextFormat(Qt::RichText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    label->setAlignment(Qt::AlignHCenter);
    label->setText(QStringLiteral("<pre>%1</pre>").arg(art.toHtmlEscaped()));
    layout->addWidget(label);

    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, window);
    QObject::connect(close, &QDialogButtonBox::rejected, window, &QWidget::close);
    layout->addWidget(close);
    window->show();
}

QWidget *creditRow(const QString &iconName,
                   const QString &title,
                   const QString &description,
                   QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 4, 0, 4);
    layout->setSpacing(12);

    auto *icon = new QLabel(row);
    icon->setPixmap(QIcon::fromTheme(iconName).pixmap(32, 32));
    icon->setFixedSize(36, 36);
    icon->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon, 0, Qt::AlignTop);

    auto *copy = new QVBoxLayout();
    copy->setContentsMargins(0, 0, 0, 0);
    copy->setSpacing(2);
    copy->addWidget(wrappedLabel(QStringLiteral("<b>%1</b>").arg(title), row));
    copy->addWidget(wrappedLabel(description, row));
    layout->addLayout(copy, 1);
    return row;
}

QTextBrowser *documentPage(const QString &markdown, QWidget *parent)
{
    auto *browser = new QTextBrowser(parent);
    browser->setReadOnly(true);
    browser->setOpenExternalLinks(true);
    browser->setMarkdown(markdown);
    return browser;
}

QToolButton *artLinkButton(const QString &resource,
                           const QString &toolTip,
                           const QString &accessibleName,
                           QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(toolTip);
    button->setAccessibleName(accessibleName);
    button->setFixedSize(52, 52);
    const QPixmap pixmap(resource);
    if (!pixmap.isNull()) {
        button->setIcon(QIcon(pixmap));
        button->setIconSize(QSize(44, 44));
    }
    return button;
}

} // namespace

AboutDialog::AboutDialog(QWidget *parent)
    : KPageDialog(parent)
{
    setWindowTitle(i18n("About DwarFSctl"));
    setFaceType(KPageDialog::Tabbed);
    setStandardButtons(QDialogButtonBox::Close);
    resize(720, 580);
    setMinimumSize(620, 480);

    auto *aboutContent = new QWidget(this);
    auto *root = new QVBoxLayout(aboutContent);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(16);

    auto *hero = new QGridLayout();
    hero->setContentsMargins(0, 0, 0, 0);
    hero->setHorizontalSpacing(24);
    hero->setVerticalSpacing(0);

    auto *artButton = new QToolButton(aboutContent);
    artButton->setAutoRaise(true);
    artButton->setAccessibleName(i18n("DwarFSctl artwork"));
    artButton->setToolTip(i18n("Show the DwarFS banner"));
    artButton->setCursor(Qt::PointingHandCursor);
    artButton->setFixedSize(184, 184);
    const QPixmap artwork(QStringLiteral(":/dwarfsctl/dwarfsctl_about.png"));
    if (!artwork.isNull()) {
        artButton->setIcon(QIcon(artwork));
        artButton->setIconSize(QSize(176, 176));
    }
    QObject::connect(artButton, &QToolButton::clicked, this, [this] {
        showAsciiEasterEgg(this);
    });
    hero->addWidget(artButton, 0, 0, 1, 1, Qt::AlignTop | Qt::AlignHCenter);

    auto *identity = new QVBoxLayout();
    identity->setContentsMargins(0, 2, 0, 0);
    identity->setSpacing(8);

    auto *title = new QLabel(i18n("DwarFSctl"), aboutContent);
    QFont brandFont = title->font();
    const int fontId = QFontDatabase::addApplicationFont(QStringLiteral(":/dwarfsctl/DellaRespira-Regular.ttf"));
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    if (!families.isEmpty()) {
        brandFont.setFamily(families.first());
    }
    brandFont.setPointSizeF(brandFont.pointSizeF() * 1.7);
    brandFont.setBold(true);
    title->setFont(brandFont);
    identity->addWidget(title);

    identity->addWidget(wrappedLabel(i18n("<b>DwarFS images, at home in KDE Plasma.</b>"), aboutContent));
    identity->addWidget(wrappedLabel(
        i18n("Create, browse, mount, and extract compressed filesystem images with DwarFS Utility and Dolphin."),
        aboutContent));
    auto *linkRow = new QHBoxLayout();
    linkRow->setContentsMargins(0, 4, 0, 0);
    linkRow->setSpacing(6);
    auto *githubButton = artLinkButton(
        QStringLiteral(":/dwarfsctl/github-icon.png"),
        i18n("Open the DwarFSctl project on GitHub"),
        i18n("GitHub project"),
        aboutContent);
    auto *licenseButton = artLinkButton(
        QStringLiteral(":/dwarfsctl/gplv3plus-icon.png"),
        i18n("View the GPLv3+ license"),
        i18n("GPLv3+ license"),
        aboutContent);
    linkRow->addWidget(githubButton);
    linkRow->addWidget(licenseButton);
    linkRow->addStretch(1);
    identity->addLayout(linkRow);
    QObject::connect(githubButton, &QToolButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/UglyEgg/dwarfsctl")));
    });
    identity->addStretch(1);
    hero->addLayout(identity, 0, 1);
    hero->setColumnStretch(1, 1);
    root->addLayout(hero);

    auto *versionsBox = new QGroupBox(i18n("Version information"), aboutContent);
    auto *versions = new QFormLayout(versionsBox);
    versions->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    versions->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    versions->addRow(i18n("Command-line tool:"), new QLabel(dwarfsctlVersionString(), versionsBox));
    versions->addRow(i18n("Tray integration:"), new QLabel(QString::fromLatin1(DWARFSCTL_TRAY_VERSION), versionsBox));
    versions->addRow(i18n("Dolphin integration:"), new QLabel(QString::fromLatin1(DWARFSCTL_PLUGIN_VERSION), versionsBox));
    versions->addRow(i18n("D-Bus interface:"), new QLabel(QString::number(DWARFSCTL_DBUS_API_MAJOR), versionsBox));
    root->addWidget(versionsBox);

    root->addWidget(separator(aboutContent));
    root->addWidget(wrappedLabel(
        i18n("DwarFSctl is an independent project and is not affiliated with or endorsed by the DwarFS project. "
             "DwarFS was created by Marcus Holland-Moritz."),
        aboutContent));
    auto *copyright = new QLabel(QStringLiteral("© 2026 Richard Majewski · Varanid Works"), aboutContent);
    copyright->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(copyright);
    root->addStretch(1);

    auto *aboutItem = addPage(scrollPage(aboutContent, this), i18n("About"));
    aboutItem->setIcon(QIcon(QStringLiteral(":/dwarfsctl-tray/resources/dwarfsctl-app.svg")));

    auto *creditsContent = new QWidget(this);
    auto *creditsLayout = new QVBoxLayout(creditsContent);
    creditsLayout->setContentsMargins(24, 20, 24, 20);
    creditsLayout->setSpacing(14);

    auto *creditsTitle = new QLabel(i18n("People and projects"), creditsContent);
    QFont creditsTitleFont = creditsTitle->font();
    creditsTitleFont.setPointSizeF(creditsTitleFont.pointSizeF() * 1.35);
    creditsTitleFont.setBold(true);
    creditsTitle->setFont(creditsTitleFont);
    creditsLayout->addWidget(creditsTitle);
    creditsLayout->addWidget(wrappedLabel(
        i18n("DwarFSctl stands on the work of the following people and open-source communities."),
        creditsContent));

    auto *projectBox = new QGroupBox(i18n("DwarFSctl"), creditsContent);
    auto *projectLayout = new QVBoxLayout(projectBox);
    projectLayout->addWidget(creditRow(
        QStringLiteral("user-identity"),
        i18n("Richard Majewski · Varanid Works"),
        i18n("Application design and development, KDE integration, packaging, and visual identity."),
        projectBox));
    creditsLayout->addWidget(projectBox);

    auto *foundationBox = new QGroupBox(i18n("Foundation"), creditsContent);
    auto *foundationLayout = new QVBoxLayout(foundationBox);
    foundationLayout->addWidget(creditRow(
        QStringLiteral("folder-compress"),
        i18n("DwarFS"),
        i18n("Created by <a href=\"https://github.com/mhx/dwarfs\">Marcus Holland-Moritz and contributors</a>. "
             "DwarFSctl builds and ships checksum-pinned DwarFS 0.15.7 libraries."),
        foundationBox));
    foundationLayout->addWidget(separator(foundationBox));
    foundationLayout->addWidget(creditRow(
        QStringLiteral("kde"),
        i18n("KDE and Qt"),
        i18n("Qt 6, KDE Frameworks 6, Plasma, and Dolphin provide the native desktop platform."),
        foundationBox));
    creditsLayout->addWidget(foundationBox);

    auto *librariesBox = new QGroupBox(i18n("Libraries and build tools"), creditsContent);
    auto *librariesLayout = new QVBoxLayout(librariesBox);
    librariesLayout->addWidget(creditRow(
        QStringLiteral("applications-engineering"),
        i18n("Open-source dependencies"),
        i18n("Boost, nlohmann JSON, toml++, range-v3, parallel-hashmap, utf8cpp, libarchive, "
             "OpenSSL, libfuse, compression libraries, CMake, and Ninja."),
        librariesBox));
    creditsLayout->addWidget(librariesBox);
    creditsLayout->addWidget(wrappedLabel(
        i18n("Complete license and source notices are included with each package."),
        creditsContent));
    creditsLayout->addStretch(1);

    auto *creditsItem = addPage(scrollPage(creditsContent, this), i18n("Credits"));
    creditsItem->setIcon(QIcon::fromTheme(QStringLiteral("help-about")));

    auto *license = documentPage(
        resourceText(QStringLiteral(":/dwarfsctl/LICENSE.txt"), i18n("License text is unavailable.")),
        this);
    auto *licenseItem = addPage(license, i18n("License"));
    licenseItem->setIcon(QIcon::fromTheme(QStringLiteral("text-x-copying")));
    QObject::connect(licenseButton, &QToolButton::clicked, this, [this, licenseItem] {
        setCurrentPage(licenseItem);
    });
}
