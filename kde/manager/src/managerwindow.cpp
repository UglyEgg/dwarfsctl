// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "managerwindow.h"

#include "builddialog.h"
#include "buildfeedback.h"
#include "checksumdialog.h"
#include "detailsdialog.h"
#include "dwarfsctljob.h"
#include "dwarfsctlrunner.h"
#include "integritycheckdialog.h"
#include "mountscache.h"
#include "notifications.h"
#include "settingsdialog.h"
#include "trayagentjob.h"
#include "traydbus.h"
#include "visualizationwidgets.h"
#include "uipresentation.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QJsonObject>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <KJobWidgets>
#include <KLocalizedString>
#include <KMessageWidget>

namespace {

constexpr auto kPathRole = Qt::UserRole + 1;
constexpr auto kMountedRole = Qt::UserRole + 2;

QSettings managerSettings()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString directory = QDir(root).filePath(QStringLiteral("dwarfsctl"));
    QDir().mkpath(directory);
    return QSettings(QDir(directory).filePath(QStringLiteral("manager.ini")), QSettings::IniFormat);
}

QString canonicalPath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

QString usageIdentity(const QString &path)
{
    const QFileInfo info(path);
    return canonicalPath(path) + QLatin1Char('|') + QString::number(info.size())
        + QLatin1Char('|') + QString::number(info.lastModified().toMSecsSinceEpoch());
}

QString mountpointFromEnvelope(const dwarfsplugin::Envelope &envelope)
{
    return envelope.details.value(QStringLiteral("mountpoint")).toString();
}

QLabel *valueLabel(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

} // namespace

