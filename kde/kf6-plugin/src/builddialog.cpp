// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "builddialog.h"

#include "bannerwidget.h"
#include "dwarfsctljob.h"
#include "dwarfsctljson.h"
#include "backendquery.h"
#include "logging.h"
#include "recommendation.h"
#include "../../common/uipresentation.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <functional>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace dwarfsplugin {

namespace {

struct CreationDefaults {
    QString mode;
    QString profile;
    QString goal;
};

bool parseCreationDefaults(const QJsonObject &details, CreationDefaults *out)
{
    const auto mode = details.value(QStringLiteral("default_mode"));
    const auto profile = details.value(QStringLiteral("default_profile"));
    const auto goal = details.value(QStringLiteral("recommendation_goal"));
    if (!mode.isString() || !profile.isString() || !goal.isString()) return false;
    if (mode == QJsonValue(QStringLiteral("profile")) && profile.toString().isEmpty()) return false;
    if (mode != QJsonValue(QStringLiteral("profile")) && mode != QJsonValue(QStringLiteral("recommend"))) return false;
    const auto goalName = goal.toString();
    if (goalName != QLatin1String("balanced") && goalName != QLatin1String("smallest")
        && goalName != QLatin1String("fastest-browse") && goalName != QLatin1String("fast-build")) return false;
    *out = {mode.toString(), profile.toString(), goalName};
    return true;
}

bool parseProfileArgs(const QJsonObject &details, QStringList *out)
{
    const auto profile = details.value(QStringLiteral("profile"));
    const auto args = profile.toObject().value(QStringLiteral("args"));
    if (!profile.isObject() || !args.isArray()) return false;
    QStringList parsed;
    for (const auto &arg : args.toArray()) {
        if (!arg.isString()) return false;
        parsed.append(arg.toString());
    }
    *out = parsed;
    return true;
}

bool validCreationSchema(const QJsonValue &schema)
{
    const auto categories = schema.toObject().value(QStringLiteral("categories"));
    if (!schema.isObject() || !categories.isArray() || categories.toArray().isEmpty()) return false;
    bool hasOptions = false;
    for (const auto &category : categories.toArray()) {
        const auto object = category.toObject();
        const auto options = object.value(QStringLiteral("options"));
        if (!category.isObject() || !object.value(QStringLiteral("name")).isString() || !options.isArray()) return false;
        for (const auto &option : options.toArray()) {
            const auto fields = option.toObject();
            if (!option.isObject() || !fields.value(QStringLiteral("opt")).isString()
                || fields.value(QStringLiteral("opt")).toString().isEmpty()
                || !fields.value(QStringLiteral("label")).isString() || !fields.value(QStringLiteral("kind")).isString()) return false;
            hasOptions = true;
        }
    }
    return hasOptions;
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

    QDialog dialog(parent);
    dialog.setWindowTitle(i18n("Create DwarFS Image"));
    dialog.setModal(true);
    dialog.setMinimumWidth(580);

    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(dwarfsctl_kde::contentMargins(&dialog));
    root->setSpacing(dwarfsctl_kde::layoutSpacing(&dialog));
    const QFileInfo sourceInfo(sourceDir);
    auto *header = new QWidget(&dialog);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    auto *sourceIcon = new QLabel(header);
    sourceIcon->setPixmap(QIcon::fromTheme(QStringLiteral("folder")).pixmap(36, 36));
    sourceIcon->setFixedSize(40, 40);
    sourceIcon->setAlignment(Qt::AlignCenter);
    auto *sourceText = new QWidget(header);
    auto *sourceTextLayout = new QVBoxLayout(sourceText);
    sourceTextLayout->setContentsMargins(0, 0, 0, 0);
    sourceTextLayout->setSpacing(2);
    auto *sourceName = new QLabel(sourceInfo.fileName(), sourceText);
    sourceName->setFont(dwarfsctl_kde::headingFont(sourceName->font()));
    sourceName->setWordWrap(true);
    auto *sourcePath = new QLabel(sourceDir, sourceText);
    sourcePath->setWordWrap(true);
    sourcePath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sourceTextLayout->addWidget(sourceName);
    sourceTextLayout->addWidget(sourcePath);
    headerLayout->addWidget(sourceIcon);
    headerLayout->addWidget(sourceText, 1);
    root->addWidget(header);

    auto addDivider = [&] {
        auto *line = new QFrame(&dialog);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Plain);
        root->addWidget(line);
    };
    addDivider();

    auto *banner = new BannerWidget(&dialog);
    banner->setSeverity(BannerWidget::Severity::Warning);

