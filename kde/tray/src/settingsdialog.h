// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KPageDialog>
#include <QString>

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
    bool loadSettings();
    bool loadProfiles(const QString& selectedProfile = {});
    bool saveSettings();
    void openProfileEditor();
    void showError(const QString& message);
    void updateCreationControls();

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