DwarfsManagerWindow::DwarfsManagerWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setObjectName(QStringLiteral("dwarfsManagerWindow"));
    setWindowTitle(i18n("DwarFS Utility"));
    setMinimumSize(880, 600);
    resize(1080, 720);

    auto *openAction = new QAction(QIcon::fromTheme(QStringLiteral("document-open")), i18n("Open Image…"), this);
    openAction->setObjectName(QStringLiteral("openImageAction"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &DwarfsManagerWindow::chooseImages);

    auto *createAction = new QAction(QIcon::fromTheme(QStringLiteral("document-new")), i18n("Create Image…"), this);
    createAction->setObjectName(QStringLiteral("createImageAction"));
    createAction->setShortcut(QKeySequence::New);
    connect(createAction, &QAction::triggered, this, &DwarfsManagerWindow::createImage);

    m_mountAction = new QAction(QIcon::fromTheme(QStringLiteral("media-mount")), i18n("Mount"), this);
    m_mountAction->setObjectName(QStringLiteral("mountAction"));
    connect(m_mountAction, &QAction::triggered, this, &DwarfsManagerWindow::mountOrUnmount);

    m_browseAction = new QAction(QIcon::fromTheme(QStringLiteral("system-file-manager")), i18n("Open in Dolphin"), this);
    m_browseAction->setObjectName(QStringLiteral("browseAction"));
    connect(m_browseAction, &QAction::triggered, this, &DwarfsManagerWindow::browseImage);

    m_extractAction = new QAction(QIcon::fromTheme(QStringLiteral("archive-extract")), i18n("Extract…"), this);
    m_extractAction->setObjectName(QStringLiteral("extractAction"));
    connect(m_extractAction, &QAction::triggered, this, &DwarfsManagerWindow::extractImage);

    m_verifyAction = new QAction(QIcon::fromTheme(QStringLiteral("security-high")), i18n("Verify…"), this);
    m_verifyAction->setObjectName(QStringLiteral("verifyAction"));
    connect(m_verifyAction, &QAction::triggered, this, &DwarfsManagerWindow::verifyImage);

    m_checksumAction = new QAction(QIcon::fromTheme(QStringLiteral("document-encrypt")), i18n("Checksums…"), this);
    m_checksumAction->setObjectName(QStringLiteral("checksumAction"));
    connect(m_checksumAction, &QAction::triggered, this, &DwarfsManagerWindow::checksums);

    m_forgetAction = new QAction(QIcon::fromTheme(QStringLiteral("list-remove")), i18n("Remove from Recent Images"), this);
    m_forgetAction->setObjectName(QStringLiteral("forgetImageAction"));
    connect(m_forgetAction, &QAction::triggered, this, &DwarfsManagerWindow::forgetCurrentImage);

    m_clearRecentAction = new QAction(i18n("Clear Recent Images"), this);
    m_clearRecentAction->setObjectName(QStringLiteral("clearRecentImagesAction"));
    connect(m_clearRecentAction, &QAction::triggered, this, &DwarfsManagerWindow::clearRecentImages);

    auto *settingsAction = new QAction(QIcon::fromTheme(QStringLiteral("configure")), i18n("Settings…"), this);
    settingsAction->setObjectName(QStringLiteral("settingsAction"));
    connect(settingsAction, &QAction::triggered, this, &DwarfsManagerWindow::showSettings);

    auto *aboutAction = new QAction(QIcon::fromTheme(QStringLiteral("help-about")), i18n("About DwarFSctl"), this);
    aboutAction->setObjectName(QStringLiteral("aboutAction"));
    connect(aboutAction, &QAction::triggered, this, [] { dwarfsplugin::showTrayAbout(); });

    auto *fileMenu = menuBar()->addMenu(i18n("File"));
    fileMenu->addAction(createAction);
    fileMenu->addAction(openAction);
    fileMenu->addSeparator();
    auto *quitAction = new QAction(QIcon::fromTheme(QStringLiteral("application-exit")), i18n("Quit"), this);
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
    fileMenu->addAction(quitAction);
    auto *imageMenu = menuBar()->addMenu(i18n("Image"));
    imageMenu->addAction(m_mountAction);
    imageMenu->addAction(m_browseAction);
    imageMenu->addAction(m_extractAction);
    imageMenu->addSeparator();
    imageMenu->addAction(m_verifyAction);
    imageMenu->addAction(m_checksumAction);
    imageMenu->addSeparator();
    imageMenu->addAction(m_forgetAction);
    imageMenu->addAction(m_clearRecentAction);
    auto *settingsMenu = menuBar()->addMenu(i18n("Settings"));
    settingsMenu->addAction(settingsAction);
    auto *helpMenu = menuBar()->addMenu(i18n("Help"));
    helpMenu->addAction(aboutAction);

    auto *toolbar = addToolBar(i18n("Image actions"));
    toolbar->setObjectName(QStringLiteral("mainToolBar"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    const int toolbarIconSize = qMax(style()->pixelMetric(QStyle::PM_ToolBarIconSize), fontMetrics().height());
    toolbar->setIconSize(QSize(toolbarIconSize, toolbarIconSize));
    toolbar->addAction(createAction);
    toolbar->addAction(openAction);
    toolbar->addSeparator();
    toolbar->addAction(m_mountAction);
    toolbar->addAction(m_browseAction);
    toolbar->addAction(m_extractAction);
    toolbar->addSeparator();
    toolbar->addAction(m_verifyAction);
    auto *toolbarSpacer = new QWidget(toolbar);
    toolbarSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(toolbarSpacer);
    auto *settingsButton = new QToolButton(toolbar);
    settingsButton->setObjectName(QStringLiteral("settingsButton"));
    settingsButton->setDefaultAction(settingsAction);
    settingsButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    settingsButton->setAutoRaise(true);
    toolbar->addWidget(settingsButton);
    auto *aboutButton = new QToolButton(toolbar);
    aboutButton->setObjectName(QStringLiteral("aboutButton"));
    aboutButton->setDefaultAction(aboutAction);
    aboutButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    aboutButton->setAutoRaise(true);
    toolbar->addWidget(aboutButton);

    auto *splitter = new QSplitter(this);
    splitter->setObjectName(QStringLiteral("managerSplitter"));
    splitter->setChildrenCollapsible(false);

    auto *sidebar = new QWidget(splitter);
    sidebar->setMinimumWidth(220);
    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(12, 12, 8, 12);
    auto *imagesHeading = new QLabel(i18n("Images"), sidebar);
    QFont headingFont = imagesHeading->font();
    headingFont.setBold(true);
    imagesHeading->setFont(headingFont);
    sidebarLayout->addWidget(imagesHeading);
    m_images = new QListWidget(sidebar);
    m_images->setObjectName(QStringLiteral("imageList"));
    m_images->setSelectionMode(QAbstractItemView::SingleSelection);
    m_images->setAlternatingRowColors(false);
    m_images->setFrameShape(QFrame::NoFrame);
    m_images->setSpacing(2);
    m_images->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_images, &QListWidget::customContextMenuRequested, this, &DwarfsManagerWindow::showImageContextMenu);
    sidebarLayout->addWidget(m_images, 1);
    m_sidebarSummary = new QLabel(sidebar);
    m_sidebarSummary->setObjectName(QStringLiteral("sidebarSummary"));
    m_sidebarSummary->setAlignment(Qt::AlignCenter);
    sidebarLayout->addWidget(m_sidebarSummary);
    auto *sidebarButtons = new QHBoxLayout();
    auto *addButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), i18n("Add Image…"), sidebar);
    addButton->setObjectName(QStringLiteral("addImageButton"));
    connect(addButton, &QPushButton::clicked, this, &DwarfsManagerWindow::chooseImages);
    sidebarButtons->addWidget(addButton, 1);
    auto *removeButton = new QToolButton(sidebar);
    removeButton->setObjectName(QStringLiteral("forgetImageButton"));
    removeButton->setDefaultAction(m_forgetAction);
    removeButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    sidebarButtons->addWidget(removeButton);
    sidebarLayout->addLayout(sidebarButtons);
    sidebar->setMinimumWidth(qMax(220, sidebarLayout->minimumSize().width()));
    splitter->addWidget(sidebar);

    m_pages = new QStackedWidget(splitter);
    m_pages->setObjectName(QStringLiteral("detailPages"));
    auto *empty = new QWidget(m_pages);
    auto *emptyLayout = new QVBoxLayout(empty);
    emptyLayout->addStretch(1);
    auto *emptyIcon = new QLabel(empty);
    emptyIcon->setPixmap(QIcon::fromTheme(QStringLiteral("dwarfsctl")).pixmap(96, 96));
    emptyIcon->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyIcon);
    m_emptyMessage = new QLabel(i18n("Choose a DwarFS image to inspect it, or create a new one."), empty);
    m_emptyMessage->setObjectName(QStringLiteral("emptyMessage"));
    m_emptyMessage->setAlignment(Qt::AlignCenter);
    m_emptyMessage->setWordWrap(true);
    emptyLayout->addWidget(m_emptyMessage);
    emptyLayout->addStretch(1);
    m_pages->addWidget(empty);

    auto *details = new QWidget(m_pages);
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(dwarfsctl_kde::contentMargins(details));
    detailsLayout->setSpacing(2 * dwarfsctl_kde::layoutSpacing(details));
    auto *header = new QHBoxLayout();
    auto *imageIcon = new QLabel(details);
    const int headerIconSize = style()->pixelMetric(QStyle::PM_MessageBoxIconSize);
    imageIcon->setPixmap(QIcon::fromTheme(QStringLiteral("dwarfsctlimage")).pixmap(headerIconSize, headerIconSize));
    imageIcon->setFixedSize(headerIconSize, headerIconSize);
    imageIcon->setAlignment(Qt::AlignCenter);
    header->addWidget(imageIcon);
    auto *titleColumn = new QVBoxLayout();
    m_name = new ElidingLabel(details);
    m_name->setObjectName(QStringLiteral("imageName"));
    m_name->setFont(dwarfsctl_kde::headingFont(m_name->font()));
    titleColumn->addWidget(m_name);
    m_path = new ElidingLabel(details);
    m_path->setObjectName(QStringLiteral("imagePath"));
    titleColumn->addWidget(m_path);
    header->addLayout(titleColumn, 1);
    auto *stateBox = new QFrame(details);
    stateBox->setObjectName(QStringLiteral("mountStateBox"));
    stateBox->setFrameShape(QFrame::NoFrame);
    auto *stateLayout = new QHBoxLayout(stateBox);
    stateLayout->setContentsMargins(0, 0, 0, 0);
    stateLayout->setSpacing(dwarfsctl_kde::layoutSpacing(stateBox));
    m_stateIcon = new QLabel(stateBox);
    m_stateIcon->setFixedSize(24, 24);
    m_stateIcon->setAlignment(Qt::AlignCenter);
    stateLayout->addWidget(m_stateIcon);
    m_state = new QLabel(stateBox);
    m_state->setObjectName(QStringLiteral("mountState"));
    m_state->setAlignment(Qt::AlignCenter);
    stateLayout->addWidget(m_state);
    header->addWidget(stateBox);
    detailsLayout->addLayout(header);

    m_mountFrame = new QFrame(details);
    m_mountFrame->setObjectName(QStringLiteral("mountLocationStrip"));
    m_mountFrame->setFrameShape(QFrame::NoFrame);
    auto *mountLayout = new QHBoxLayout(m_mountFrame);
    mountLayout->setContentsMargins(0, 0, 0, 0);
    auto *mountIcon = new QLabel(m_mountFrame);
    mountIcon->setPixmap(QIcon::fromTheme(QStringLiteral("dwarfsctl-volume")).pixmap(22, 22));
    mountLayout->addWidget(mountIcon);
    auto *mountCaption = new QLabel(i18n("Mounted at"), m_mountFrame);
    QFont mountCaptionFont = mountCaption->font();
    mountCaptionFont.setBold(true);
    mountCaption->setFont(mountCaptionFont);
    mountLayout->addWidget(mountCaption);
    m_mountpoint = new ElidingLabel(m_mountFrame);
    m_mountpoint->setObjectName(QStringLiteral("mountLocation"));
    mountLayout->addWidget(m_mountpoint, 1);
    auto *copyMountButton = new QToolButton(m_mountFrame);
    copyMountButton->setObjectName(QStringLiteral("copyMountLocationButton"));
    copyMountButton->setIcon(QIcon::fromTheme(QStringLiteral("edit-copy")));
    copyMountButton->setToolTip(i18n("Copy mount location"));
    copyMountButton->setAccessibleName(i18n("Copy mount location"));
    connect(copyMountButton, &QToolButton::clicked, this, &DwarfsManagerWindow::copyMountpoint);
    mountLayout->addWidget(copyMountButton);
    auto *showMountButton = new QToolButton(m_mountFrame);
    showMountButton->setObjectName(QStringLiteral("showMountLocationButton"));
    showMountButton->setIcon(QIcon::fromTheme(QStringLiteral("system-file-manager")));
    showMountButton->setToolTip(i18n("Show mounted image in Dolphin"));
    showMountButton->setAccessibleName(i18n("Show mounted image in Dolphin"));
    connect(showMountButton, &QToolButton::clicked, this, &DwarfsManagerWindow::browseImage);
    mountLayout->addWidget(showMountButton);
    m_mountFrame->hide();
    detailsLayout->addWidget(m_mountFrame);

    m_error = new KMessageWidget(details);
    m_error->setMessageType(KMessageWidget::Error);
    m_error->setCloseButtonVisible(false);
    m_error->setObjectName(QStringLiteral("inlineError"));
    m_error->setWordWrap(true);
    m_error->setTextFormat(Qt::PlainText);
    m_error->hide();
    detailsLayout->addWidget(m_error);
    m_progress = new QProgressBar(details);
    m_progress->setObjectName(QStringLiteral("operationProgress"));
    m_progress->setTextVisible(false);
    m_progress->hide();
    detailsLayout->addWidget(m_progress);

    m_imageViews = new QTabWidget(details);
    m_imageViews->setObjectName(QStringLiteral("imageViews"));
    m_imageViews->setDocumentMode(true);
    auto *overviewPage = new QWidget(m_imageViews);
    auto *overviewLayout = new QVBoxLayout(overviewPage);
    overviewLayout->setContentsMargins(14, 14, 14, 10);
    overviewLayout->setSpacing(10);
    m_compressionOverview = new CompressionOverviewWidget(overviewPage);
    overviewLayout->addWidget(m_compressionOverview);
    auto *technicalHeading = new QLabel(i18n("Technical Details"), overviewPage);
    QFont technicalHeadingFont = technicalHeading->font();
    technicalHeadingFont.setBold(true);
    technicalHeading->setFont(technicalHeadingFont);
    overviewLayout->addWidget(technicalHeading);
    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(2 * dwarfsctl_kde::layoutSpacing(overviewPage));
    form->setVerticalSpacing(dwarfsctl_kde::layoutSpacing(overviewPage));
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_contents = valueLabel(overviewPage);
    m_imageSize = valueLabel(overviewPage);
    m_originalSize = valueLabel(overviewPage);
    m_saved = valueLabel(overviewPage);
    m_format = valueLabel(overviewPage);
    m_created = valueLabel(overviewPage);
    m_createdBy = valueLabel(overviewPage);
    form->addRow(i18n("Contents:"), m_contents);
    form->addRow(i18n("Image size:"), m_imageSize);
    form->addRow(i18n("Original size:"), m_originalSize);
    form->addRow(i18n("Space saved:"), m_saved);
    form->addRow(i18n("DwarFS format:"), m_format);
    form->addRow(i18n("Created:"), m_created);
    form->addRow(i18n("Created by:"), m_createdBy);
    overviewLayout->addLayout(form);
    overviewLayout->addStretch(1);
    auto *overviewScroll = new QScrollArea(m_imageViews);
    overviewScroll->setObjectName(QStringLiteral("overviewScroll"));
    overviewScroll->setFrameShape(QFrame::NoFrame);
    overviewScroll->setWidgetResizable(true);
    overviewScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    overviewScroll->setWidget(overviewPage);
    m_imageViews->addTab(overviewScroll, QIcon::fromTheme(QStringLiteral("view-statistics")), i18n("Overview"));

    auto *contentsPage = new QWidget(m_imageViews);
    auto *contentsLayout = new QVBoxLayout(contentsPage);
    contentsLayout->setContentsMargins(6, 6, 6, 6);
    m_contentRings = new ContentRingsWidget(contentsPage);
    contentsLayout->addWidget(m_contentRings, 1);
    m_imageViews->addTab(contentsPage, QIcon::fromTheme(QStringLiteral("folder")), i18n("Contents"));
    {
        QSettings settings = managerSettings();
        m_imageViews->setCurrentIndex(qBound(0, settings.value(QStringLiteral("view/current"), 0).toInt(), 1));
    }
    connect(m_imageViews, &QTabWidget::currentChanged, this, [](int index) {
        QSettings settings = managerSettings();
        settings.setValue(QStringLiteral("view/current"), index);
    });
    detailsLayout->addWidget(m_imageViews, 1);

    auto *buttons = new QHBoxLayout();
    m_primaryButton = new QPushButton(details);
    connect(m_primaryButton, &QPushButton::clicked, this, &DwarfsManagerWindow::mountOrUnmount);
    buttons->addWidget(m_primaryButton);
    m_browseButton = new QPushButton(QIcon::fromTheme(QStringLiteral("system-file-manager")), i18n("Open in Dolphin"), details);
    connect(m_browseButton, &QPushButton::clicked, this, &DwarfsManagerWindow::browseImage);
    buttons->addWidget(m_browseButton);
    buttons->addStretch(1);
    detailsLayout->addLayout(buttons);
    m_pages->addWidget(details);
    splitter->addWidget(m_pages);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 820});
    setCentralWidget(splitter);

    statusBar()->showMessage(i18n("Ready"));
    connect(m_images, &QListWidget::itemSelectionChanged, this, &DwarfsManagerWindow::selectionChanged);

    auto *mounts = dwarfsplugin::MountsCache::instance();
    connect(mounts, &dwarfsplugin::MountsCache::mountsChanged, this, &DwarfsManagerWindow::refreshImages);
    mounts->ensureStarted();
    refreshImages();
    updateActions();
}

