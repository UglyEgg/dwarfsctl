// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "builddialog.h"

#include "bannerwidget.h"
#include "dwarfsctljob.h"
#include "dwarfsctljson.h"
#include "dwarfsctlrunner.h"
#include "logging.h"
#include "recommendation.h"
#include "warnings.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace dwarfsplugin {

namespace {

bool runDwarfsctlJson(const QStringList &args, Envelope *outEnv, QString *outErr)
{
    const auto rr = DwarfsctlRunner::run(args, DwarfsctlRunner::defaultTimeoutMs());
    if (rr.crashed) {
        if (outErr)
            *outErr = i18n("dwarfsctl did not respond");
        return false;
    }
    const auto parsed = parseEnvelope(rr.stdout_bytes);
    if (!parsed.parsed) {
        if (outErr)
            *outErr = i18n("The DwarFS service returned an invalid response.");
        return false;
    }
    if (!parsed.envelope.ok) {
        if (outErr)
            *outErr = parsed.envelope.has_error ? parsed.envelope.error.message : i18n("The DwarFS operation failed.");
        return false;
    }
    if (outEnv)
        *outEnv = parsed.envelope;
    return true;
}

QStringList getProfileArgs(const QString &name, QString *error)
{
    if (name == QStringLiteral("default"))
        return {};

    Envelope env;
    QString failure;
    if (!runDwarfsctlJson({QStringLiteral("config"), QStringLiteral("mkdwarfs-profiles"),
                           QStringLiteral("get"), name},
                          &env, &failure)) {
        if (error)
            *error = failure;
        return {};
    }

    QStringList result;
    const auto args = env.details.value(QStringLiteral("profile")).toObject()
                          .value(QStringLiteral("args")).toArray();
    for (const auto &value : args) {
        const QString argument = value.toString();
        if (!argument.isEmpty())
            result.append(argument);
    }
    return result;
}

struct CreationDefaults {
    QString mode = QStringLiteral("recommend");
    QString profile = QStringLiteral("default");
    QString goal = QStringLiteral("balanced");
};

CreationDefaults loadCreationDefaults()
{
    CreationDefaults result;
    Envelope env;
    if (!runDwarfsctlJson({QStringLiteral("config"), QStringLiteral("show"),
                           QStringLiteral("--section"), QStringLiteral("creation")},
                          &env, nullptr)) {
        return result;
    }
    const QString mode = env.details.value(QStringLiteral("default_mode")).toString();
    if (mode == QStringLiteral("profile") || mode == QStringLiteral("recommend"))
        result.mode = mode;
    const QString profile = env.details.value(QStringLiteral("default_profile")).toString();
    if (!profile.isEmpty())
        result.profile = profile;
    const QString goal = env.details.value(QStringLiteral("recommendation_goal")).toString();
    if (!goal.isEmpty())
        result.goal = goal;
    return result;
}

QString goalLabel(const QString &goal)
{
    if (goal == QStringLiteral("smallest"))
        return i18n("Smallest image");
    if (goal == QStringLiteral("fastest-browse"))
        return i18n("Fastest browsing");
    if (goal == QStringLiteral("fast-build"))
        return i18n("Fastest creation");
    return i18n("Balanced");
}

} // namespace

