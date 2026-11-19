// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "managerwindow.h"

#include "builddialog.h"
#include "buildfeedback.h"
#include "extractionfeedback.h"
#include "imagebrowserwidget.h"
#include "imagehistory.h"
#include "imagelabel.h"
#include "fileidentity.h"
#include "checksumdialog.h"
#include "detailsdialog.h"
#include "dwarfsctljob.h"
#include "backendquery.h"
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
#include <QFileSystemWatcher>
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
#include <QMap>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <KJobWidgets>
#include <KLocalizedString>
#include <KMessageWidget>
#include <KIO/OpenUrlJob>
#include <KIO/OpenFileManagerWindowJob>

namespace {

constexpr auto kPathRole = Qt::UserRole + 1;
constexpr auto kMountedRole = Qt::UserRole + 2;
constexpr auto kMountpointRole = Qt::UserRole + 3;

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
    const auto identity = dwarfsctl_kde::fileIdentity(path);
    const QString canonical = canonicalPath(path);
    if (!identity) return canonical + QStringLiteral("|unavailable");
    return canonical + QLatin1Char('|') + QString::number(identity->device)
        + QLatin1Char('|') + QString::number(identity->inode)
        + QLatin1Char('|') + QString::number(identity->size)
        + QLatin1Char('|') + QString::number(identity->mtimeSeconds)
        + QLatin1Char('|') + QString::number(identity->mtimeNanoseconds)
        + QLatin1Char('|') + QString::number(identity->ctimeSeconds)
        + QLatin1Char('|') + QString::number(identity->ctimeNanoseconds);
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
    m_statusRetry = new dwarfsplugin::BackendReadRetry(this);
    m_usageRetry = new dwarfsplugin::BackendReadRetry(this);
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

    m_extractAction = new QAction(QIcon::fromTheme(QStringLiteral("archive-extract")), i18n("Extract All…"), this);
    m_extractAction->setObjectName(QStringLiteral("extractAction"));
    connect(m_extractAction, &QAction::triggered, this, &DwarfsManagerWindow::extractImage);
    m_extractSelectedAction = new QAction(QIcon::fromTheme(QStringLiteral("archive-extract")), i18n("Extract Selected…"), this);
    m_extractSelectedAction->setObjectName(QStringLiteral("extractSelectedAction"));
    connect(m_extractSelectedAction, &QAction::triggered, this, &DwarfsManagerWindow::extractSelected);

    m_verifyAction = new QAction(QIcon::fromTheme(QStringLiteral("security-high")), i18n("Verify…"), this);
    m_verifyAction->setObjectName(QStringLiteral("verifyAction"));
    connect(m_verifyAction, &QAction::triggered, this, &DwarfsManagerWindow::verifyImage);

    m_checksumAction = new QAction(QIcon::fromTheme(QStringLiteral("document-encrypt")), i18n("Checksums…"), this);
    m_checksumAction->setObjectName(QStringLiteral("checksumAction"));
    connect(m_checksumAction, &QAction::triggered, this, &DwarfsManagerWindow::checksums);

    m_forgetAction = new QAction(QIcon::fromTheme(QStringLiteral("list-remove")), i18n("Remove from Recent Images"), this);
    m_forgetAction->setObjectName(QStringLiteral("forgetImageAction"));
    connect(m_forgetAction, &QAction::triggered, this, &DwarfsManagerWindow::forgetCurrentImage);

    m_locateAction = new QAction(QIcon::fromTheme(QStringLiteral("document-open")), i18n("Locate Image…"), this);
    m_locateAction->setObjectName(QStringLiteral("locateImageAction"));
    connect(m_locateAction, &QAction::triggered, this, &DwarfsManagerWindow::locateImage);
    m_showSourceAction = new QAction(QIcon::fromTheme(QStringLiteral("document-open-folder")), i18n("Show Image in Folder"), this);
    m_showSourceAction->setObjectName(QStringLiteral("showImageInFolderAction"));
    connect(m_showSourceAction, &QAction::triggered, this, &DwarfsManagerWindow::showImageInFolder);
    m_forgetAction->setToolTip(i18n("Remove the list entry without deleting the image or unmounting its volume."));

    m_clearRecentAction = new QAction(i18n("Clear Recent Images"), this);
    m_clearRecentAction->setObjectName(QStringLiteral("clearRecentImagesAction"));
    m_clearRecentAction->setToolTip(i18n("Clear shared recent history. Open images and mounted volumes stay in the list."));
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
    imageMenu->addAction(m_extractSelectedAction);
    imageMenu->addSeparator();
    imageMenu->addAction(m_verifyAction);
    imageMenu->addAction(m_checksumAction);
    imageMenu->addSeparator();
    imageMenu->addAction(m_showSourceAction);
    imageMenu->addAction(m_locateAction);
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

    m_fileBrowser = new ImageBrowserWidget(m_imageViews);
    m_fileBrowser->setExtractAction(m_extractSelectedAction);
    m_imageViews->addTab(m_fileBrowser, QIcon::fromTheme(QStringLiteral("view-list-details")), i18n("Files"));
    connect(m_fileBrowser, &ImageBrowserWidget::selectionChanged, this, &DwarfsManagerWindow::updateActions);
    connect(m_fileBrowser, &ImageBrowserWidget::fileActivated, this, &DwarfsManagerWindow::openImageFile);

