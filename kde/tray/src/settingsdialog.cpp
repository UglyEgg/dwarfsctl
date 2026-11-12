// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "settingsdialog.h"

#include "dwarfsctlrunner.h"
#include "profileeditordialog.h"
#include "traysettings.h"

#include <KMessageWidget>
#include <KPageWidgetItem>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

namespace {

bool runDwarfsctl(const QStringList& arguments, QJsonObject* details, QString* error)
{
    QProcess process;
    process.start(DwarfsctlRunner::dwarfsctlProgram(), arguments);
    if (!process.waitForStarted(5000) || !process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);
        if (error)
            *error = SettingsDialog::tr("The settings service did not respond.");
        return false;
    }

    QJsonParseError parseError {};
    const auto document = QJsonDocument::fromJson(process.readAllStandardOutput(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = SettingsDialog::tr("The settings service returned an invalid response.");
        return false;
    }
    const auto envelope = document.object();
    if (process.exitCode() != 0 || !envelope.value(QStringLiteral("ok")).toBool()) {
        const auto errorObject = envelope.value(QStringLiteral("error")).toObject();
        if (error)
            *error = errorObject.value(QStringLiteral("message")).toString(SettingsDialog::tr("The settings could not be saved."));
        return false;
    }
    if (details)
        *details = envelope.value(QStringLiteral("details")).toObject();
    return true;
}

QWidget* pageWithForm(const QString& description, QFormLayout** formOut, QWidget* parent)
{
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    auto* intro = new QLabel(description, page);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    layout->addLayout(form);
    layout->addStretch(1);
    *formOut = form;
    return page;
}

QString profileLabel(const QString& name)
{
    if (name == QStringLiteral("@balanced"))
        return SettingsDialog::tr("Balanced (built in)");
    if (name == QStringLiteral("@fast"))
        return SettingsDialog::tr("Faster creation (built in)");
    if (name == QStringLiteral("@max"))
        return SettingsDialog::tr("Smaller image (built in)");
    if (name == QStringLiteral("@git"))
        return SettingsDialog::tr("Git repositories (built in)");
    return name;
}

} // namespace