BuildDialogResult promptBuildOptions(QWidget *parent, const QString &sourceDir)
{
    BuildDialogResult result;
    const CreationDefaults defaults = loadCreationDefaults();

    QDialog dialog(parent);
    dialog.setWindowTitle(i18n("Create DwarFS Image"));
    dialog.setModal(true);
    dialog.setMinimumWidth(600);

    auto *root = new QVBoxLayout(&dialog);
    auto *banner = new BannerWidget(&dialog);
    banner->setSeverity(BannerWidget::Severity::Warning);
    root->addWidget(banner);

    Envelope schemaEnvelope;
    QString schemaError;
    if (!runDwarfsctlJson({QStringLiteral("mkdwarfs"), QStringLiteral("schema")},
                          &schemaEnvelope, &schemaError)) {
        qCWarning(DWARFS_DOLPHIN_LOG) << "mkdwarfs schema probe failed" << schemaError;
        QMessageBox::critical(&dialog, i18n("DwarFS creation unavailable"),
                              i18n("Unable to load the image creation options from dwarfsctl.\n\n%1", schemaError));
        return result;
    }
    QString versionWarning;
    if (takeMkdwarfsWarningOnce(schemaEnvelope, &versionWarning)) {
        banner->setTitleAndBody(i18n("Image creator version warning"), versionWarning);
        banner->show();
    }

    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    root->addLayout(form);

    auto *sourceLabel = new QLabel(sourceDir, &dialog);
    sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(i18n("Source"), sourceLabel);

    auto *outputRow = new QWidget(&dialog);
    auto *outputLayout = new QHBoxLayout(outputRow);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    auto *outputEdit = new QLineEdit(outputRow);
    outputEdit->setPlaceholderText(i18n("Select an output .dwarfs image"));
    const QFileInfo sourceInfo(sourceDir);
    outputEdit->setText(sourceInfo.dir().filePath(sourceInfo.fileName() + QStringLiteral(".dwarfs")));
    auto *browseButton = new QPushButton(i18n("Browse…"), outputRow);
    outputLayout->addWidget(outputEdit, 1);
    outputLayout->addWidget(browseButton);
    form->addRow(i18n("Save as"), outputRow);

    QObject::connect(browseButton, &QPushButton::clicked, &dialog, [&]() {
        const QString path = QFileDialog::getSaveFileName(
            &dialog, i18n("Select output image"), outputEdit->text(),
            i18n("DwarFS images (*.dwarfs);;All files (*)"));
        if (!path.isEmpty())
            outputEdit->setText(path);
    });

    auto *compressionStatus = new QLabel(&dialog);
    compressionStatus->setObjectName(QStringLiteral("compressionStatus"));
    compressionStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(i18n("Compression"), compressionStatus);

    auto *settingsHint = new QLabel(
        i18n("Change compression defaults and profiles from the DwarFS tray Settings."), &dialog);
    settingsHint->setWordWrap(true);
    settingsHint->setStyleSheet(QStringLiteral("color: palette(mid);"));
    form->addRow(QString(), settingsHint);

    auto *advancedBox = new QGroupBox(i18n("Build Options"), &dialog);
    advancedBox->setCheckable(true);
    advancedBox->setChecked(false);
    auto *advancedLayout = new QVBoxLayout(advancedBox);
    auto *force = new QCheckBox(i18n("Replace an existing image"), advancedBox);
    auto *overwrite = new QCheckBox(i18n("Replace files without asking"), advancedBox);
    auto *dryRun = new QCheckBox(i18n("Preview only; do not create an image"), advancedBox);
    advancedLayout->addWidget(force);
    advancedLayout->addWidget(overwrite);
    advancedLayout->addWidget(dryRun);
    root->addWidget(advancedBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    auto *createButton = buttons->button(QDialogButtonBox::Ok);
    createButton->setText(i18n("Create"));
    root->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    QStringList compressionArgs;
    QString compressionName;
    if (defaults.mode == QStringLiteral("profile")) {
        compressionName = defaults.profile;
        QString profileError;
        compressionArgs = getProfileArgs(defaults.profile, &profileError);
        if (profileError.isEmpty()) {
            compressionStatus->setText(defaults.profile == QStringLiteral("default")
                ? i18n("Default compression")
                : i18n("Profile: %1", defaults.profile));
        } else {
            compressionName = QStringLiteral("default");
            compressionStatus->setText(i18n("Default compression"));
            banner->setTitleAndBody(i18n("Profile unavailable"),
                i18n("The configured profile could not be loaded, so default compression will be used. %1", profileError));
            banner->show();
        }
    } else {
        compressionName = QStringLiteral("recommended");
        compressionStatus->setText(i18n("Choosing the best settings for this folder…"));
        createButton->setEnabled(false);
    }

    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        result.accepted = true;
        result.outputPath = outputEdit->text().trimmed();
        result.profileName = compressionName;
        result.profileArgs = compressionArgs;
        result.force = force->isChecked();
        result.overwriteOutput = overwrite->isChecked();
        result.dryRun = dryRun->isChecked();
        dialog.accept();
    });

    if (defaults.mode == QStringLiteral("recommend")) {
        QTimer::singleShot(0, &dialog, [&]() {
            DwarfsctlJob::Request request;
            request.title = i18n("Choose compression settings automatically");
            request.args = {QStringLiteral("mkdwarfs"), QStringLiteral("recommend"), sourceDir,
                            QStringLiteral("--goal"), defaults.goal};
            request.timeout_ms = 30000;
            auto *job = new DwarfsctlJob(request, &dialog);
            job->setUiDelegate(nullptr);
            QObject::connect(job, &KJob::result, &dialog, [&, job]() {
                const auto recommendation = parseRecommendation(job->envelope().details);
                if (job->error() || !recommendation.valid) {
                    compressionName = QStringLiteral("default");
                    compressionArgs.clear();
                    compressionStatus->setText(i18n("Default compression"));
                    const QString reason = job->error()
                        ? job->errorString()
                        : i18n("The recommendation response was invalid.");
                    banner->setTitleAndBody(i18n("Automatic choice unavailable"),
                        i18n("DwarFS defaults will be used. %1", reason));
                    banner->show();
                    createButton->setEnabled(true);
                    return;
                }

                compressionArgs = recommendation.flags;
                compressionStatus->setText(i18n("Optimized for %1", goalLabel(defaults.goal)));
                compressionStatus->setToolTip(recommendation.reasons.join(QStringLiteral("\n")));
                if (!recommendation.warnings.isEmpty()) {
                    banner->setTitleAndBody(i18n("Some items could not be fully considered"),
                                            recommendation.warnings.join(QStringLiteral("\n")));
                    banner->show();
                }
                createButton->setEnabled(true);
            });
            job->start();
        });
    }

    dialog.exec();
    return result;
}

} // namespace dwarfsplugin