    auto sectionFont = dialog.font();
    sectionFont.setBold(true);
    auto *saveTitle = new QLabel(i18n("Save as"), &dialog);
    saveTitle->setFont(sectionFont);
    root->addWidget(saveTitle);

    auto *outputRow = new QWidget(&dialog);
    auto *outputLayout = new QHBoxLayout(outputRow);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    auto *outputEdit = new QLineEdit(outputRow);
    outputEdit->setObjectName(QStringLiteral("buildOutput"));
    outputEdit->setPlaceholderText(i18n("Image name.dwarfs"));
    outputEdit->setText(sourceInfo.fileName() + QStringLiteral(".dwarfs"));
    QString outputDirectory = sourceInfo.dir().absolutePath();
    auto *browseButton = new QPushButton(i18n("Browse…"), outputRow);
    outputLayout->addWidget(outputEdit, 1);
    outputLayout->addWidget(browseButton);
    root->addWidget(outputRow);

    auto *destinationLabel = new QLabel(&dialog);
    destinationLabel->setObjectName(QStringLiteral("buildDestination"));
    destinationLabel->setWordWrap(true);
    destinationLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto updateDestination = [&] {
        const QString name = outputEdit->text().trimmed();
        const QString path = QDir::isAbsolutePath(name)
            ? name : QDir(outputDirectory).filePath(name);
        const QString location = name.isEmpty() ? outputDirectory : QFileInfo(path).absolutePath();
        destinationLabel->setText(i18n("Location: %1", QDir::toNativeSeparators(location)));
        destinationLabel->setToolTip(location);
    };
    updateDestination();
    root->addWidget(destinationLabel);
    QObject::connect(outputEdit, &QLineEdit::textChanged, &dialog,
                     [&](const QString &) { updateDestination(); });

    QObject::connect(browseButton, &QPushButton::clicked, &dialog, [&]() {
        const QString current = outputEdit->text().trimmed();
        const QString suggested = QDir::isAbsolutePath(current)
            ? current : QDir(outputDirectory).filePath(current);
        const QString path = QFileDialog::getSaveFileName(
            &dialog, i18n("Select output image"), suggested,
            i18n("DwarFS images (*.dwarfs);;All files (*)"));
        if (!path.isEmpty()) {
            const QFileInfo chosen(path);
            outputDirectory = chosen.absolutePath();
            outputEdit->setText(chosen.fileName());
            updateDestination();
        }
    });

    addDivider();
    auto *compressionHeader = new QWidget(&dialog);
    auto *compressionLayout = new QHBoxLayout(compressionHeader);
    compressionLayout->setContentsMargins(0, 0, 0, 0);
    auto *compressionTitle = new QLabel(i18n("Compression"), compressionHeader);
    compressionTitle->setFont(sectionFont);
    compressionLayout->addWidget(compressionTitle);
    compressionLayout->addStretch();
    auto *whyButton = new QToolButton(compressionHeader);
    whyButton->setObjectName(QStringLiteral("buildWhy"));
    whyButton->setText(i18n("Why this setting?"));
    whyButton->setAutoRaise(true);
    whyButton->hide();
    compressionLayout->addWidget(whyButton);
    root->addWidget(compressionHeader);

    auto *compressionStatus = new QLabel(&dialog);
    compressionStatus->setObjectName(QStringLiteral("compressionStatus"));
    compressionStatus->setWordWrap(true);
    compressionStatus->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    compressionStatus->setText(i18n("Loading image creation settings…"));
    compressionStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(compressionStatus);

    auto *detailsPanel = new QFrame(&dialog);
    detailsPanel->setObjectName(QStringLiteral("buildDetailsPanel"));
    detailsPanel->setFrameShape(QFrame::StyledPanel);
    auto *detailsLayout = new QVBoxLayout(detailsPanel);
    detailsLayout->setContentsMargins(12, 10, 12, 10);
    auto *recommendationDetails = new QLabel(detailsPanel);
    recommendationDetails->setObjectName(QStringLiteral("buildRecommendationDetails"));
    recommendationDetails->setWordWrap(true);
    recommendationDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
    recommendationDetails->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    detailsLayout->addWidget(recommendationDetails);
    detailsPanel->hide();
    root->addWidget(detailsPanel);
    QObject::connect(whyButton, &QToolButton::clicked, &dialog, [=, &dialog] {
        const bool expanded = !detailsPanel->isVisible();
        detailsPanel->setVisible(expanded);
        whyButton->setText(expanded ? i18n("Hide explanation") : i18n("Why this setting?"));
        QTimer::singleShot(0, &dialog, [&dialog] { dialog.adjustSize(); });
    });

