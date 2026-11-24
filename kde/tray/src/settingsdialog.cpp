// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "settingsdialog.h"

#include "backendquery.h"
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
#include <QAction>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

namespace {

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
    setWindowTitle(tr("DwarFSctl Settings"));
    setFaceType(KPageDialog::List);
    setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    setMinimumSize(660, 480);

    auto* settingsHeader = new QWidget(this);
    auto* settingsHeaderLayout = new QVBoxLayout(settingsHeader);
    settingsHeaderLayout->setContentsMargins(0, 0, 0, 0);
    settingsHeaderLayout->setSpacing(6);

    m_message = new KMessageWidget(settingsHeader);
    m_message->setMessageType(KMessageWidget::Error);
    m_message->setTextFormat(Qt::PlainText);
    m_message->setWordWrap(true);
    m_message->setObjectName(QStringLiteral("settingsStatus"));
    m_retry = new QAction(tr("Reload settings"), m_message);
    m_retry->setObjectName(QStringLiteral("settingsRetryAction"));
    m_message->addAction(m_retry);
    connect(m_retry, &QAction::triggered, this, [this] { loadSettings(m_ready); });
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
    m_commandTimeout->setDecimals(3);
    m_commandTimeout->setRange(0, 4294967.295);
    m_commandTimeout->setSingleStep(1.0);
    m_commandTimeout->setSpecialValueText(tr("No timeout"));
    m_commandTimeout->setSuffix(tr(" seconds"));
    diagnosticsForm->addRow(tr("Normal operations:"), m_commandTimeout);
    m_buildTimeout = new QDoubleSpinBox(m_diagnosticControls);
    m_buildTimeout->setDecimals(3);
    m_buildTimeout->setRange(0, 4294967.295);
    m_buildTimeout->setSingleStep(1.0);
    m_buildTimeout->setSpecialValueText(tr("No timeout"));
    m_buildTimeout->setSuffix(tr(" seconds"));
    diagnosticsForm->addRow(tr("Image creation:"), m_buildTimeout);
    diagnosticsLayout->addWidget(m_diagnosticControls);
    diagnosticsLayout->addStretch(1);
    auto* diagnosticsItem = addPage(diagnostics, tr("Diagnostics"));
    diagnosticsItem->setIcon(QIcon::fromTheme(QStringLiteral("preferences-other")));

    m_backendPages = {general, creation, diagnostics};
    m_openAfterMount->setObjectName(QStringLiteral("openAfterMount"));
    m_recentLimit->setObjectName(QStringLiteral("recentImageLimit"));
    m_defaultProfile->setObjectName(QStringLiteral("defaultCreationProfile"));
    m_commandTimeout->setObjectName(QStringLiteral("commandTimeout"));
    m_buildTimeout->setObjectName(QStringLiteral("buildTimeout"));
    connect(m_defaultProfile, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateCreationControls);
    connect(m_recommendationGoal, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateCreationControls);
    connect(m_advancedUser, &QCheckBox::toggled, this, &SettingsDialog::updateCreationControls);
    connect(m_useCustomProfile, &QCheckBox::toggled, this, &SettingsDialog::updateCreationControls);
    connect(m_manageProfiles, &QPushButton::clicked, this, &SettingsDialog::openProfileEditor);
    updateCreationControls();
    loadSettings();
}

void SettingsDialog::setBusy(bool busy, const QString &message)
{
    m_busy = busy;
    for (auto *page : m_backendPages) page->setEnabled(m_ready && !busy);
    m_advancedUser->setEnabled(m_ready && !busy);
    m_retry->setEnabled(!busy);
    if (!message.isEmpty()) {
        m_message->setMessageType(KMessageWidget::Information);
        m_message->setText(message);
        m_message->animatedShow();
    }
    updateCreationControls();
}

