// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fileidentity.h"
#include <QPointer>
#include <QStringList>
#include <QWidget>

class QAction;
class QLabel;
class QLineEdit;
class ImageEntryModel;
class QSortFilterProxyModel;
class QToolButton;
class QTreeView;
namespace dwarfsplugin { class DwarfsctlJob; class BackendReadRetry; }

class ImageBrowserWidget final : public QWidget {
    Q_OBJECT
public:
    explicit ImageBrowserWidget(QWidget *parent = nullptr);
    ~ImageBrowserWidget() override;
    void setImage(const QString &path);
    QString imagePath() const { return m_image; }
    QString currentDirectory() const { return m_directory; }
    QStringList selectedPaths() const;
    // False while loading, after a failure, or if the image changed on disk.
    bool isReady() const;
    void setExtractAction(QAction *action);
Q_SIGNALS:
    void selectionChanged();
    void fileActivated(const QString &innerPath);
private:
    void navigate(const QString &path, bool remember = true);
    void request();
    void cancel();
    void fail(const QString &message);
    void updateNavigation();
    std::optional<dwarfsctl_kde::FileIdentity> identity() const;
    QString m_image;
    std::optional<dwarfsctl_kde::FileIdentity> m_identity;
    QString m_directory = QStringLiteral("/");
    QStringList m_history;
    dwarfsplugin::BackendReadRetry *m_readRetry = nullptr;
    quint64 m_generation = 0;
    bool m_ready = false;
    QPointer<dwarfsplugin::DwarfsctlJob> m_job;
    QTreeView *m_view;
    ImageEntryModel *m_model;
    QSortFilterProxyModel *m_proxy;
    QLineEdit *m_path;
    QLabel *m_state;
    QToolButton *m_back;
    QToolButton *m_up;
    QToolButton *m_retry;
    QToolButton *m_extract;
};