    root->addWidget(banner);

    auto *moreButton = new QToolButton(&dialog);
    moreButton->setObjectName(QStringLiteral("buildMoreOptions"));
    moreButton->setText(i18n("More options"));
    moreButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    moreButton->setArrowType(Qt::RightArrow);
    moreButton->setAutoRaise(true);
    root->addWidget(moreButton, 0, Qt::AlignLeft);
    auto *advancedPanel = new QWidget(&dialog);
    advancedPanel->setObjectName(QStringLiteral("buildOptionsPanel"));
    auto *advancedLayout = new QVBoxLayout(advancedPanel);
    advancedLayout->setContentsMargins(20, 0, 0, 0);
    auto *force = new QCheckBox(i18n("Replace an existing image"), advancedPanel);
    force->setObjectName(QStringLiteral("buildReplaceExisting"));
    auto *previewCheck = new QCheckBox(i18n("Measure image size before creating"), advancedPanel);
    previewCheck->setObjectName(QStringLiteral("buildPreviewOnly"));
    auto *previewHint = new QWidget(advancedPanel);
    previewHint->setObjectName(QStringLiteral("buildPreviewHint"));
    auto *previewHintLayout = new QVBoxLayout(previewHint);
    previewHintLayout->setContentsMargins(0, 0, 0, 0);
    previewHintLayout->setSpacing(2);
    auto *previewDescription = new QLabel(i18n("Builds and measures a temporary image. Create it from the preview, or close to discard it."), previewHint);
    previewDescription->setObjectName(QStringLiteral("buildPreviewDescription"));
    previewDescription->setWordWrap(true);
    auto *previewTime = new QLabel(i18n("This may take as long as creating the image."), previewHint);
    previewTime->setObjectName(QStringLiteral("buildPreviewTime"));
    previewTime->setWordWrap(true);
    previewHintLayout->addWidget(previewDescription);
    previewHintLayout->addWidget(previewTime);
    previewCheck->setToolTip(i18n("Builds a temporary image to measure its actual size. Create it from the preview, or close to discard it. This may take as long as creating the image."));
    previewHint->hide();
    advancedLayout->addWidget(force);
    advancedLayout->addWidget(previewCheck);
    advancedLayout->addWidget(previewHint);
    QObject::connect(previewCheck, &QCheckBox::toggled, &dialog, [=, &dialog](bool checked) {
        previewHint->setVisible(checked);
        previewHint->updateGeometry();
        advancedLayout->activate();
        QTimer::singleShot(0, &dialog, [&dialog] { dialog.adjustSize(); });
    });
    advancedPanel->hide();
    root->addWidget(advancedPanel);
    QObject::connect(moreButton, &QToolButton::clicked, &dialog, [=, &dialog] {
        const bool expanded = !advancedPanel->isVisible();
        advancedPanel->setVisible(expanded);
        moreButton->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        QTimer::singleShot(0, &dialog, [&dialog] { dialog.adjustSize(); });
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    auto *createButton = buttons->button(QDialogButtonBox::Ok);
    createButton->setText(i18n("Create"));
    createButton->setEnabled(false);
    QObject::connect(previewCheck, &QCheckBox::toggled, &dialog,
                     [=](bool preview) { createButton->setText(preview ? i18n("Preview") : i18n("Create")); });
    auto *retryButton = buttons->addButton(i18n("Retry"), QDialogButtonBox::ActionRole);
    retryButton->setObjectName(QStringLiteral("buildRetry"));
    retryButton->hide();
    root->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    QStringList compressionArgs;
    QString compressionName;
    bool ready = false;
    bool loading = false;
    auto updateCreate = [&] { createButton->setEnabled(ready && !loading && !outputEdit->text().trimmed().isEmpty()); };
    QObject::connect(outputEdit, &QLineEdit::textChanged, &dialog, [&](const QString &) { updateCreate(); });
    auto fail = [&](const QString &message) {
        loading = false;
        ready = false;
        updateCreate();
        compressionStatus->setText(i18n("Compression settings unavailable"));
        banner->setTitleAndBody(i18n("Image creation unavailable"), message);
        banner->show();
        retryButton->show();
        retryButton->setEnabled(true);
    };
    auto prepared = [&] {
        loading = false;
        ready = true;
        updateCreate();
        retryButton->hide();
    };

    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        if (!ready || loading) return;
        result.accepted = true;
        const QString imageName = outputEdit->text().trimmed();
        result.outputPath = QDir::isAbsolutePath(imageName)
            ? imageName : QDir(outputDirectory).filePath(imageName);
        result.profileName = compressionName;
        result.profileArgs = compressionArgs;
        result.force = force->isChecked();
        result.preview = previewCheck->isChecked();
        dialog.accept();
    });