void DwarfsManagerWindow::openImages(const QStringList &paths)
{
    for (const QString &path : paths) {
        addImage(path, path == paths.constLast());
    }
}

QString DwarfsManagerWindow::currentImage() const
{
    const auto *item = m_images->currentItem();
    return item ? item->data(kPathRole).toString() : QString();
}

void DwarfsManagerWindow::chooseImages()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, i18n("Open DwarFS Images"), QString(), i18n("DwarFS images (*.dwarfs)"));
    openImages(paths);
}

void DwarfsManagerWindow::addImage(const QString &path, bool select)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || !info.fileName().endsWith(QStringLiteral(".dwarfs"), Qt::CaseInsensitive)) {
        QMessageBox::warning(this, i18n("Open DwarFS Image"), i18n("%1 is not an accessible DwarFS image.", QDir::toNativeSeparators(path)));
        return;
    }
    const QString canonical = canonicalPath(path);
    rememberImage(canonical);
    refreshImages();
    if (select) {
        for (int row = 0; row < m_images->count(); ++row) {
            if (m_images->item(row)->data(kPathRole).toString() == canonical) {
                m_images->setCurrentRow(row);
                break;
            }
        }
    }
}

void DwarfsManagerWindow::rememberImage(const QString &path)
{
    QSettings settings = managerSettings();
    QStringList images = settings.value(QStringLiteral("recent/images")).toStringList();
    images.removeAll(path);
    images.prepend(path);
    while (images.size() > 20) {
        images.removeLast();
    }
    settings.setValue(QStringLiteral("recent/images"), images);
    settings.sync();
}