SettingsDialog::SettingsDialog(QWidget* parent)
    : KPageDialog(parent)
{
    setWindowTitle(tr("DwarFS Settings"));
    setFaceType(KPageDialog::List);
    setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    setMinimumSize(660, 480);

    auto* settingsHeader = new QWidget(this);
    auto* settingsHeaderLayout = new QVBoxLayout(settingsHeader);
    settingsHeaderLayout->setContentsMargins(0, 0, 0, 0);
    settingsHeaderLayout->setSpacing(6);

    m_message = new KMessageWidget(settingsHeader);
    m_message->setMessageType(KMessageWidget::Error);
    m_message->setCloseButtonVisible(true);
    m_message->hide();
    settingsHeaderLayout->addWidget(m_message);

    auto* advancedRow = new QWidget(settingsHeader);
    auto* advancedRowLayout = new QHBoxLayout(advancedRow);
    advancedRowLayout->setContentsMargins(0, 0, 0, 0);
    advancedRowLayout->addStretch(1);
    m_advancedUser = new QCheckBox(tr("Advanced User"), advancedRow);
    m_advancedUser->setObjectName(QStringLiteral("settingsAdvancedUserToggle"));
    m_advancedUser->setToolTip(tr("Show custom profiles, diagnostic controls, and exact DwarFS settings."));
    advancedRowLayout->addWidget(m_advancedUser);
    settingsHeaderLayout->addWidget(advancedRow);
    layout()->setMenuBar(settingsHeader);

    QFormLayout* generalForm = nullptr;
    auto* general = pageWithForm(
        tr("Choose how mounted images behave and how much history the tray keeps."),
        &generalForm,
        this);
    m_openAfterMount = new QCheckBox(tr("Open the volume in Dolphin after mounting"), general);
    generalForm->addRow(QString(), m_openAfterMount);
    m_recentLimit = new QSpinBox(general);
    m_recentLimit->setRange(0, 9);
    m_recentLimit->setSpecialValueText(tr("Off"));
    m_recentLimit->setSuffix(tr(" images"));
    generalForm->addRow(tr("Recent images:"), m_recentLimit);
    auto* generalItem = addPage(general, tr("General"));
    generalItem->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system")));

    QFormLayout* creationForm = nullptr;
    auto* creation = pageWithForm(
        tr("Choose the result you want. DwarFSctl examines each selected folder and chooses the technical settings automatically."),
        &creationForm,
        this);
    m_recommendationGoal = new QComboBox(creation);
    m_recommendationGoal->setObjectName(QStringLiteral("creationPriority"));
    m_recommendationGoal->addItem(tr("Balanced (recommended)"), QStringLiteral("balanced"));
    m_recommendationGoal->addItem(tr("Smallest image"), QStringLiteral("smallest"));
    m_recommendationGoal->addItem(tr("Faster browsing"), QStringLiteral("fastest-browse"));
    m_recommendationGoal->addItem(tr("Fastest creation"), QStringLiteral("fast-build"));
    creationForm->addRow(tr("Optimize for:"), m_recommendationGoal);
    m_recommendationDescription = new QLabel(creation);
    m_recommendationDescription->setObjectName(QStringLiteral("creationPriorityDescription"));
    m_recommendationDescription->setWordWrap(true);
    creationForm->addRow(QString(), m_recommendationDescription);

    m_profileControls = new QWidget(creation);
    m_profileControls->setObjectName(QStringLiteral("advancedCreationControls"));
    auto* profileForm = new QFormLayout(m_profileControls);
    profileForm->setContentsMargins(0, 0, 0, 0);
    profileForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_useCustomProfile = new QCheckBox(tr("Use a custom compression profile"), m_profileControls);
    m_useCustomProfile->setObjectName(QStringLiteral("useCustomProfile"));
    profileForm->addRow(QString(), m_useCustomProfile);
    m_defaultProfile = new QComboBox(m_profileControls);
    profileForm->addRow(tr("Profile:"), m_defaultProfile);
    m_manageProfiles = new QPushButton(tr("Create or Edit Profiles…"), m_profileControls);
    m_manageProfiles->setIcon(QIcon::fromTheme(QStringLiteral("document-edit")));
    m_manageProfiles->setToolTip(tr("Create a starting point or tune the exact DwarFS controls."));
    profileForm->addRow(QString(), m_manageProfiles);
    creationForm->addRow(QString(), m_profileControls);
    auto* creationItem = addPage(creation, tr("Image Creation"));
    creationItem->setIcon(QIcon::fromTheme(QStringLiteral("drive-harddisk")));

    auto* diagnostics = new QWidget(this);
    auto* diagnosticsLayout = new QVBoxLayout(diagnostics);
    auto* diagnosticsIntro = new QLabel(
        tr("Control how long DwarFSctl waits for operations before reporting a timeout."),
        diagnostics);
    diagnosticsIntro->setWordWrap(true);
    diagnosticsLayout->addWidget(diagnosticsIntro);
    m_diagnosticHint = new QLabel(
        tr("Enable Advanced User above to change diagnostic controls."),
        diagnostics);
    m_diagnosticHint->setWordWrap(true);
    diagnosticsLayout->addWidget(m_diagnosticHint);
    m_diagnosticControls = new QWidget(diagnostics);
    m_diagnosticControls->setObjectName(QStringLiteral("advancedDiagnosticControls"));
    auto* diagnosticsForm = new QFormLayout(m_diagnosticControls);
    diagnosticsForm->setContentsMargins(0, 0, 0, 0);
    diagnosticsForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    diagnosticsForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_commandTimeout = new QDoubleSpinBox(m_diagnosticControls);
    m_commandTimeout->setRange(0, 86400);
    m_commandTimeout->setDecimals(3);
    m_commandTimeout->setSingleStep(1.0);
    m_commandTimeout->setSpecialValueText(tr("No timeout"));
    m_commandTimeout->setSuffix(tr(" seconds"));
    diagnosticsForm->addRow(tr("Normal operations:"), m_commandTimeout);
    m_buildTimeout = new QDoubleSpinBox(m_diagnosticControls);
    m_buildTimeout->setRange(0, 86400);
    m_buildTimeout->setDecimals(3);
    m_buildTimeout->setSingleStep(1.0);
    m_buildTimeout->setSpecialValueText(tr("No timeout"));
    m_buildTimeout->setSuffix(tr(" seconds"));
    diagnosticsForm->addRow(tr("Image creation:"), m_buildTimeout);
    diagnosticsLayout->addWidget(m_diagnosticControls);
    diagnosticsLayout->addStretch(1);
    auto* diagnosticsItem = addPage(diagnostics, tr("Diagnostics"));
    diagnosticsItem->setIcon(QIcon::fromTheme(QStringLiteral("preferences-other")));

    connect(m_recommendationGoal, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateCreationControls);
    connect(m_advancedUser, &QCheckBox::toggled, this, &SettingsDialog::updateCreationControls);
    connect(m_useCustomProfile, &QCheckBox::toggled, this, &SettingsDialog::updateCreationControls);
    connect(m_manageProfiles, &QPushButton::clicked, this, &SettingsDialog::openProfileEditor);
    updateCreationControls();
    if (!loadSettings())
        buttonBox()->button(QDialogButtonBox::Ok)->setEnabled(false);
}

bool SettingsDialog::loadSettings()
{
    QString error;
    QJsonObject config;
    if (!runDwarfsctl({QStringLiteral("config"), QStringLiteral("show")}, &config, &error)) {
        showError(error);
        return false;
    }

    const auto ui = config.value(QStringLiteral("ui")).toObject();
    const auto creation = config.value(QStringLiteral("creation")).toObject();
    m_openAfterMount->setChecked(ui.value(QStringLiteral("open_after_mount")).toBool(true));
    m_recentLimit->setValue(dwarfsctl_tray::recentlyMountedLimit());
    m_commandTimeout->setValue(ui.value(QStringLiteral("command_timeout_ms")).toInt(300000) / 1000.0);
    m_buildTimeout->setValue(ui.value(QStringLiteral("build_timeout_ms")).toInt(0) / 1000.0);

    const QString mode = creation.value(QStringLiteral("default_mode")).toString(QStringLiteral("recommend"));
    m_advancedUser->setChecked(dwarfsctl_tray::advancedUserEnabled() || mode == QStringLiteral("profile"));
    m_useCustomProfile->setChecked(mode == QStringLiteral("profile"));
    const QString profile = creation.value(QStringLiteral("default_profile")).toString(QStringLiteral("default"));
    if (!loadProfiles(profile))
        return false;
    const QString goal = creation.value(QStringLiteral("recommendation_goal")).toString(QStringLiteral("balanced"));
    m_recommendationGoal->setCurrentIndex(qMax(0, m_recommendationGoal->findData(goal)));
    updateCreationControls();
    return true;
}