    std::function<void()> load;
    load = [&] {
        if (loading) return;
        loading = true;
        ready = false;
        updateCreate();
        retryButton->setEnabled(false);
        banner->hide();
        whyButton->hide();
        detailsPanel->hide();
        whyButton->setText(i18n("Why this setting?"));
        compressionStatus->setText(i18n("Loading image creation settings…"));
        BackendQuery::ui(&dialog, [&](bool valid, const dwarfsctl_kde::UiConfig &, const QString &error) {
            if (!valid) { fail(error); return; }
            BackendQuery::json(&dialog, {QStringLiteral("config"), QStringLiteral("show"),
                                         QStringLiteral("--section"), QStringLiteral("creation")},
                               [&](const BackendReply &reply) {
                CreationDefaults defaults;
                if (!reply.ok || !parseCreationDefaults(reply.envelope.details, &defaults)) {
                    fail(reply.ok ? i18n("The image creation settings response is invalid.") : reply.error);
                    return;
                }
                BackendQuery::json(&dialog, {QStringLiteral("mkdwarfs"), QStringLiteral("schema")},
                                   [&, defaults](const BackendReply &schemaReply) {
                    const auto schema = schemaReply.envelope.details.value(QStringLiteral("schema"));
                    if (!schemaReply.ok || !validCreationSchema(schema)) {
                        fail(schemaReply.ok ? i18n("The image creation schema response is invalid.") : schemaReply.error);
                        return;
                    }
                    if (defaults.mode == QLatin1String("profile")) {
                        if (defaults.profile == QLatin1String("default")) {
                            compressionName = defaults.profile;
                            compressionArgs.clear();
                            compressionStatus->setText(i18n("Default compression"));
                            prepared();
                            return;
                        }
                        BackendQuery::json(&dialog, {QStringLiteral("config"), QStringLiteral("mkdwarfs-profiles"),
                                                     QStringLiteral("get"), defaults.profile},
                                           [&, defaults](const BackendReply &profileReply) {
                            QStringList args;
                            if (!profileReply.ok || !parseProfileArgs(profileReply.envelope.details, &args)) {
                                fail(profileReply.ok ? i18n("The selected profile response is invalid.") : profileReply.error);
                                return;
                            }
                            compressionName = defaults.profile;
                            compressionArgs = args;
                            compressionStatus->setText(i18n("Profile: %1", defaults.profile));
                            prepared();
                        });
                        return;
                    }
                    compressionStatus->setText(i18n("Choosing the best settings for this folder…"));
                    DwarfsctlJob::Request request;
                    request.title = i18n("Choose compression settings automatically");
                    request.args = {QStringLiteral("mkdwarfs"), QStringLiteral("recommend"), sourceDir,
                                    QStringLiteral("--goal"), defaults.goal};
                    request.timeout_ms = 30000;
                    request.show_ui = false;
                    request.responseLimit = BackendQuery::ResponseLimit;
                    auto *job = new DwarfsctlJob(request, &dialog);
                    QObject::connect(job, &KJob::result, &dialog, [&, job, defaults] {
                        const auto recommendation = parseRecommendation(job->envelope().details);
                        if (job->error() || !recommendation.valid) {
                            fail(job->error() ? job->errorString() : i18n("The recommendation response was invalid."));
                            return;
                        }
                        compressionName = QStringLiteral("recommended");
                        compressionArgs = recommendation.flags;
                        compressionStatus->setText(i18n("Optimized for %1", goalLabel(defaults.goal)));
                        QStringList explanation = recommendation.reasons;
                        explanation.append(recommendation.notes);
                        explanation.append(i18n("Change this default in DwarFS tray Settings."));
                        recommendationDetails->setText(explanation.join(QStringLiteral("\n\n")));
                        whyButton->show();
                        if (!recommendation.warnings.isEmpty()) {
                            banner->setTitleAndBody(i18n("Some items could not be fully considered"),
                                                    recommendation.warnings.join(QStringLiteral("\n")));
                            banner->show();
                        }
                        prepared();
                    });
                    job->start();
                });
            });
        });
    };
    QObject::connect(retryButton, &QPushButton::clicked, &dialog, [&] { load(); });
    QTimer::singleShot(0, &dialog, [&] { load(); });

    dialog.exec();
    return result;
}

} // namespace dwarfsplugin