QStringList DwarfsManagerWindow::rememberedImages() const
{
    QSettings settings = managerSettings();
    return settings.value(QStringLiteral("recent/images")).toStringList();
}

void DwarfsManagerWindow::refreshImages()
{
    const QString selected = currentImage();
    const auto mounts = dwarfsplugin::MountsCache::instance()->mounts();
    QStringList mounted = mounts.keys();
    mounted.sort(Qt::CaseInsensitive);

    m_images->clear();
    QSet<QString> added;
    const auto appendHeader = [this](const QString &title) {
        auto *item = new QListWidgetItem(title, m_images);
        item->setFlags(Qt::NoItemFlags);
        QFont font = item->font();
        font.setBold(true);
        item->setFont(font);
        item->setForeground(palette().brush(QPalette::PlaceholderText));
    };
    const auto append = [this, &added, &mounts](const QString &rawPath, bool mountedSection) {
        const QString path = canonicalPath(rawPath);
        if (added.contains(path) || !QFileInfo::exists(path)) {
            return;
        }
        added.insert(path);
        const bool isMounted = mounts.contains(path) || mounts.contains(rawPath);
        QString title = QFileInfo(path).completeBaseName();
        QString mountpoint;
        if (isMounted) {
            mountpoint = mounts.value(path, mounts.value(rawPath));
            if (!mountpoint.isEmpty()) {
                title += i18n("\nMounted at %1", QDir::toNativeSeparators(mountpoint));
            }
        }
        auto *item = new QListWidgetItem(
            QIcon::fromTheme(isMounted ? QStringLiteral("dwarfsctl-volume") : QStringLiteral("dwarfsctlimage")),
            title,
            m_images);
        item->setData(kPathRole, path);
        item->setData(kMountedRole, isMounted);
        item->setToolTip(isMounted ? i18n("Mounted\n%1", QDir::toNativeSeparators(path))
                                   : QDir::toNativeSeparators(path));
        item->setStatusTip(path);
        if (mountedSection) {
            item->setSizeHint(QSize(item->sizeHint().width(), qMax(item->sizeHint().height(), 50)));
        }
    };
    if (!mounted.isEmpty()) {
        appendHeader(i18n("Mounted Volumes"));
        for (const QString &path : mounted) append(path, true);
    }
    const QStringList recent = rememberedImages();
    QStringList visibleRecent;
    for (const QString &path : recent) {
        if (QFileInfo::exists(path) && !added.contains(canonicalPath(path))) visibleRecent.append(path);
    }
    if (!visibleRecent.isEmpty()) {
        appendHeader(i18n("Recent Images"));
        for (const QString &path : visibleRecent) append(path, false);
    }

    const int mountedCount = mounted.size();
    const int recentCount = visibleRecent.size();
    if (mountedCount > 0) {
        m_sidebarSummary->setText(i18np("1 image mounted", "%1 images mounted", mountedCount));
    } else if (recentCount > 0) {
        m_sidebarSummary->setText(i18np("1 recent image", "%1 recent images", recentCount));
    } else {
        m_sidebarSummary->setText(i18n("No images added"));
    }

    int selectedRow = -1;
    for (int row = 0; row < m_images->count(); ++row) {
        if (m_images->item(row)->data(kPathRole).toString() == selected) {
            selectedRow = row;
            break;
        }
    }
    if (selectedRow >= 0) {
        m_images->setCurrentRow(selectedRow);
    } else if (m_images->count() > 0) {
        m_images->setCurrentRow(0);
    } else {
        m_pages->setCurrentIndex(0);
    }
    updateActions();
}