    auto *contentsPage = new QWidget(m_imageViews);
    auto *contentsLayout = new QVBoxLayout(contentsPage);
    contentsLayout->setContentsMargins(6, 6, 6, 6);
    m_contentRings = new ContentRingsWidget(contentsPage);
    contentsLayout->addWidget(m_contentRings, 1);
    m_imageViews->addTab(contentsPage, QIcon::fromTheme(QStringLiteral("folder")), i18n("Size Map"));
    {
        QSettings settings = managerSettings();
        const QString selectedView = settings.value(QStringLiteral("view/selected")).toString();
        const int previous = settings.value(QStringLiteral("view/current"), 0).toInt();
        const int index = selectedView == QLatin1String("files") ? 1
            : (selectedView == QLatin1String("size-map") || (selectedView.isEmpty() && previous == 1)) ? 2 : 0;
        m_imageViews->setCurrentIndex(index);
    }
    connect(m_imageViews, &QTabWidget::currentChanged, this, [this](int index) {
        QSettings settings = managerSettings();
        settings.setValue(QStringLiteral("view/selected"), index == 1 ? QStringLiteral("files") : index == 2 ? QStringLiteral("size-map") : QStringLiteral("overview"));
        loadFileBrowser();
        updateActions();
    });
    detailsLayout->addWidget(m_imageViews, 1);
    m_unavailablePage = new QWidget(details);
    m_unavailablePage->setObjectName(QStringLiteral("unavailableImagePage"));
    auto *unavailableLayout = new QVBoxLayout(m_unavailablePage);
    unavailableLayout->setContentsMargins(24, 24, 24, 24);
    unavailableLayout->addStretch(1);
    m_unavailableTitle = new QLabel(m_unavailablePage);
    m_unavailableTitle->setTextFormat(Qt::PlainText);
    m_unavailableTitle->setWordWrap(true);
    m_unavailableTitle->setAlignment(Qt::AlignCenter);
    m_unavailableTitle->setFont(dwarfsctl_kde::headingFont(m_unavailableTitle->font()));
    unavailableLayout->addWidget(m_unavailableTitle);
    m_unavailableMessage = new QLabel(m_unavailablePage);
    m_unavailableMessage->setTextFormat(Qt::PlainText);
    m_unavailableMessage->setWordWrap(true);
    m_unavailableMessage->setAlignment(Qt::AlignCenter);
    unavailableLayout->addWidget(m_unavailableMessage);
    unavailableLayout->addStretch(1);
    detailsLayout->addWidget(m_unavailablePage, 1);
    m_unavailablePage->hide();

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
    auto *historyWatch = new QFileSystemWatcher(this);
    historyWatch->addPath(QFileInfo(dwarfsctl_kde::imageHistoryPath()).absolutePath());
    connect(historyWatch, &QFileSystemWatcher::directoryChanged, this, [this] {
        if (dwarfsctl_kde::recentImages() != m_lastHistory) refreshImages();
    });
    updateActions();
}

void DwarfsManagerWindow::openImages(const QStringList &paths)
{
    for (const QString &path : paths) addImage(path);
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
    const QString canonical = dwarfsctl_kde::canonicalImagePath(path);
    const bool remembered = rememberedImages().contains(canonical);
    if ((!info.isFile() || !info.fileName().endsWith(QStringLiteral(".dwarfs"), Qt::CaseInsensitive)) && !remembered) {
        statusBar()->showMessage(i18n("%1 is not an accessible DwarFS image.", QDir::toNativeSeparators(path)), 6000);
        return;
    }
    m_sessionImages.removeAll(canonical);
    m_sessionImages.prepend(canonical);
    if (info.isFile()) rememberImage(canonical);
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
    if (!dwarfsctl_kde::rememberImage(path))
        statusBar()->showMessage(i18n("Recent images could not be saved."), 5000);
}

QStringList DwarfsManagerWindow::rememberedImages() const
{
    return dwarfsctl_kde::recentImages();
}