void SettingsDialog::loadSettings(bool preserveEdits)
{
    if (m_busy) return;
    delete m_queryContext;
    m_queryContext = new QObject(this);
    auto *context = m_queryContext;
    setBusy(true, tr("Loading settings…"));
    dwarfsplugin::BackendQuery::json(context, {QStringLiteral("config"), QStringLiteral("show")},
        [this, context, preserveEdits](const dwarfsplugin::BackendReply &reply) {
        const auto config = reply.envelope.details;
        const auto ui = config.value(QStringLiteral("ui"));
        const auto creation = config.value(QStringLiteral("creation"));
        dwarfsctl_kde::UiConfig parsed;
        const QJsonObject envelope{{QStringLiteral("ok"), reply.ok}, {QStringLiteral("details"), ui}};
        const auto values = creation.toObject();
        const auto mode = values.value(QStringLiteral("default_mode"));
        const auto profile = values.value(QStringLiteral("default_profile"));
        const auto goal = values.value(QStringLiteral("recommendation_goal"));
        const bool valid = reply.ok && ui.isObject() && creation.isObject()
            && dwarfsctl_kde::parseUiConfig(QJsonDocument(envelope).toJson(), &parsed)
            && mode.isString() && (mode == QJsonValue(QStringLiteral("recommend")) || mode == QJsonValue(QStringLiteral("profile")))
            && ui.toObject().value(QStringLiteral("command_timeout_ms")).toDouble() <= 4294967295.0
            && ui.toObject().value(QStringLiteral("build_timeout_ms")).toDouble() <= 4294967295.0
            && profile.isString() && !profile.toString().isEmpty() && goal.isString()
            && m_recommendationGoal->findData(goal.toString()) >= 0;
        if (!valid) {
            m_needsReload = true;
            setBusy(false);
            showError(reply.ok ? tr("The settings response is invalid. Reload settings to try again.") : reply.error);
            return;
        }
        dwarfsplugin::BackendQuery::json(context,
            {QStringLiteral("config"), QStringLiteral("mkdwarfs-profiles"), QStringLiteral("list"), QStringLiteral("--all")},
            [this, config, preserveEdits](const dwarfsplugin::BackendReply &profiles) {
            const auto ui = config.value(QStringLiteral("ui")).toObject();
            const auto creation = config.value(QStringLiteral("creation")).toObject();
            const auto selection = preserveEdits ? m_defaultProfile->currentData().toString()
                : creation.value(QStringLiteral("default_profile")).toString();
            if (!profiles.ok || !applyProfiles(profiles.envelope.details, selection)) {
                m_needsReload = true;
                setBusy(false);
                showError(profiles.ok ? tr("The compression profile list is invalid. Reload settings to try again.") : profiles.error);
                return;
            }
            if (!preserveEdits) {
                m_openAfterMount->setChecked(ui.value(QStringLiteral("open_after_mount")).toBool());
                m_recentLimit->setValue(dwarfsctl_tray::recentlyMountedLimit());
                m_commandTimeout->setValue(ui.value(QStringLiteral("command_timeout_ms")).toDouble() / 1000.0);
                m_buildTimeout->setValue(ui.value(QStringLiteral("build_timeout_ms")).toDouble() / 1000.0);
                const bool profileMode = creation.value(QStringLiteral("default_mode")).toString() == QLatin1String("profile");
                m_advancedUser->setChecked(dwarfsctl_tray::advancedUserEnabled() || profileMode);
                m_useCustomProfile->setChecked(profileMode);
                m_recommendationGoal->setCurrentIndex(m_recommendationGoal->findData(creation.value(QStringLiteral("recommendation_goal")).toString()));
            }
            m_ready = true;
            m_needsReload = false;
            setBusy(false);
            m_message->animatedHide();
            if (preserveEdits) {
                QStringList differences;
                if (ui.value(QStringLiteral("open_after_mount")).toBool() != m_openAfterMount->isChecked())
                    differences.append(tr("Open after mounting: %1").arg(ui.value(QStringLiteral("open_after_mount")).toBool() ? tr("On") : tr("Off")));
                const auto timeoutText = [](double ms) { return ms == 0 ? tr("No timeout") : tr("%1 seconds").arg(QString::number(ms / 1000.0, 'f', 3)); };
                if (ui.value(QStringLiteral("command_timeout_ms")).toDouble() != qRound64(m_commandTimeout->value() * 1000.0))
                    differences.append(tr("Normal operation timeout: %1").arg(timeoutText(ui.value(QStringLiteral("command_timeout_ms")).toDouble())));
                if (ui.value(QStringLiteral("build_timeout_ms")).toDouble() != qRound64(m_buildTimeout->value() * 1000.0))
                    differences.append(tr("Creation timeout: %1").arg(timeoutText(ui.value(QStringLiteral("build_timeout_ms")).toDouble())));
                const auto draftMode = m_advancedUser->isChecked() && m_useCustomProfile->isChecked() ? QStringLiteral("profile") : QStringLiteral("recommend");
                if (creation.value(QStringLiteral("default_mode")).toString() != draftMode
                    || creation.value(QStringLiteral("default_profile")).toString() != m_defaultProfile->currentData().toString()
                    || creation.value(QStringLiteral("recommendation_goal")).toString() != m_recommendationGoal->currentData().toString()) {
                    differences.append(creation.value(QStringLiteral("default_mode")).toString() == QLatin1String("profile")
                        ? tr("Compression profile: %1").arg(creation.value(QStringLiteral("default_profile")).toString())
                        : tr("Automatic compression: %1").arg(m_recommendationGoal->itemText(m_recommendationGoal->findData(creation.value(QStringLiteral("recommendation_goal")).toString()))));
                }
                m_message->setMessageType(differences.isEmpty() ? KMessageWidget::Information : KMessageWidget::Warning);
                m_message->setText(differences.isEmpty()
                    ? tr("Saved image behavior and creation settings match your changes. Review the history and Advanced User preferences, then Save to confirm all settings.")
                    : tr("Saved settings differ from your changes. Your edits are still shown; Save will apply them.\nSaved values:\n%1").arg(differences.join(QLatin1Char('\n'))));
                m_message->animatedShow();
            }
            if (m_advancedUser->isChecked() && m_useCustomProfile->isChecked() && !m_profiles.contains(selection))
                showError(tr("The selected compression profile is unavailable. Choose another profile or turn off custom compression."));
        });
    });
}

