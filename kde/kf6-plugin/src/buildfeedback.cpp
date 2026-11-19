// SPDX-License-Identifier: GPL-3.0-or-later
#include "buildfeedback.h"
#include "detailsdialog.h"
#include "operationstatus.h"
#include "uipresentation.h"
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLayout>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QTemporaryDir>
#include <QUrl>
#include <QVBoxLayout>
#include <KLocalizedString>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace dwarfsplugin {
BuildFeedback::BuildFeedback(const QString &output, QWidget *parent, bool preview)
    : QDialog(parent), m_preview(preview), m_output(output)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::WindowModal);
    setWindowTitle(preview ? i18n("Preview DwarFS Image") : i18n("Create DwarFS Image"));
    setMinimumWidth(520);
    if (preview) {
        const auto parentDir = QFileInfo(QFileInfo(output).absolutePath()).canonicalFilePath();
        if (!parentDir.isEmpty()) m_stage = std::make_unique<QTemporaryDir>(parentDir + QStringLiteral("/.dwarfsctl-preview-XXXXXX"));
        if (m_stage && m_stage->isValid()) {
            m_stageFd = ::open(QFile::encodeName(m_stage->path()).constData(),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            // The sticky bit marks a recoverable stage without adding a file
            // that the writer could see when the destination is inside the source.
            if (m_stageFd < 0 || ::flock(m_stageFd, LOCK_SH | LOCK_NB) != 0
                || ::fchmod(m_stageFd, S_ISVTX | S_IRWXU) != 0) {
                discardStage();
            }
        } else {
            m_stage.reset();
        }
    }
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(dwarfsctl_kde::contentMargins(this));
    layout->setSpacing(dwarfsctl_kde::layoutSpacing(this));
    auto *name = new QLabel(QFileInfo(output).fileName(), this);
    name->setTextFormat(Qt::PlainText);
    name->setWordWrap(true);
    name->setFont(dwarfsctl_kde::headingFont(name->font()));
    layout->addWidget(name);
    m_status = new dwarfsctl_kde::OperationStatus(this, QStringLiteral("buildStatus"));
    m_status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_status->setCompactCompletion(true);
    m_status->begin(preview ? i18n("Measuring image…") : i18n("Preparing image…"));
    layout->addWidget(m_status);

    m_metrics = new QFrame(this);
    m_metrics->setObjectName(QStringLiteral("buildMetrics"));
    m_metrics->setFrameShape(QFrame::StyledPanel);
    auto *metricsLayout = new QVBoxLayout(m_metrics);
    metricsLayout->setContentsMargins(14, 12, 14, 12);
    auto *values = new QGridLayout();
    values->setColumnStretch(1, 1);
    auto addValue = [&](int row, const QString &title, QLabel **value, const QString &name) {
        values->addWidget(new QLabel(title, m_metrics), row, 0);
        *value = new QLabel(m_metrics);
        (*value)->setObjectName(name);
        (*value)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        values->addWidget(*value, row, 1);
    };
    addValue(0, i18n("File contents"), &m_originalValue, QStringLiteral("buildOriginalSize"));
    addValue(1, i18n("DwarFS image"), &m_imageValue, QStringLiteral("buildImageSize"));
    addValue(2, i18n("Space saved"), &m_savedValue, QStringLiteral("buildSavedSize"));
    metricsLayout->addLayout(values);
    m_savingsBar = new QProgressBar(m_metrics);
    m_savingsBar->setObjectName(QStringLiteral("buildSavingsBar"));
    m_savingsBar->setRange(0, 100);
    m_savingsBar->setTextVisible(false);
    m_savingsBar->setAccessibleName(i18n("Space saved relative to source content"));
    metricsLayout->addWidget(m_savingsBar);

    m_typeSection = new QWidget(m_metrics);
    m_typeSection->setObjectName(QStringLiteral("buildSourceTypes"));
    auto *typeLayout = new QVBoxLayout(m_typeSection);
    typeLayout->setContentsMargins(0, 6, 0, 0);
    auto *typeTitle = new QLabel(i18n("Files by type · original size"), m_typeSection);
    typeTitle->setToolTip(i18n("DwarFS shares compressed blocks, so a reliable compressed size cannot be assigned to each file."));
    auto font = typeTitle->font();
    font.setBold(true);
    typeTitle->setFont(font);
    typeLayout->addWidget(typeTitle);
    m_typeGrid = new QGridLayout();
    m_typeGrid->setColumnStretch(0, 1);
    typeLayout->addLayout(m_typeGrid);
    m_typeSection->hide();
    metricsLayout->addWidget(m_typeSection);
    m_metrics->hide();
    layout->addWidget(m_metrics);

    m_summary = new QLabel(preview
        ? i18n("A temporary image will be built and measured. You can then create it without building it again, or close this preview to discard it.")
        : output, this);
    m_summary->setTextFormat(Qt::PlainText);
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_summary->setObjectName(QStringLiteral("buildSummary"));
    layout->addWidget(m_summary);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close | QDialogButtonBox::Cancel, this);
    m_close = buttons->button(QDialogButtonBox::Close);
    m_close->setObjectName(QStringLiteral("closeBuild"));
    m_close->setEnabled(false);
    m_cancel = buttons->button(QDialogButtonBox::Cancel);
    m_cancel->setObjectName(QStringLiteral("cancelBuild"));
    connect(m_cancel, &QPushButton::clicked, this, [this] { requestCancel(false); });
    connect(m_close, &QPushButton::clicked, this, &BuildFeedback::reject);
    m_reveal = buttons->addButton(i18n("Open Containing Folder"), QDialogButtonBox::ActionRole);
    m_reveal->setObjectName(QStringLiteral("openBuildFolder"));
    m_reveal->setEnabled(false);
    m_reveal->setVisible(!preview);
    connect(m_reveal, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_output).absolutePath()));
    });
    m_details = buttons->addButton(i18n("Details…"), QDialogButtonBox::ActionRole);
    m_details->setObjectName(QStringLiteral("buildDetails"));
    m_details->hide();
    connect(m_details, &QPushButton::clicked, this, [this] {
        DetailsDialog dialog(m_envelope, i18n("Image Creation Details"), this);
        dialog.exec();
    });
    m_publish = buttons->addButton(i18n("Create this image"), QDialogButtonBox::ActionRole);
    m_publish->setObjectName(QStringLiteral("createMeasuredImage"));
    m_publish->hide();
    connect(m_publish, &QPushButton::clicked, this, [this] {
        if (!m_preview || m_publishing || m_candidate.isEmpty() || m_status->isActive()) return;
        m_publishing = true;
        m_cancelling = false;
        m_publish->hide();
        m_close->setEnabled(false);
        m_status->begin(i18n("Saving the measured image…"));
        Q_EMIT createRequested(m_candidate, m_measuredBytes);
    });
    layout->addWidget(buttons);
    show();
}
BuildFeedback::~BuildFeedback()
{
    discardStage();
}
void BuildFeedback::discardStage()
{
    m_stage.reset();
    if (m_stageFd >= 0) {
        ::close(m_stageFd);
        m_stageFd = -1;
    }
}
QString BuildFeedback::previewStage() const
{
    return m_stage ? m_stage->path() : QString();
}
void BuildFeedback::phase(const QString &value)
{
    if (value == QLatin1String("scanning")) m_status->phase(i18n("Scanning files…"));
    else if (value == QLatin1String("compressing")) m_status->phase(i18n("Compressing files…"));
    else if (value == QLatin1String("metadata")) m_status->phase(i18n("Writing image metadata…"));
    else if (value == QLatin1String("verifying")) m_status->phase(i18n("Checking the completed image…"));
}
void BuildFeedback::finish()
{
    m_cancel->hide();
    m_close->setEnabled(true);
    m_close->setDefault(true);
}
void BuildFeedback::complete(const Envelope &env)
{
    if (!m_status->isActive()) return;
    m_envelope = env;
    const auto summary = env.details.value(QStringLiteral("summary")).toObject();
    if (summary.isEmpty() || summary.value(QStringLiteral("preview")).toBool() != m_preview) {
        failed(i18n("The image size result was incomplete. Open Details for more information."), env);
        return;
    }
    const bool created = !m_preview;
    const QFileInfo output(m_output);
    if (created && (!output.isFile() || output.isSymLink())) {
        failed(i18n("The expected image file could not be found. Open Details for more information."), env);
        return;
    }
    const QLocale locale;
    const auto bytes = summary.value(QStringLiteral("image_bytes")).toInteger(-1);
    const auto original = summary.value(QStringLiteral("original_bytes")).toInteger(-1);
    if (bytes < 0 || original < 0) {
        failed(i18n("The image size result was incomplete. Open Details for more information."), env);
        return;
    }
    if (m_preview) {
        const auto candidate = env.details.value(QStringLiteral("preview_candidate")).toString();
        const QFileInfo staged(candidate);
        if (candidate.isEmpty() || !m_stage || staged.isSymLink() || !staged.isFile()
            || staged.absolutePath() != m_stage->path() || staged.size() != bytes) {
            failed(i18n("The measured image could not be retained. Open Details for more information."), env);
            return;
        }
        m_candidate = candidate;
        m_measuredBytes = bytes;
    }
    m_originalValue->setText(locale.formattedDataSize(original));
    m_imageValue->setText(locale.formattedDataSize(bytes));
    const auto saved = original - bytes;
    if (saved >= 0) {
        const auto percent = original > 0 ? qRound(100.0 * saved / original) : 0;
        m_savedValue->setText(i18n("%1 (%2%)", locale.formattedDataSize(saved), percent));
        m_savingsBar->setValue(percent);
        m_savingsBar->setToolTip(i18n("%1% of source content saved", percent));
    } else {
        m_savedValue->setText(i18n("Image is %1 larger", locale.formattedDataSize(-saved)));
        m_savingsBar->setValue(0);
    }
    const auto types = summary.value(QStringLiteral("source_types")).toArray();
    int row = 0;
    for (const auto &value : types) {
        const auto item = value.toObject();
        const auto type = item.value(QStringLiteral("type")).toString();
        const auto count = item.value(QStringLiteral("files")).toInteger(-1);
        const auto size = item.value(QStringLiteral("original_bytes")).toInteger(-1);
        if (type.isEmpty() || count < 0 || size < 0) continue;
        auto *typeLabel = new QLabel(type, m_typeSection);
        typeLabel->setTextFormat(Qt::PlainText);
        m_typeGrid->addWidget(typeLabel, row, 0);
        auto *detail = new QLabel(i18n("%1 files · %2", count, locale.formattedDataSize(size)), m_typeSection);
        detail->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_typeGrid->addWidget(detail, row, 1);
        ++row;
    }
    m_typeSection->setVisible(row > 0);
    m_metrics->show();
    m_summary->setText(created ? i18n("Saved to: %1", m_output) : i18n("Would save as: %1", m_output));
    m_status->succeed(created ? i18n("Image created successfully") : i18n("Measured preview complete — no image was created"));
    finish();
    if (!env.warnings.isEmpty() || !env.warning_objects.isEmpty())
        m_summary->setText(m_summary->text() + QLatin1Char('\n') + i18n("Completed with warnings. See Details for more information."));
    m_details->setVisible(!env.warnings.isEmpty() || !env.warning_objects.isEmpty() || env.has_error);
    m_reveal->setEnabled(created);
    m_publish->setVisible(m_preview && !m_candidate.isEmpty());
    layout()->invalidate();
    layout()->activate();
    adjustSize();
    if (m_closeWhenStopped) QDialog::reject();
}
void BuildFeedback::published(const Envelope &env)
{
    if (!m_publishing || !m_status->isActive()) return;
    m_envelope = env;
    const auto details = env.details;
    const QFileInfo output(m_output);
    if (!details.value(QStringLiteral("published_preview")).toBool()
        || !details.value(QStringLiteral("output_verified")).toBool()
        || details.value(QStringLiteral("image_bytes")).toInteger(-1) != m_measuredBytes
        || !output.isFile() || output.isSymLink() || output.size() != m_measuredBytes) {
        publishFailed(i18n("The saved image could not be verified."), env);
        return;
    }
    m_preview = false;
    m_publishing = false;
    m_candidate.clear();
    discardStage();
    m_status->succeed(i18n("Image created successfully"));
    m_summary->setText(i18n("Saved to: %1", m_output));
    m_reveal->show();
    m_reveal->setEnabled(true);
    finish();
    layout()->invalidate();
    layout()->activate();
    adjustSize();
}
void BuildFeedback::publishFailed(const QString &message, const Envelope &env)
{
    if (!m_publishing || !m_status->isActive()) return;
    m_envelope = env;
    m_publishing = false;
    m_status->fail(i18n("Could Not Save Image"));
    m_summary->setText(message.isEmpty() ? i18n("The measured image could not be saved.") : message);
    m_details->setVisible(env.has_error || !env.details.isEmpty());
    m_publish->setVisible(!m_candidate.isEmpty());
    finish();
    layout()->invalidate();
    layout()->activate();
    adjustSize();
}
void BuildFeedback::failed(const QString &message, const Envelope &env)
{
    if (!m_status->isActive()) return;
    m_envelope = env;
    m_status->fail(m_preview ? i18n("Could Not Measure Image") : i18n("Could Not Create Image"));
    m_summary->setText(message.isEmpty() ? i18n("The image could not be created.") : message);
    m_details->setVisible(env.has_error || !env.details.isEmpty());
    finish();
    if (m_closeWhenStopped) QDialog::reject();
}
void BuildFeedback::cancelled()
{
    if (!m_status->isActive()) return;
    m_status->cancelled();
    m_summary->setText(m_preview ? i18n("Image preview was cancelled.") : i18n("Image creation was cancelled."));
    finish();
    if (m_closeWhenStopped) QDialog::reject();
}
void BuildFeedback::requestCancel(bool closeWhenStopped)
{
    if (m_publishing) return;
    if (!m_status->isActive()) return;
    m_closeWhenStopped |= closeWhenStopped;
    if (m_cancelling) return;
    m_cancelling = true;
    m_cancel->setEnabled(false);
    m_status->cancelling();
    Q_EMIT cancelRequested();
}
void BuildFeedback::reject()
{
    if (m_publishing) return;
    if (m_status->isActive()) requestCancel(true);
    else QDialog::reject();
}
void BuildFeedback::closeEvent(QCloseEvent *event)
{
    if (m_publishing) { event->ignore(); return; }
    if (m_status->isActive()) { event->ignore(); requestCancel(true); }
    else QDialog::closeEvent(event);
}
}
