// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dwarfsctljson.h"
#include "dwarfsctlmodel.h"

#include <QMainWindow>
#include <QPointer>
#include <QStringList>


class QAction;
class CompressionOverviewWidget;
class ContentRingsWidget;
class ElidingLabel;
class ImageBrowserWidget;
class QFrame;
class QLabel;
class KMessageWidget;
class QListWidget;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTabWidget;

namespace dwarfsplugin {
class DwarfsctlJob;
class BackendReadRetry;
}

class DwarfsManagerWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit DwarfsManagerWindow(QWidget *parent = nullptr);
    void openImages(const QStringList &paths);

private Q_SLOTS:
    void chooseImages();
    void createImage();
    void selectionChanged();
    void refreshImages();
    void forgetCurrentImage();
    void clearRecentImages();
    void locateImage();
    void showImageInFolder();
    void showImageContextMenu(const QPoint &position);
    void mountOrUnmount();
    void browseImage();
    void extractImage();
    void extractSelected();
    void openImageFile(const QString &innerPath);
    void verifyImage();
    void checksums();
    void showSettings();
    void copyMountpoint();

private:
    QString currentImage() const;
    void startExtraction(const QStringList &paths);
    void loadFileBrowser();
    void addImage(const QString &path, bool select = true);
    void rememberImage(const QString &path);
    QStringList rememberedImages() const;
    void loadStatus(const QString &path);
    void loadUsage(const QString &path);
    void showStatus(const QString &path,
                    const dwarfsplugin::Envelope &envelope,
                    const dwarfsplugin::StatusDetails &details);
    void showInlineError(const QString &message);
    void setBusy(bool busy, const QString &message = {});
    void updateActions();
    void showUnavailableImage();


    QListWidget *m_images = nullptr;
    QStackedWidget *m_pages = nullptr;
    QLabel *m_emptyMessage = nullptr;
    QLabel *m_sidebarSummary = nullptr;
    ElidingLabel *m_name = nullptr;
    ElidingLabel *m_path = nullptr;
    QLabel *m_stateIcon = nullptr;
    QLabel *m_state = nullptr;
    QLabel *m_contents = nullptr;
    QLabel *m_imageSize = nullptr;
    QLabel *m_originalSize = nullptr;
    QLabel *m_saved = nullptr;
    QLabel *m_format = nullptr;
    QLabel *m_created = nullptr;
    QLabel *m_createdBy = nullptr;
    QFrame *m_mountFrame = nullptr;
    ElidingLabel *m_mountpoint = nullptr;
    QTabWidget *m_imageViews = nullptr;
    CompressionOverviewWidget *m_compressionOverview = nullptr;
    ContentRingsWidget *m_contentRings = nullptr;
    ImageBrowserWidget *m_fileBrowser = nullptr;
    KMessageWidget *m_error = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_primaryButton = nullptr;
    QPushButton *m_browseButton = nullptr;
    QAction *m_mountAction = nullptr;
    QAction *m_browseAction = nullptr;
    QAction *m_extractAction = nullptr;
    QAction *m_extractSelectedAction = nullptr;
    QAction *m_verifyAction = nullptr;
    QAction *m_checksumAction = nullptr;
    QAction *m_forgetAction = nullptr;
    QAction *m_clearRecentAction = nullptr;
    QAction *m_locateAction = nullptr;
    QAction *m_showSourceAction = nullptr;
    QStringList m_sessionImages;
    QStringList m_lastHistory;
    QWidget *m_unavailablePage = nullptr;
    QLabel *m_unavailableTitle = nullptr;
    QLabel *m_unavailableMessage = nullptr;
    dwarfsplugin::StatusDetails m_currentStatus;
    dwarfsplugin::Envelope m_currentEnvelope;
    dwarfsplugin::BackendReadRetry *m_statusRetry = nullptr;
    dwarfsplugin::BackendReadRetry *m_usageRetry = nullptr;
    quint64 m_statusGeneration = 0;
    quint64 m_usageGeneration = 0;
    QPointer<dwarfsplugin::DwarfsctlJob> m_statusJob;
    QPointer<dwarfsplugin::DwarfsctlJob> m_usageJob;
    QString m_readIntentPath;
    QString m_statusPath;
    QString m_usageIdentity;
    QString m_usagePath;
    QString m_loadedUsageIdentity;
    bool m_busy = false;
    bool m_operationBusy = false;
    QString m_operationMessage;
};