void DwarfsManagerWindow::forgetCurrentImage()
{
    const QString path = currentImage();
    if (path.isEmpty()) return;
    QSettings settings = managerSettings();
    QStringList images = settings.value(QStringLiteral("recent/images")).toStringList();
    images.removeAll(path);
    settings.setValue(QStringLiteral("recent/images"), images);
    settings.sync();
    refreshImages();
}

void DwarfsManagerWindow::clearRecentImages()
{
    QSettings settings = managerSettings();
    settings.remove(QStringLiteral("recent/images"));
    settings.sync();
    refreshImages();
}

void DwarfsManagerWindow::showImageContextMenu(const QPoint &position)
{
    auto *item = m_images->itemAt(position);
    if (!item || item->data(kPathRole).toString().isEmpty()) return;
    m_images->setCurrentItem(item);
    QMenu menu(this);
    menu.addAction(m_mountAction);
    menu.addAction(m_browseAction);
    menu.addAction(m_extractAction);
    menu.addSeparator();
    menu.addAction(m_verifyAction);
    menu.addAction(m_checksumAction);
    menu.addSeparator();
    menu.addAction(m_forgetAction);
    menu.exec(m_images->viewport()->mapToGlobal(position));
}

void DwarfsManagerWindow::selectionChanged()
{
    const QString path = currentImage();
    if (path.isEmpty()) {
        m_pages->setCurrentIndex(0);
        updateActions();
        return;
    }
    m_pages->setCurrentIndex(1);
    m_currentStatus = {};
    m_currentEnvelope = {};
    m_name->setFullText(QFileInfo(path).completeBaseName());
    m_path->setFullText(QDir::toNativeSeparators(path));
    m_mountFrame->hide();
    m_compressionOverview->setValues(-1, -1);
    if (m_loadedUsageIdentity != usageIdentity(path)) m_contentRings->setLoading();
    loadStatus(path);
}