bool SettingsDialog::loadProfiles(const QString& selectedProfile)
{
    QString error;
    QJsonObject details;
    if (!runDwarfsctl({QStringLiteral("config"), QStringLiteral("mkdwarfs-profiles"),
                       QStringLiteral("list"), QStringLiteral("--all")},
                      &details, &error)) {
        showError(error);
        return false;
    }

    const QString selection = selectedProfile.isEmpty()
        ? m_defaultProfile->currentData().toString()
        : selectedProfile;
    m_defaultProfile->clear();
    m_defaultProfile->addItem(tr("DwarFS defaults"), QStringLiteral("default"));
    for (const auto& value : details.value(QStringLiteral("profiles")).toArray()) {
        const QString name = value.toString();
        if (!name.isEmpty() && name != QStringLiteral("default"))
            m_defaultProfile->addItem(profileLabel(name), name);
    }
    const int index = m_defaultProfile->findData(selection);
    m_defaultProfile->setCurrentIndex(index >= 0 ? index : 0);
    return true;
}

void SettingsDialog::openProfileEditor()
{
    const QString previousSelection = m_defaultProfile->currentData().toString();
    QString selected = previousSelection;
    if (selected == QStringLiteral("default") || selected.startsWith(QLatin1Char('@')))
        selected.clear();

    dwarfsplugin::ProfileEditorDialog editor(this, selected);
    const int dialogResult = editor.exec();
    const auto result = editor.result();
    loadProfiles(dialogResult == QDialog::Accepted && result.saved
        ? result.profileName
        : previousSelection);
}

bool SettingsDialog::saveSettings()
{
    const QStringList arguments{
        QStringLiteral("config"), QStringLiteral("set"),
        QStringLiteral("ui.open_after_mount"), m_openAfterMount->isChecked() ? QStringLiteral("true") : QStringLiteral("false"),
        QStringLiteral("ui.command_timeout_ms"), QString::number(qRound64(m_commandTimeout->value() * 1000.0)),
        QStringLiteral("ui.build_timeout_ms"), QString::number(qRound64(m_buildTimeout->value() * 1000.0)),
        QStringLiteral("creation.default_mode"),
            m_advancedUser->isChecked() && m_useCustomProfile->isChecked()
                ? QStringLiteral("profile") : QStringLiteral("recommend"),
        QStringLiteral("creation.default_profile"), m_defaultProfile->currentData().toString(),
        QStringLiteral("creation.recommendation_goal"), m_recommendationGoal->currentData().toString(),
    };
    QString error;
    if (!runDwarfsctl(arguments, nullptr, &error)) {
        showError(error);
        return false;
    }
    dwarfsctl_tray::setRecentlyMountedLimit(m_recentLimit->value());
    dwarfsctl_tray::setAdvancedUserEnabled(m_advancedUser->isChecked());
    return true;
}

void SettingsDialog::accept()
{
    buttonBox()->button(QDialogButtonBox::Ok)->setEnabled(false);
    if (saveSettings()) {
        KPageDialog::accept();
        return;
    }
    buttonBox()->button(QDialogButtonBox::Ok)->setEnabled(true);
}

void SettingsDialog::showError(const QString& message)
{
    m_message->setText(message);
    m_message->animatedShow();
}

void SettingsDialog::updateCreationControls()
{
    const bool advanced = m_advancedUser->isChecked();
    const bool profileMode = advanced && m_useCustomProfile->isChecked();
    m_profileControls->setVisible(advanced);
    m_defaultProfile->setEnabled(profileMode);
    m_manageProfiles->setEnabled(advanced);
    m_recommendationGoal->setEnabled(!profileMode);
    m_diagnosticControls->setVisible(advanced);
    m_diagnosticHint->setVisible(!advanced);

    const QString goal = m_recommendationGoal->currentData().toString();
    if (goal == QStringLiteral("smallest"))
        m_recommendationDescription->setText(tr("Spend more time creating the image to use less storage."));
    else if (goal == QStringLiteral("fastest-browse"))
        m_recommendationDescription->setText(tr("Favor quick browsing and opening files inside the image."));
    else if (goal == QStringLiteral("fast-build"))
        m_recommendationDescription->setText(tr("Create the image sooner, even if the file is larger."));
    else
        m_recommendationDescription->setText(tr("Balance creation time, image size, and browsing speed for everyday use."));
}