bool SettingsDialog::applyProfiles(const QJsonObject &details, const QString &selection)
{
    const auto list = details.value(QStringLiteral("profiles"));
    if (!list.isArray()) return false;
    QStringList names{QStringLiteral("default")};
    for (const auto &item : list.toArray()) {
        if (!item.isString() || item.toString().isEmpty()) return false;
        if (!names.contains(item.toString())) names.append(item.toString());
    }
    m_profiles = names;
    m_defaultProfile->clear();
    for (const auto &name : names)
        m_defaultProfile->addItem(name == QLatin1String("default") ? tr("DwarFS defaults") : profileLabel(name), name);
    if (!names.contains(selection))
        m_defaultProfile->addItem(tr("%1 (unavailable)").arg(selection), selection);
    m_defaultProfile->setCurrentIndex(m_defaultProfile->findData(selection));
    return true;
}

void SettingsDialog::loadProfiles(const QString &selectedProfile)
{
    if (m_busy) return;
    delete m_queryContext;
    m_queryContext = new QObject(this);
    setBusy(true, tr("Loading compression profiles…"));
    dwarfsplugin::BackendQuery::json(m_queryContext,
        {QStringLiteral("config"), QStringLiteral("mkdwarfs-profiles"), QStringLiteral("list"), QStringLiteral("--all")},
        [this, selectedProfile](const dwarfsplugin::BackendReply &reply) {
        if (!reply.ok || !applyProfiles(reply.envelope.details, selectedProfile)) {
            m_needsReload = true;
            setBusy(false);
            showError(reply.ok ? tr("The compression profile list is invalid. Reload settings to try again.") : reply.error);
            return;
        }
        setBusy(false);
        m_message->animatedHide();
        if (!m_profiles.contains(selectedProfile)) showError(tr("The selected compression profile is unavailable. Choose another profile."));
    });
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

void SettingsDialog::saveSettings()
{
    if (!m_ready || m_busy || m_needsReload) return;
    const QStringList arguments{
        QStringLiteral("config"), QStringLiteral("set"),
        QStringLiteral("ui.open_after_mount"), m_openAfterMount->isChecked() ? QStringLiteral("true") : QStringLiteral("false"),
        QStringLiteral("ui.command_timeout_ms"), QString::number(qRound64(m_commandTimeout->value() * 1000.0)),
        QStringLiteral("ui.build_timeout_ms"), QString::number(qRound64(m_buildTimeout->value() * 1000.0)),
        QStringLiteral("creation.default_mode"), m_advancedUser->isChecked() && m_useCustomProfile->isChecked() ? QStringLiteral("profile") : QStringLiteral("recommend"),
        QStringLiteral("creation.default_profile"), m_defaultProfile->currentData().toString(),
        QStringLiteral("creation.recommendation_goal"), m_recommendationGoal->currentData().toString()};
    QJsonObject expected;
    for (int i = 2; i < arguments.size(); i += 2) {
        const auto &key = arguments[i];
        if (key == QLatin1String("ui.open_after_mount")) expected.insert(key, arguments[i + 1] == QLatin1String("true"));
        else if (key.endsWith(QLatin1String("_ms"))) expected.insert(key, arguments[i + 1].toLongLong());
        else expected.insert(key, arguments[i + 1]);
    }
    const int recent = m_recentLimit->value();
    const bool advanced = m_advancedUser->isChecked();
    delete m_queryContext;
    m_queryContext = new QObject(this);
    setBusy(true, tr("Saving settings…"));
    dwarfsplugin::BackendQuery::json(m_queryContext, arguments,
        [this, expected, recent, advanced](const dwarfsplugin::BackendReply &reply) {
        if (!reply.ok || reply.envelope.details.value(QStringLiteral("preferences")) != QJsonValue(expected)) {
            const auto kind = reply.envelope.error.kind;
            m_needsReload = reply.ok || (kind != QLatin1String("busy") && kind != QLatin1String("spawn")
                && kind != QLatin1String("Usage") && kind != QLatin1String("LockBusy"));
            setBusy(false);
            showError(m_needsReload
                ? tr("The settings save could not be confirmed. Changes may have been saved. Reload settings to check before saving again.\n%1").arg(reply.error)
                : reply.error);
            return;
        }
        setBusy(false);
        if (!dwarfsctl_kde::setRecentImageLimit(recent)) {
            showError(tr("Image behavior and creation defaults were saved, but the history limit could not be saved. Your changes are still shown."));
            return;
        }
        if (!dwarfsctl_tray::setAdvancedUserEnabled(advanced)) {
            showError(tr("Image behavior and history settings were saved, but the Advanced User preference could not be saved. Your changes are still shown."));
            return;
        }
        KPageDialog::accept();
    });
}

void SettingsDialog::accept()
{
    if (buttonBox()->button(QDialogButtonBox::Ok)->isEnabled()) saveSettings();
}

void SettingsDialog::showError(const QString& message)
{
    m_message->setMessageType(KMessageWidget::Error);
    m_message->setText(message);
    m_message->animatedShow();
}

void SettingsDialog::updateCreationControls()
{
    const bool advanced = m_advancedUser->isChecked();
    const bool profileMode = advanced && m_useCustomProfile->isChecked();
    const bool usable = m_ready && !m_busy;
    buttonBox()->button(QDialogButtonBox::Ok)->setEnabled(usable && !m_needsReload
        && (!profileMode || m_profiles.contains(m_defaultProfile->currentData().toString())));
    m_profileControls->setVisible(advanced);
    m_defaultProfile->setEnabled(usable && profileMode);
    m_manageProfiles->setEnabled(usable && advanced);
    m_recommendationGoal->setEnabled(usable && !profileMode);
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