void DwarfsManagerWindow::loadStatus(const QString &path)
{
    // Mount-cache and recent-list refreshes can briefly reselect the same item.
    // Status takes the backend's per-image lock, so coalesce identical reads
    // instead of making harmless UI churn contend with itself.
    if (m_statusJob && m_statusPath == path) {
        return;
    }
    setBusy(true, i18n("Reading image details…"));
    auto *job = new dwarfsplugin::DwarfsctlJob(
        {i18n("Read image details"), {QStringLiteral("status"), path}, DwarfsctlRunner::defaultTimeoutMs(), false}, this);
    m_statusJob = job;
    m_statusPath = path;
    connect(job, &KJob::result, this, [this, path, job](KJob *base) {
        auto *statusJob = qobject_cast<dwarfsplugin::DwarfsctlJob *>(base);
        if (m_statusJob == job) {
            m_statusJob = nullptr;
            m_statusPath.clear();
        }
        if (!statusJob || currentImage() != path) return;
        setBusy(false);
        const auto envelope = statusJob->envelope();
        if (statusJob->error() != 0) {
            showInlineError(envelope.error.message.isEmpty() ? statusJob->errorText() : envelope.error.message);
            return;
        }
        dwarfsplugin::StatusDetails status;
        const auto decoded = dwarfsplugin::decodeStatus(envelope, &status);
        if (!decoded.ok) {
            showInlineError(decoded.error_message);
            return;
        }
        showStatus(path, envelope, status);
    });
    job->start();
}

void DwarfsManagerWindow::loadUsage(const QString &path)
{
    const QString identity = usageIdentity(path);
    if (m_loadedUsageIdentity == identity) return;
    if (m_usageJob && m_usageIdentity == identity) return;
    m_contentRings->setLoading();
    auto *job = new dwarfsplugin::DwarfsctlJob(
        {i18n("Read image contents"), {QStringLiteral("usage"), path}, DwarfsctlRunner::defaultTimeoutMs(), false}, this);
    m_usageJob = job;
    m_usageIdentity = identity;
    connect(job, &KJob::result, this, [this, path, identity, job](KJob *base) {
        auto *usageJob = qobject_cast<dwarfsplugin::DwarfsctlJob *>(base);
        if (m_usageJob == job) {
            m_usageJob = nullptr;
            m_usageIdentity.clear();
        }
        if (!usageJob || currentImage() != path || usageIdentity(path) != identity) return;
        const auto envelope = usageJob->envelope();
        if (usageJob->error() != 0) {
            m_contentRings->setError(envelope.error.message.isEmpty() ? usageJob->errorText() : envelope.error.message);
            return;
        }
        if (!m_contentRings->setUsage(envelope.details)) return;
        m_loadedUsageIdentity = identity;
    });
    job->start();
}

void DwarfsManagerWindow::showStatus(const QString &path,
                                     const dwarfsplugin::Envelope &envelope,
                                     const dwarfsplugin::StatusDetails &details)
{
    m_currentStatus = details;
    m_currentEnvelope = envelope;
    m_error->hide();
    const QLocale locale;
    const qint64 imageBytes = QFileInfo(path).size();
    m_compressionOverview->setValues(details.original_size, imageBytes);
    m_contents->setText(details.file_count >= 0 && details.directory_count >= 0
        ? i18np("1 file", "%1 files", details.file_count) + QStringLiteral(", ")
            + i18np("1 folder", "%1 folders", details.directory_count)
        : i18n("Unavailable"));
    m_imageSize->setText(locale.formattedDataSize(imageBytes));
    m_originalSize->setText(details.original_size >= 0 ? locale.formattedDataSize(details.original_size) : i18n("Unavailable"));
    if (details.original_size > 0 && imageBytes >= 0) {
        const qint64 saved = qMax<qint64>(0, details.original_size - imageBytes);
        const double percent = 100.0 * static_cast<double>(saved) / static_cast<double>(details.original_size);
        m_saved->setText(i18n("%1 (%2%)", locale.formattedDataSize(saved), locale.toString(percent, 'f', 1)));
    } else {
        m_saved->setText(i18n("Unavailable"));
    }
    m_format->setText(details.format_version.isEmpty() ? i18n("Unavailable") : details.format_version);
    const QDateTime created = QDateTime::fromString(details.created_on, Qt::ISODate);
    m_created->setText(details.created_on.isEmpty()
        ? i18n("Unavailable")
        : (created.isValid() ? locale.toString(created.toLocalTime(), QLocale::LongFormat) : details.created_on));
    m_createdBy->setText(details.created_by.isEmpty() ? i18n("Unavailable") : details.created_by);
    const bool haveMountpoint = details.mounted && !details.mountpoint.isEmpty();
    m_mountpoint->setFullText(haveMountpoint ? QDir::toNativeSeparators(details.mountpoint) : QString());
    m_mountFrame->setVisible(haveMountpoint);
    m_state->setText(details.mounted ? i18n("Mounted") : i18n("Not Mounted"));
    m_stateIcon->setPixmap(QIcon::fromTheme(details.mounted ? QStringLiteral("dwarfsctl-volume")
                                                           : QStringLiteral("dwarfsctlimage")).pixmap(22, 22));
    statusBar()->showMessage(details.mounted ? i18n("Image is mounted") : i18n("Image is ready"), 4000);
    updateActions();
    loadUsage(path);
}

