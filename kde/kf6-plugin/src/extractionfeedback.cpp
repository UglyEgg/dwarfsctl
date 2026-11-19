// SPDX-License-Identifier: GPL-3.0-or-later
#include "extractionfeedback.h"
#include "uipresentation.h"
#include "operationstatus.h"
#include "detailsdialog.h"

#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <KLocalizedString>

ExtractionFeedback::ExtractionFeedback(const QString &destination, bool selected, QWidget *parent)
    : QDialog(parent), m_destination(destination)
{
    setObjectName(QStringLiteral("extractionFeedback"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(selected ? i18n("Extract Selected Items") : i18n("Extract Image"));
    setWindowModality(Qt::WindowModal);
    setMinimumWidth(480);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(dwarfsctl_kde::contentMargins(this));
    layout->setSpacing(dwarfsctl_kde::layoutSpacing(this));
    auto *name = new QLabel(QFileInfo(destination).fileName(), this);
    name->setTextFormat(Qt::PlainText);
    name->setWordWrap(true);
    name->setFont(dwarfsctl_kde::headingFont(name->font()));
    layout->addWidget(name);
    m_status = new dwarfsctl_kde::OperationStatus(this, QStringLiteral("extractionStatus"));
    m_status->begin(i18n("Extracting…"));
    layout->addWidget(m_status);
    auto *path = new QLabel(QDir::toNativeSeparators(destination), this);
    path->setObjectName(QStringLiteral("extractionDestination"));
    path->setTextFormat(Qt::PlainText);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setWordWrap(true);
    layout->addWidget(path);
    m_summary = new QLabel(i18n("The original folder structure will be kept inside this new folder."), this);
    m_summary->setObjectName(QStringLiteral("extractionSummary"));
    m_summary->setTextFormat(Qt::PlainText);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close | QDialogButtonBox::Cancel, this);
    m_close = buttons->button(QDialogButtonBox::Close);
    m_close->setEnabled(false);
    m_close->setObjectName(QStringLiteral("closeExtraction"));
    m_cancel = buttons->button(QDialogButtonBox::Cancel);
    m_cancel->setObjectName(QStringLiteral("cancelExtraction"));
    m_open = buttons->addButton(i18n("Open Extracted Folder"), QDialogButtonBox::ActionRole);
    m_open->setObjectName(QStringLiteral("openExtractedFolder"));
    m_open->setEnabled(false);
    connect(m_open, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_destination));
    });
    connect(m_cancel, &QPushButton::clicked, this, [this] { requestCancel(false); });
    connect(m_close, &QPushButton::clicked, this, &ExtractionFeedback::reject);
    m_details = buttons->addButton(i18n("Details…"), QDialogButtonBox::ActionRole);
    m_details->setObjectName(QStringLiteral("extractionDetails"));
    m_details->hide();
    connect(m_details, &QPushButton::clicked, this, [this] {
        dwarfsplugin::DetailsDialog dialog(m_envelope, i18n("Extraction Details"), this);
        dialog.exec();
    });
    layout->addWidget(buttons);

}

void ExtractionFeedback::finish()
{
    m_cancel->hide();
    m_close->setEnabled(true);
    m_close->setDefault(true);
}

void ExtractionFeedback::complete(const dwarfsplugin::Envelope &env)
{
    if (!m_status->isActive()) return;
    const QFileInfo output(m_destination);
    if (!output.isDir() || output.isSymLink()) {
        failed(i18n("The expected extracted folder could not be found."));
        return;
    }
    m_envelope = env;
    m_details->setVisible(!env.warnings.isEmpty() || !env.warning_objects.isEmpty());
    finish();
    m_status->succeed(i18n("Extraction complete"));
    m_summary->setText(i18n("Your files are ready in the folder above.")
        + ((!env.warnings.isEmpty() || !env.warning_objects.isEmpty())
            ? QLatin1Char('\n') + i18n("Completed with warnings. See Details for more information.") : QString()));
    m_open->setEnabled(true);
    if (m_closeWhenStopped) QDialog::reject();
}

void ExtractionFeedback::failed(const QString &message, const dwarfsplugin::Envelope &env)
{
    if (!m_status->isActive()) return;
    m_envelope = env;
    m_status->fail(i18n("Could Not Extract"));
    m_details->setVisible(env.has_error || !env.details.isEmpty());
    finish();
    m_summary->setText(message.isEmpty() ? i18n("The extraction could not be completed.") : message);
    if (m_closeWhenStopped) QDialog::reject();
}

void ExtractionFeedback::cancelled()
{
    if (!m_status->isActive()) return;
    m_status->cancelled();
    m_summary->setText(i18n("Extraction was cancelled."));
    finish();
    if (m_closeWhenStopped) QDialog::reject();
}

void ExtractionFeedback::requestCancel(bool closeWhenStopped)
{
    if (!m_status->isActive()) return;
    m_closeWhenStopped |= closeWhenStopped;
    if (m_cancelling) return;
    m_cancelling = true;
    m_cancel->setEnabled(false);
    m_status->cancelling();
    Q_EMIT cancelRequested();
}

void ExtractionFeedback::reject()
{
    if (m_status->isActive()) requestCancel(true);
    else QDialog::reject();
}

void ExtractionFeedback::closeEvent(QCloseEvent *event)
{
    if (m_status->isActive()) {
        event->ignore();
        requestCancel(true);
    } else QDialog::closeEvent(event);
}