void DwarfsManagerWindow::refreshImages()
{
    const QString selected = currentImage();
    const auto rawMounts = dwarfsplugin::MountsCache::instance()->mounts();
    QMap<QString, QString> mounts;
    for (auto it = rawMounts.cbegin(); it != rawMounts.cend(); ++it)
        mounts.insert(dwarfsctl_kde::canonicalImagePath(it.key()), it.value());
    const QStringList recent = rememberedImages();
    m_lastHistory = recent;
    QStringList all = mounts.keys();
    all.append(m_sessionImages);
    all.append(recent);
    all.removeDuplicates();
    QSignalBlocker selectionBlocker(m_images);
    m_images->clear();
    QSet<QString> added;
    int missing = 0;
    const auto appendHeader = [this](const QString &title) {
        auto *item = new QListWidgetItem(title, m_images);
        item->setFlags(Qt::NoItemFlags);
        QFont font = item->font(); font.setBold(true); item->setFont(font);
        item->setForeground(palette().brush(QPalette::PlaceholderText));
    };
    const auto append = [this, &added, &mounts, &all, &missing](const QString &path) {
        if (path.isEmpty() || added.contains(path)) return;
        added.insert(path);
        const QFileInfo info(path);
        const bool available = info.isFile() && info.isReadable();
        const bool mounted = mounts.contains(path);
        if (!available) ++missing;
        QString title = info.completeBaseName();
        if (title.isEmpty()) title = info.fileName();
        const QString parentHint = dwarfsctl_kde::imageParentHint(path, all);
        if (!parentHint.isEmpty()) title += QStringLiteral("\n") + parentHint;
        if (!available) title += mounted ? i18n("\nSource unavailable") : i18n("\nUnavailable");
        auto *item = new QListWidgetItem(QIcon::fromTheme(mounted ? QStringLiteral("dwarfsctl-volume")
            : available ? QStringLiteral("dwarfsctlimage") : QStringLiteral("dialog-warning")), title, m_images);
        item->setData(kPathRole, path);
        item->setData(kMountedRole, mounted);
        item->setData(kMountpointRole, mounts.value(path));
        const QString tooltip = QDir::toNativeSeparators(path)
            + (mounted ? i18n("\nMounted at %1", QDir::toNativeSeparators(mounts.value(path))) : QString());
        item->setToolTip(QStringLiteral("<qt>%1</qt>").arg(tooltip.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>"))));
        item->setStatusTip(path);
    };
    if (!mounts.isEmpty()) {
        appendHeader(i18n("Mounted Volumes"));
        for (auto it = mounts.cbegin(); it != mounts.cend(); ++it) append(it.key());
    }
    const auto appendSection = [&](const QString &title, const QStringList &paths) {
        bool header = false;
        for (const auto &path : paths) {
            if (added.contains(path) || path.isEmpty()) continue;
            if (!header) { appendHeader(title); header = true; }
            append(path);
        }
    };
    appendSection(i18n("Open Images"), m_sessionImages);
    appendSection(i18n("Recent Images"), recent);
    m_sidebarSummary->setText(added.isEmpty() ? i18n("No images added")
        : i18np("1 image", "%1 images", added.size())
            + (missing ? i18n(" · %1 unavailable", missing) : QString()));
    int firstImage = -1;
    int selectedRow = -1;
    for (int row = 0; row < m_images->count(); ++row) {
        const auto path = m_images->item(row)->data(kPathRole).toString();
        if (path.isEmpty()) continue;
        if (firstImage < 0) firstImage = row;
        if (!selected.isEmpty() && path == selected) selectedRow = row;
    }
    m_images->setCurrentRow(selectedRow >= 0 ? selectedRow : firstImage);
    selectionBlocker.unblock();
    selectionChanged();
}

void DwarfsManagerWindow::forgetCurrentImage()
{
    const QString path = currentImage();
    if (path.isEmpty() || m_operationBusy) return;
    if (!dwarfsctl_kde::forgetImage(path)) {
        statusBar()->showMessage(i18n("The image could not be removed from recent history."), 5000);
        return;
    }
    const bool mounted = m_currentStatus.mounted;
    m_sessionImages.removeAll(path);
    refreshImages();
    statusBar()->showMessage(mounted ? i18n("Removed from recent images. The volume remains mounted.")
        : i18n("Removed from the list. The image file is unchanged."), 5000);
}

void DwarfsManagerWindow::clearRecentImages()
{
    if (m_operationBusy) return;
    if (!dwarfsctl_kde::clearRecentImages()) {
        statusBar()->showMessage(i18n("Recent history could not be cleared."), 5000);
        return;
    }
    refreshImages();
    statusBar()->showMessage(i18n("Recent history cleared. Open images and mounted volumes stay in the list."), 5000);
}

void DwarfsManagerWindow::locateImage()
{
    const QString oldPath = currentImage();
    if (!m_locateAction->isEnabled() || oldPath.isEmpty()) return;
    const auto oldIdentity = dwarfsctl_kde::fileIdentity(oldPath);
    const QString picked = QFileDialog::getOpenFileName(this, i18n("Locate %1", QFileInfo(oldPath).fileName()),
        QFileInfo(oldPath).absolutePath(), i18n("DwarFS images (*.dwarfs)"));
    if (picked.isEmpty() || currentImage() != oldPath || m_operationBusy || m_currentStatus.mounted) return;
    if (dwarfsctl_kde::fileIdentity(oldPath) != oldIdentity
        || (QFileInfo(oldPath).isFile() && QFileInfo(oldPath).isReadable())) {
        refreshImages();
        statusBar()->showMessage(i18n("The original image changed while choosing a new location. Its list entry was kept."), 5000);
        return;
    }
    const auto replacementIdentity = dwarfsctl_kde::fileIdentity(picked);
    const QFileInfo info(picked);
    if (!replacementIdentity || !info.isFile() || !info.isReadable() || !info.fileName().endsWith(QStringLiteral(".dwarfs"), Qt::CaseInsensitive)) {
        statusBar()->showMessage(i18n("Choose an accessible DwarFS image."), 5000); return;
    }
    const QString replacement = dwarfsctl_kde::canonicalImagePath(picked);
    if (dwarfsctl_kde::fileIdentity(replacement) != replacementIdentity) {
        statusBar()->showMessage(i18n("The selected image changed. Choose its location again."), 5000); return;
    }
    if (!dwarfsctl_kde::replaceRecentImage(oldPath, replacement)) {
        statusBar()->showMessage(i18n("The image location could not be saved."), 5000); return;
    }
    m_sessionImages.removeAll(oldPath);
    addImage(replacement);
}

void DwarfsManagerWindow::showImageInFolder()
{
    if (!m_showSourceAction->isEnabled()) return;
    auto *job = KIO::highlightInFileManager({QUrl::fromLocalFile(currentImage())});
    KJobWidgets::setWindow(job, this);
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
    menu.addAction(m_showSourceAction);
    menu.addAction(m_locateAction);
    menu.addAction(m_forgetAction);
    menu.exec(m_images->viewport()->mapToGlobal(position));
}

void DwarfsManagerWindow::selectionChanged()
{
    const QString path = currentImage();
    if (m_readIntentPath != path) {
        m_statusRetry->cancel();
        m_usageRetry->cancel();
        m_readIntentPath = path;
    }
    // Stop observing obsolete reads immediately; stop their owned processes on
    // the next event-loop turn so recovery actions never wait for another image.
    if (m_statusJob && m_statusPath != path) {
        const QPointer<dwarfsplugin::DwarfsctlJob> obsolete = m_statusJob;
        ++m_statusGeneration;
        m_statusJob = nullptr;
        m_statusPath.clear();
        m_busy = false;
        QTimer::singleShot(0, this, [obsolete] { if (obsolete) obsolete->kill(KJob::Quietly); });
    }
    if (m_usageJob && m_usagePath != path) {
        const QPointer<dwarfsplugin::DwarfsctlJob> obsolete = m_usageJob;
        ++m_usageGeneration;
        m_usageJob = nullptr;
        m_usagePath.clear();
        m_usageIdentity.clear();
        QTimer::singleShot(0, this, [obsolete] { if (obsolete) obsolete->kill(KJob::Quietly); });
    }
    if (path.isEmpty()) {
        m_fileBrowser->setImage({});
        setBusy(false);
        m_pages->setCurrentIndex(0);
        updateActions();
        return;
    }
    m_pages->setCurrentIndex(1);
    if (m_fileBrowser->imagePath() != path) m_fileBrowser->setImage({});
    m_currentStatus = {};
    m_currentEnvelope = {};
    if (const auto *item = m_images->currentItem()) {
        m_currentStatus.mounted = item->data(kMountedRole).toBool();
        m_currentStatus.mountpoint = item->data(kMountpointRole).toString();
    }
    m_name->setFullText(QFileInfo(path).completeBaseName());
    m_path->setFullText(QDir::toNativeSeparators(path));
    m_mountFrame->hide();
    m_state->setText(m_currentStatus.mounted ? i18n("Mounted") : i18n("Reading…"));
    m_stateIcon->setPixmap(QIcon::fromTheme(m_currentStatus.mounted ? QStringLiteral("dwarfsctl-volume") : QStringLiteral("dwarfsctlimage")).pixmap(22, 22));
    for (auto *label : {m_contents, m_imageSize, m_originalSize, m_saved, m_format, m_created, m_createdBy})
        label->setText(i18n("Reading…"));
    m_compressionOverview->setValues(-1, -1);
    if (m_loadedUsageIdentity != usageIdentity(path)) m_contentRings->setLoading();
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable()) {
        showUnavailableImage();
        return;
    }
    m_unavailablePage->hide();
    m_imageViews->show();
    m_imageViews->setEnabled(true);
    loadStatus(path);
}

void DwarfsManagerWindow::showUnavailableImage()
{
    m_fileBrowser->setImage({});
    m_imageViews->hide();
    m_unavailableTitle->setText(m_currentStatus.mounted ? i18n("Source image unavailable") : i18n("Locate this image to continue"));
    m_unavailableMessage->setText(m_currentStatus.mounted
        ? i18n("Use Open Mounted Folder to access the volume, or Unmount when you have finished.")
        : i18n("Check that its drive is connected, or use Locate Image to choose its new location.\nRemoving it from the list keeps the image file unchanged."));
    m_unavailablePage->show();
    for (auto *action : m_error->actions()) { m_error->removeAction(action); action->deleteLater(); }
    m_error->hide();
    for (auto *label : {m_contents, m_imageSize, m_originalSize, m_saved, m_format, m_created, m_createdBy})
        label->setText(i18n("Unavailable"));
    const bool mounted = m_currentStatus.mounted && !m_currentStatus.mountpoint.isEmpty();
    m_state->setText(mounted ? i18n("Mounted") : i18n("Unavailable"));
    m_stateIcon->setPixmap(QIcon::fromTheme(mounted ? QStringLiteral("dwarfsctl-volume") : QStringLiteral("dialog-warning")).pixmap(22, 22));
    m_mountpoint->setFullText(mounted ? QDir::toNativeSeparators(m_currentStatus.mountpoint) : QString());
    m_mountFrame->setVisible(mounted);
    m_contentRings->setError(i18n("The image file is unavailable."));
    setBusy(false);
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
    const auto generation = ++m_statusGeneration;
    m_statusPath = path;
    m_statusJob = dwarfsplugin::BackendQuery::ui(this,
        [this, path, generation](bool ok, const dwarfsctl_kde::UiConfig &config, const QString &error) {
        if (m_statusGeneration != generation || currentImage() != path) return;
        if (!ok) {
            m_statusJob = nullptr;
            m_statusPath.clear();
            setBusy(false); showInlineError(error);
            return;
        }
        auto *job = new dwarfsplugin::DwarfsctlJob(
            {i18n("Read image details"), {QStringLiteral("status"), path}, config.command_timeout_ms, false}, this);
        m_statusJob = job;
        m_statusPath = path;
        const auto identity = dwarfsctl_kde::fileIdentity(path);
        connect(job, &KJob::result, this, [this, path, job, identity](KJob *base) {
            auto *statusJob = qobject_cast<dwarfsplugin::DwarfsctlJob *>(base);
            if (m_statusJob != job) return;
            m_statusJob = nullptr;
            m_statusPath.clear();
            if (!statusJob || currentImage() != path) {
                setBusy(false);
                return;
            }
            setBusy(false);
            if (identity != dwarfsctl_kde::fileIdentity(path)) {
                selectionChanged();
                return;
            }
            const auto envelope = statusJob->envelope();
            if (statusJob->error() != 0) {
                if (m_statusRetry->schedule(envelope, [this, path] { if (currentImage() == path) loadStatus(path); })) {
                    setBusy(true, i18n("Waiting for the previous image read to stop…"));
                    return;
                }
                showInlineError(envelope.error.message.isEmpty() ? statusJob->errorText() : envelope.error.message);
                return;
            }
            m_statusRetry->cancel();
            dwarfsplugin::StatusDetails status;
            const auto decoded = dwarfsplugin::decodeStatus(envelope, &status);
            if (!decoded.ok) {
                showInlineError(decoded.error_message);
                return;
            }
            showStatus(path, envelope, status);
        });
        job->start();
    });
}

void DwarfsManagerWindow::loadUsage(const QString &path)
{
    const QString identity = usageIdentity(path);
    if (m_loadedUsageIdentity == identity && m_contentRings->hasUsage()) return;
    if (m_usageJob && m_usageIdentity == identity) return;
    m_contentRings->setLoading();
    const auto generation = ++m_usageGeneration;
    m_usagePath = path;
    m_usageIdentity = identity;
    m_usageJob = dwarfsplugin::BackendQuery::ui(this,
        [this, path, generation, identity](bool ok, const dwarfsctl_kde::UiConfig &config, const QString &error) {
        if (m_usageGeneration != generation || currentImage() != path) return;
        if (!ok) {
            m_usageJob = nullptr;
            m_usagePath.clear();
            m_usageIdentity.clear(); m_contentRings->setError(error);
            return;
        }
        auto *job = new dwarfsplugin::DwarfsctlJob(
            {i18n("Read image contents"), {QStringLiteral("usage"), path}, config.command_timeout_ms, false}, this);
        m_usageJob = job;
        m_usagePath = path;
        m_usageIdentity = identity;
        connect(job, &KJob::result, this, [this, path, identity, job](KJob *base) {
            auto *usageJob = qobject_cast<dwarfsplugin::DwarfsctlJob *>(base);
            if (m_usageJob != job) return;
            m_usageJob = nullptr;
            m_usagePath.clear();
            m_usageIdentity.clear();
            if (!usageJob || currentImage() != path || usageIdentity(path) != identity) return;
            const auto envelope = usageJob->envelope();
            if (usageJob->error() != 0) {
                if (m_usageRetry->schedule(envelope, [this, path] { if (currentImage() == path) loadUsage(path); })) return;
                m_contentRings->setError(envelope.error.message.isEmpty() ? usageJob->errorText() : envelope.error.message);
                return;
            }
            m_usageRetry->cancel();
            if (!m_contentRings->setUsage(envelope.details)) return;
            m_loadedUsageIdentity = identity;
        });
        job->start();
    });
}

void DwarfsManagerWindow::showStatus(const QString &path,
                                     const dwarfsplugin::Envelope &envelope,
                                     const dwarfsplugin::StatusDetails &details)
{
    m_currentStatus = details;
    m_currentEnvelope = envelope;
    m_error->setMessageType(KMessageWidget::Error);
    m_error->hide();
    for (auto *action : m_error->actions()) { m_error->removeAction(action); action->deleteLater(); }
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
    if (m_operationBusy) statusBar()->showMessage(m_operationMessage);
    else statusBar()->showMessage(details.mounted ? i18n("Image is mounted") : i18n("Image is ready"), 4000);
    updateActions();
    loadFileBrowser();
    loadUsage(path);
}

void DwarfsManagerWindow::showInlineError(const QString &message)
{
    m_error->setMessageType(KMessageWidget::Error);
    for (auto *action : m_error->actions()) { m_error->removeAction(action); action->deleteLater(); }
    auto *retry = new QAction(i18n("Retry"), m_error);
    m_error->addAction(retry);
    connect(retry, &QAction::triggered, this, [this] { selectionChanged(); });
    m_error->setText(message.isEmpty() ? i18n("Image details could not be read.") : message);
    m_error->show();
    m_fileBrowser->setImage({});
    for (auto *label : {m_contents, m_imageSize, m_originalSize, m_saved, m_format, m_created, m_createdBy})
        label->setText(i18n("Unavailable"));
    const bool mounted = m_currentStatus.mounted && !m_currentStatus.mountpoint.isEmpty();
    m_state->setText(mounted ? i18n("Mounted") : i18n("Unavailable"));
    m_stateIcon->setPixmap(QIcon::fromTheme(mounted ? QStringLiteral("dwarfsctl-volume") : QStringLiteral("dialog-warning")).pixmap(22, 22));
    m_mountpoint->setFullText(mounted ? QDir::toNativeSeparators(m_currentStatus.mountpoint) : QString());
    m_mountFrame->setVisible(mounted);
    m_compressionOverview->setValues(-1, -1);
    m_contentRings->setError(message);
    statusBar()->showMessage(i18n("Could not read image details"), 5000);
    updateActions();
}

void DwarfsManagerWindow::setBusy(bool busy, const QString &message)
{
    m_busy = busy;
    m_progress->setVisible(busy || m_operationBusy || m_statusJob);
    if (busy) {
        m_progress->setRange(0, 0);
        if (!message.isEmpty()) statusBar()->showMessage(message);
    }
    updateActions();
}

void DwarfsManagerWindow::updateActions()
{
    const QString image = currentImage();
    const bool haveImage = !image.isEmpty();
    const bool idle = !m_busy && !m_operationBusy && !m_statusJob;
    const QFileInfo info(image);
    const bool source = haveImage && info.isFile() && info.isReadable();
    const bool ready = haveImage && source && m_currentEnvelope.ok;
    const bool mounted = haveImage && m_currentStatus.mounted && !m_currentStatus.mountpoint.isEmpty();
    if (auto *create = findChild<QAction *>(QStringLiteral("createImageAction"))) create->setEnabled(idle);
    m_mountAction->setEnabled(idle && (mounted || ready));
    m_mountAction->setText(mounted ? i18n("Unmount") : i18n("Mount"));
    m_mountAction->setIcon(QIcon::fromTheme(mounted ? QStringLiteral("media-eject") : QStringLiteral("media-mount")));
    m_browseAction->setEnabled(idle && (mounted || ready));
    m_browseAction->setText(mounted ? i18n("Open Mounted Folder") : i18n("Browse Image in Dolphin"));
    m_extractAction->setEnabled(idle && ready);
    m_fileBrowser->setEnabled(idle && ready);
    m_extractSelectedAction->setEnabled(idle && ready && m_imageViews->currentIndex() == 1
        && m_fileBrowser->imagePath() == image && m_fileBrowser->isReady() && !m_fileBrowser->selectedPaths().isEmpty());
    m_verifyAction->setEnabled(idle && source);
    m_checksumAction->setEnabled(idle && source);
    m_locateAction->setEnabled(idle && haveImage && !source && !mounted);
    m_showSourceAction->setEnabled(idle && haveImage && info.isFile());
    const QStringList recent = rememberedImages();
    m_forgetAction->setEnabled(!m_operationBusy && haveImage && (recent.contains(image) || m_sessionImages.contains(image)));
    m_forgetAction->setText(mounted ? i18n("Remove from Recent Images") : i18n("Remove from List"));
    m_clearRecentAction->setEnabled(!m_operationBusy && !recent.isEmpty());
    if (m_primaryButton) {
        m_primaryButton->setEnabled(m_mountAction->isEnabled() || m_locateAction->isEnabled());
        m_primaryButton->setText(m_locateAction->isEnabled() ? i18n("Locate Image…") : m_mountAction->text());
        m_primaryButton->setIcon(QIcon::fromTheme(m_locateAction->isEnabled() ? QStringLiteral("document-open")
            : mounted ? QStringLiteral("media-eject") : QStringLiteral("media-mount")));
        m_browseButton->setEnabled(m_browseAction->isEnabled());
        m_browseButton->setText(m_browseAction->text());
        m_browseButton->setIcon(QIcon::fromTheme(QStringLiteral("system-file-manager")));
    }
}

void DwarfsManagerWindow::mountOrUnmount()
{
    if (m_locateAction->isEnabled()) { locateImage(); return; }
    const QString image = currentImage();
    if (!m_mountAction->isEnabled() || image.isEmpty() || m_operationBusy || m_busy) return;
    const bool mounted = m_currentStatus.mounted && !m_currentStatus.mountpoint.isEmpty();
    const QString method = mounted ? QStringLiteral("UnmountByMountpoint") : QStringLiteral("Mount");
    const QList<QVariant> arguments = mounted ? QList<QVariant>{m_currentStatus.mountpoint} : QList<QVariant>{image};
    m_operationBusy = true;
    m_operationMessage = mounted ? i18n("Unmounting image…") : i18n("Mounting image…");
    setBusy(true, m_operationMessage);
    dwarfsplugin::BackendQuery::ui(this,
        [this, image, mounted, method, arguments](bool ok, const dwarfsctl_kde::UiConfig &config, const QString &error) {
        if (!ok) {
            m_operationBusy = false;
            setBusy(false);
            showInlineError(error);
            return;
        }
        auto *job = new dwarfsplugin::TrayAgentJob(
            {mounted ? i18n("Unmount image") : i18n("Mount image"), method, arguments, config.command_timeout_ms}, this);
        KJobWidgets::setWindow(job, this);
        connect(job, &KJob::result, this, [this, image, mounted, config](KJob *base) {
            auto *mountJob = qobject_cast<dwarfsplugin::TrayAgentJob *>(base);
            m_operationBusy = false;
            setBusy(false);
            if (!mountJob) return;
            const auto envelope = mountJob->envelope();
            if (mountJob->error() != 0) {
                {
                    m_error->setMessageType(KMessageWidget::Error);
                    const QString message = envelope.error.message.isEmpty() ? mountJob->errorText() : envelope.error.message;
                    m_error->setText(i18n("%1: %2", QFileInfo(image).fileName(), message));
                    for (auto *action : m_error->actions()) { m_error->removeAction(action); action->deleteLater(); }
                    auto *details = new QAction(i18n("Details…"), m_error);
                    m_error->addAction(details);
                    connect(details, &QAction::triggered, this, [this, envelope] {
                        dwarfsplugin::DetailsDialog dialog(envelope, i18n("Mount Operation Details"), this);
                        dialog.exec();
                    });
                    m_error->show();
                    statusBar()->showMessage(envelope.error.kind == QLatin1String("timeout")
                        ? i18n("Mount state is unconfirmed. Check the current state before trying again.")
                        : i18n("The mount operation could not be completed."));
                }
                return;
            }
            rememberImage(image);
            const QString mountpoint = mountpointFromEnvelope(envelope);
            if (!currentImage().isEmpty()) loadStatus(currentImage());
            if (!mounted && config.open_after_mount && !mountpoint.isEmpty()) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(mountpoint));
            }
        });
        job->start();
    });
}