void DwarfsManagerWindow::showInlineError(const QString &message)
{
    m_error->setText(message.isEmpty() ? i18n("Image details could not be read.") : message);
    m_error->show();
    m_state->setText(i18n("Unavailable"));
    m_stateIcon->setPixmap(QIcon::fromTheme(QStringLiteral("dialog-warning")).pixmap(22, 22));
    m_mountFrame->hide();
    m_compressionOverview->setValues(-1, -1);
    m_contentRings->setError(message);
    statusBar()->showMessage(i18n("Could not read image details"), 5000);
    updateActions();
}

void DwarfsManagerWindow::setBusy(bool busy, const QString &message)
{
    m_busy = busy;
    m_progress->setVisible(busy);
    if (busy) {
        m_progress->setRange(0, 0);
        if (!message.isEmpty()) statusBar()->showMessage(message);
    }
    updateActions();
}

void DwarfsManagerWindow::updateActions()
{
    const bool haveImage = !currentImage().isEmpty();
    const bool enabled = haveImage && !m_busy;
    const bool mounted = enabled && m_currentStatus.mounted;
    m_mountAction->setEnabled(enabled);
    m_mountAction->setText(mounted ? i18n("Unmount") : i18n("Mount"));
    m_mountAction->setIcon(QIcon::fromTheme(mounted ? QStringLiteral("media-eject") : QStringLiteral("media-mount")));
    m_browseAction->setEnabled(enabled);
    m_extractAction->setEnabled(enabled);
    m_verifyAction->setEnabled(enabled);
    m_checksumAction->setEnabled(enabled);
    const QStringList recent = rememberedImages();
    m_forgetAction->setEnabled(haveImage && recent.contains(currentImage()));
    m_clearRecentAction->setEnabled(!recent.isEmpty());
    if (m_primaryButton) {
        m_primaryButton->setEnabled(enabled);
        m_primaryButton->setText(mounted ? i18n("Unmount") : i18n("Mount"));
        m_primaryButton->setIcon(QIcon::fromTheme(mounted ? QStringLiteral("media-eject") : QStringLiteral("media-mount")));
        m_browseButton->setEnabled(enabled);
        m_browseButton->setText(i18n("Open in Dolphin"));
        m_browseButton->setIcon(QIcon::fromTheme(QStringLiteral("system-file-manager")));
    }
}

void DwarfsManagerWindow::mountOrUnmount()
{
    const QString image = currentImage();
    if (image.isEmpty()) return;
    dwarfsplugin::ensureTrayRunning();
    const bool mounted = m_currentStatus.mounted && !m_currentStatus.mountpoint.isEmpty();
    const QString method = mounted ? QStringLiteral("UnmountByMountpoint") : QStringLiteral("Mount");
    const QList<QVariant> arguments = mounted ? QList<QVariant>{m_currentStatus.mountpoint} : QList<QVariant>{image};
    setBusy(true, mounted ? i18n("Unmounting image…") : i18n("Mounting image…"));
    auto *job = new dwarfsplugin::TrayAgentJob(
        {mounted ? i18n("Unmount image") : i18n("Mount image"), method, arguments, DwarfsctlRunner::defaultTimeoutMs()}, this);
    KJobWidgets::setWindow(job, this);
    connect(job, &KJob::result, this, [this, image, mounted](KJob *base) {
        auto *mountJob = qobject_cast<dwarfsplugin::TrayAgentJob *>(base);
        setBusy(false);
        if (!mountJob) return;
        const auto envelope = mountJob->envelope();
        if (mountJob->error() != 0) {
            dwarfsplugin::criticalModalWithDetails(this,
                mounted ? i18n("Could Not Unmount Image") : i18n("Could Not Mount Image"),
                envelope.error.message.isEmpty() ? mountJob->errorText() : envelope.error.message,
                envelope);
            return;
        }
        rememberImage(image);
        const QString mountpoint = mountpointFromEnvelope(envelope);
        loadStatus(image);
        if (!mounted && DwarfsctlRunner::openAfterMount() && !mountpoint.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(mountpoint));
        }
    });
    job->start();
}

