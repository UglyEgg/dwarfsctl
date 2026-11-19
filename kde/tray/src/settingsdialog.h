// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KPageDialog>
#include <QString>
#include <QStringList>
#include <QList>
#include <QJsonObject>

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class KMessageWidget;
class QSpinBox;
class QPushButton;
class QLabel;
class QWidget;

class SettingsDialog final : public KPageDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);

protected:
    void accept() override;

private:
    void loadSettings(bool preserveEdits = false);
    void loadProfiles(const QString& selectedProfile);
    bool applyProfiles(const QJsonObject &details, const QString &selection);
    void saveSettings();
    void setBusy(bool busy, const QString &message = {});
    void openProfileEditor();
    void showError(const QString& message);
    void updateCreationControls();

    bool m_ready = false;
    bool m_busy = false;
    bool m_needsReload = false;
    QObject *m_queryContext = nullptr;
    QAction *m_retry = nullptr;
    QList<QWidget *> m_backendPages;
    QStringList m_profiles;
    KMessageWidget* m_message = nullptr;
    QCheckBox* m_openAfterMount = nullptr;
    QSpinBox* m_recentLimit = nullptr;
    QCheckBox* m_advancedUser = nullptr;
    QCheckBox* m_useCustomProfile = nullptr;
    QWidget* m_profileControls = nullptr;
    QComboBox* m_defaultProfile = nullptr;
    QPushButton* m_manageProfiles = nullptr;
    QComboBox* m_recommendationGoal = nullptr;
    QLabel* m_recommendationDescription = nullptr;
    QWidget* m_diagnosticControls = nullptr;
    QLabel* m_diagnosticHint = nullptr;
    QDoubleSpinBox* m_commandTimeout = nullptr;
    QDoubleSpinBox* m_buildTimeout = nullptr;
};