void DwarfsManagerWindow::browseImage()
{
    if (!m_browseAction->isEnabled()) return;
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

void DwarfsManagerWindow::loadFileBrowser()
{
    if (m_imageViews->currentIndex() == 1 && !m_busy && !m_operationBusy && m_currentEnvelope.ok
        && !currentImage().isEmpty()) m_fileBrowser->setImage(currentImage());
}

void DwarfsManagerWindow::openImageFile(const QString &innerPath)
{
    if (m_busy || m_operationBusy || !m_fileBrowser->isReady() || m_fileBrowser->imagePath() != currentImage()) return;
    QUrl url;
    url.setScheme(QStringLiteral("dwarfs"));
    url.setPath(currentImage() + innerPath, QUrl::DecodedMode);
    auto *job = new KIO::OpenUrlJob(url, this);
    job->setRunExecutables(false);
    KJobWidgets::setWindow(job, this);
    connect(job, &KJob::result, this, [this, job] {
        if (job->error()) {
            m_error->setText(job->errorText());
            m_error->show();
        }
    });
    job->start();
}

void DwarfsManagerWindow::extractImage()
{
    startExtraction({});
}

void DwarfsManagerWindow::extractSelected()
{
    if (!m_extractSelectedAction->isEnabled() || m_busy || m_operationBusy) return;
    if (!m_fileBrowser->isReady() || m_fileBrowser->imagePath() != currentImage()) {
        m_fileBrowser->setImage(currentImage());
        statusBar()->showMessage(i18n("The image changed. Select the items again."), 5000);
        updateActions();
        return;
    }
    const QStringList paths = m_fileBrowser->selectedPaths();
    if (paths.isEmpty()) return;
    startExtraction(paths);
}

void DwarfsManagerWindow::startExtraction(const QStringList &paths)
{
    const QString image = currentImage();
    if (m_busy || m_operationBusy || image.isEmpty()) return;
    const QFileInfo imageInfo(image);
    const auto imageIdentity = dwarfsctl_kde::fileIdentity(image);
    const QString parent = QFileDialog::getExistingDirectory(this,
        paths.isEmpty() ? i18n("Choose a Folder for the Extracted Image")
                        : i18n("Choose a Folder for the Selected Items"), imageInfo.absolutePath());
    if (parent.isEmpty()) return;
    if (currentImage() != image || !imageIdentity
        || dwarfsctl_kde::fileIdentity(image) != imageIdentity
        || (!paths.isEmpty() && !m_fileBrowser->isReady())) {
        m_error->setText(i18n("The image changed while choosing a destination. Select the items again."));
        m_error->show();
        loadStatus(currentImage());
        return;
    }
    QString name = imageInfo.completeBaseName();
    if (name.isEmpty()) name = i18n("Extracted DwarFS Image");
    QString destination = QDir(parent).filePath(name);
    for (int suffix = 2; QFileInfo::exists(destination) || QFileInfo(destination).isSymLink(); ++suffix)
        destination = QDir(parent).filePath(i18n("%1 (%2)", name, suffix));
    QStringList args{QStringLiteral("extract"), image, destination};
    for (const QString &path : paths) args << QStringLiteral("--path") << path;
    m_operationBusy = true;
    m_operationMessage = i18n("Extracting…");
    setBusy(true, m_operationMessage);
    dwarfsplugin::BackendQuery::ui(this,
        [this, destination, paths, args](bool ok, const dwarfsctl_kde::UiConfig &config, const QString &error) {
        if (!ok) {
            m_operationBusy = false;
            setBusy(false);
            showInlineError(error);
            return;
        }
        const QPointer<ExtractionFeedback> feedback = new ExtractionFeedback(destination, !paths.isEmpty(), this);
        const QPointer<dwarfsplugin::DwarfsctlJob> job = new dwarfsplugin::DwarfsctlJob(
            {i18n("Extract Image"), args, config.build_timeout_ms, false}, this);
        KJobWidgets::setWindow(job, this);
        connect(feedback, &ExtractionFeedback::cancelRequested, this, [job] {
            if (job) job->kill(KJob::EmitResult);
        }, Qt::QueuedConnection);
        connect(job, &KJob::result, this, [this, job, feedback] {
            m_operationBusy = false;
            setBusy(false);
            if (!job || !feedback) return;
            if (job->error() == KJob::KilledJobError) feedback->cancelled();
            else if (job->error()) {
                const auto env = job->envelope();
                feedback->failed(env.error.message.isEmpty() ? job->errorText() : env.error.message, env);
            } else feedback->complete(job->envelope());
        });
        feedback->show();
        job->start();
    });
}

void DwarfsManagerWindow::verifyImage()
{
    if (!m_verifyAction->isEnabled()) return;
    dwarfsplugin::IntegrityCheckDialog dialog(currentImage(), this);
    dialog.exec();
}

void DwarfsManagerWindow::checksums()
{
    if (!m_checksumAction->isEnabled()) return;
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
    if (m_busy || m_operationBusy) return;
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
    if (options.preview) arguments << QStringLiteral("--preview");
    arguments << source;

    m_operationBusy = true;
    m_operationMessage = options.preview ? i18n("Measuring image…") : i18n("Creating image…");
    setBusy(true, m_operationMessage);
    dwarfsplugin::BackendQuery::ui(this,
        [this, options, source, arguments](bool ok, const dwarfsctl_kde::UiConfig &config, const QString &error) {
        if (!ok) {
            m_operationBusy = false;
            setBusy(false);
            showInlineError(error);
            return;
        }
        const QPointer<dwarfsplugin::BuildFeedback> feedback = new dwarfsplugin::BuildFeedback(options.outputPath, this, options.preview);
        QStringList jobArguments = arguments;
        if (options.preview) {
            if (feedback->previewStage().isEmpty()) {
                m_operationBusy = false;
                setBusy(false);
                feedback->failed(i18n("Could not prepare a private location for the measured image."));
                return;
            }
            jobArguments.insert(jobArguments.size() - 1, QStringLiteral("--preview-stage"));
            jobArguments.insert(jobArguments.size() - 1, feedback->previewStage());
            connect(feedback, &dwarfsplugin::BuildFeedback::createRequested, feedback,
                    [this, feedback, output = options.outputPath, source, force = options.force,
                     timeout = config.build_timeout_ms](const QString &candidate, qint64 size) {
                if (m_operationBusy) {
                    feedback->publishFailed(i18n("Finish the current operation before creating this image."));
                    return;
                }
                m_operationBusy = true;
                m_operationMessage = i18n("Saving measured image…");
                setBusy(true, m_operationMessage);
                QStringList publishArgs{QStringLiteral("build"), QStringLiteral("--output"), output,
                                        QStringLiteral("--publish-preview"), candidate,
                                        QStringLiteral("--expected-size"), QString::number(size)};
                if (force) publishArgs << QStringLiteral("--force");
                publishArgs << source;
                auto *publishJob = new dwarfsplugin::DwarfsctlJob(
                    {i18n("Create DwarFS Image"), publishArgs, timeout, false}, feedback);
                KJobWidgets::setWindow(publishJob, this);
                connect(publishJob, &dwarfsplugin::DwarfsctlJob::buildPhase, feedback,
                        &dwarfsplugin::BuildFeedback::phase);
                connect(publishJob, &KJob::result, feedback, [this, feedback, publishJob, output](KJob *) {
                    m_operationBusy = false;
                    setBusy(false);
                    if (!feedback) return;
                    const auto envelope = publishJob->envelope();
                    if (publishJob->error() != 0) {
                        feedback->publishFailed(envelope.error.message.isEmpty() ? publishJob->errorText() : envelope.error.message,
                                                envelope);
                        return;
                    }
                    feedback->published(envelope);
                    if (QFileInfo::exists(output)) addImage(output);
                });
                publishJob->start();
            });
        }
        auto *job = new dwarfsplugin::DwarfsctlJob(
            {options.preview ? i18n("Measure DwarFS Image") : i18n("Create DwarFS Image"), jobArguments, config.build_timeout_ms, false}, this);
        KJobWidgets::setWindow(job, this);
        connect(job, &dwarfsplugin::DwarfsctlJob::buildPhase, feedback, &dwarfsplugin::BuildFeedback::phase);
        connect(feedback, &dwarfsplugin::BuildFeedback::cancelRequested,
                job, [job] { job->kill(KJob::EmitResult); }, Qt::QueuedConnection);
        connect(job, &KJob::result, this, [this, job, feedback, output = options.outputPath, preview = options.preview](KJob *) {
            m_operationBusy = false;
            setBusy(false);
            if (!feedback) return;
            if (job->error() == KJob::KilledJobError) { feedback->cancelled(); return; }
            if (job->error() != 0) {
                const auto envelope = job->envelope();
                feedback->failed(envelope.error.message.isEmpty() ? job->errorText() : envelope.error.message, envelope);
                return;
            }
            feedback->complete(job->envelope());
            if (!preview && QFileInfo::exists(output)) addImage(output);
        });
        job->start();
    });
}