void DwarfsManagerWindow::browseImage()
{
    const QString image = currentImage();
    if (image.isEmpty()) return;
    if (m_currentStatus.mounted && !m_currentStatus.mountpoint.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_currentStatus.mountpoint));
        return;
    }
    QUrl url;
    url.setScheme(QStringLiteral("dwarfs"));
    url.setPath(image, QUrl::DecodedMode);
    QDesktopServices::openUrl(url);
}

void DwarfsManagerWindow::extractImage()
{
    const QString image = currentImage();
    if (image.isEmpty()) return;
    const QFileInfo imageInfo(image);
    const QString parent = QFileDialog::getExistingDirectory(this, i18n("Choose a Folder for the Extracted Image"), imageInfo.absolutePath());
    if (parent.isEmpty()) return;
    QString name = imageInfo.completeBaseName();
    if (name.isEmpty()) name = i18n("Extracted DwarFS Image");
    QString destination = QDir(parent).filePath(name);
    for (int suffix = 2; QFileInfo::exists(destination) || QFileInfo(destination).isSymLink(); ++suffix) {
        destination = QDir(parent).filePath(i18n("%1 (%2)", name, suffix));
    }
    runImageJob(i18n("Extract %1", imageInfo.fileName()),
                {QStringLiteral("extract"), image, destination},
                DwarfsctlRunner::buildTimeoutMs(),
                [this, destination, name](const dwarfsplugin::Envelope &) {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(destination));
                    dwarfsplugin::notifyResult(this, dwarfsplugin::NotifyKind::Info,
                                               i18n("Extraction Complete"),
                                               i18n("%1 was extracted successfully.", name));
                });
}

void DwarfsManagerWindow::verifyImage()
{
    if (currentImage().isEmpty()) return;
    dwarfsplugin::IntegrityCheckDialog dialog(currentImage(), this);
    dialog.exec();
}

void DwarfsManagerWindow::checksums()
{
    if (currentImage().isEmpty()) return;
    dwarfsplugin::ChecksumDialog dialog(currentImage(), this);
    dialog.exec();
}

void DwarfsManagerWindow::showSettings()
{
    SettingsDialog dialog(this);
    dialog.exec();
}

void DwarfsManagerWindow::copyMountpoint()
{
    if (m_currentStatus.mountpoint.isEmpty()) return;
    QApplication::clipboard()->setText(m_currentStatus.mountpoint);
    statusBar()->showMessage(i18n("Mount location copied"), 3000);
}

void DwarfsManagerWindow::createImage()
{
    const QString source = QFileDialog::getExistingDirectory(this, i18n("Choose a Folder to Turn Into a DwarFS Image"));
    if (source.isEmpty()) return;
    const auto options = dwarfsplugin::promptBuildOptions(this, source);
    if (!options.accepted) return;
    QStringList arguments{QStringLiteral("build"), QStringLiteral("--output"), options.outputPath};
    for (const QString &argument : options.profileArgs) {
        const QString trimmed = argument.trimmed();
        if (!trimmed.isEmpty()) arguments << QStringLiteral("--mkdwarfs-arg") << trimmed;
    }
    if (options.force) arguments << QStringLiteral("--force");
    if (options.overwriteOutput) arguments << QStringLiteral("--overwrite-output");
    if (options.yes) arguments << QStringLiteral("--yes");
    if (options.dryRun) arguments << QStringLiteral("--dry-run");
    arguments << source;

    auto *feedback = new dwarfsplugin::BuildFeedback(options.outputPath, this);
    auto *job = new dwarfsplugin::DwarfsctlJob(
        {i18n("Create DwarFS Image"), arguments, DwarfsctlRunner::buildTimeoutMs()}, this);
    KJobWidgets::setWindow(job, this);
    connect(job, &dwarfsplugin::DwarfsctlJob::buildPhase, feedback, &dwarfsplugin::BuildFeedback::phase);
    connect(feedback, &dwarfsplugin::BuildFeedback::cancelRequested,
            job, [job] { job->kill(KJob::EmitResult); });
    connect(job, &KJob::result, this, [this, job, feedback, output = options.outputPath](KJob *) {
        if (job->error() != 0) {
            feedback->failed();
            if (job->error() == KJob::KilledJobError) return;
            const auto envelope = job->envelope();
            dwarfsplugin::criticalModalWithDetails(this, i18n("Could Not Create Image"),
                envelope.error.message.isEmpty() ? job->errorText() : envelope.error.message, envelope);
            return;
        }
        feedback->complete(job->envelope());
        if (QFileInfo::exists(output)) addImage(output);
    });
    job->start();
}

void DwarfsManagerWindow::runImageJob(
    const QString &title,
    const QStringList &arguments,
    int timeout,
    const std::function<void(const dwarfsplugin::Envelope &)> &onSuccess)
{
    setBusy(true, title);
    auto *job = new dwarfsplugin::DwarfsctlJob({title, arguments, timeout}, this);
    KJobWidgets::setWindow(job, this);
    connect(job, &KJob::result, this, [this, job, title, onSuccess](KJob *) {
        setBusy(false);
        const auto envelope = job->envelope();
        if (job->error() != 0) {
            if (job->error() == KJob::KilledJobError) return;
            dwarfsplugin::criticalModalWithDetails(this, title,
                envelope.error.message.isEmpty() ? job->errorText() : envelope.error.message,
                envelope);
            return;
        }
        onSuccess(envelope);
        loadStatus(currentImage());
    });
    job->start();
}
