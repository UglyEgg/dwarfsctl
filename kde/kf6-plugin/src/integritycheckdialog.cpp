// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "integritycheckdialog.h"
#include "bannerwidget.h"
#include "detailsdialog.h"
#include "dwarfsctljob.h"
#include "operationstatus.h"
#include "uipresentation.h"
#include <QCloseEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QTimer>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <KLocalizedString>

namespace dwarfsplugin {
namespace {
bool hasValue(const QJsonValue &value)
{
    if (value.isNull() || value.isUndefined()) return false;
    if (value.isString()) return !value.toString().trimmed().isEmpty();
    if (value.isArray()) {
        for (const auto &entry : value.toArray()) if (hasValue(entry)) return true;
        return false;
    }
    if (value.isObject()) {
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) if (hasValue(it.value())) return true;
        return false;
    }
    return true;
}

bool hasDetails(const Envelope &env)
{
    if (env.has_error) return true;
    for (const auto &warning : env.warnings) if (!warning.trimmed().isEmpty()) return true;
    for (const auto &warning : env.warning_objects)
        if (!warning.code.trimmed().isEmpty() || !warning.message.trimmed().isEmpty()) return true;
    for (auto it = env.details.begin(); it != env.details.end(); ++it)
        if (!it.key().startsWith(QLatin1Char('_')) && hasValue(it.value())) return true;
    return false;
}

QStringList warningMessages(const Envelope &env)
{
    QStringList messages;
    for (const auto &warning : env.warnings) {
        if (!warning.trimmed().isEmpty()) messages.append(warning.trimmed());
    }
    for (const auto &warning : env.warning_objects) {
        const auto message = warning.message.trimmed().isEmpty() ? warning.code.trimmed() : warning.message.trimmed();
        if (!message.isEmpty()) messages.append(message);
    }
    return messages;
}
}

IntegrityCheckDialog::IntegrityCheckDialog(const QString &imagePath, QWidget *parent)
    : QDialog(parent), m_imagePath(imagePath)
{
    setWindowTitle(i18n("Integrity Check"));
    setModal(true);
    setMinimumWidth(480);
    resize(560, 180);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(dwarfsctl_kde::contentMargins(this));
    root->setSpacing(dwarfsctl_kde::layoutSpacing(this));

    auto *header = new QWidget(this);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(dwarfsctl_kde::layoutSpacing(this));
    auto *icon = new QLabel(header);
    icon->setPixmap(QIcon::fromTheme(QStringLiteral("dwarfsctlimage"),
        QIcon::fromTheme(QStringLiteral("drive-harddisk"))).pixmap(48, 48));
    headerLayout->addWidget(icon, 0, Qt::AlignTop);
    auto *heading = new QWidget(header);
    auto *headingLayout = new QVBoxLayout(heading);
    headingLayout->setContentsMargins(0, 0, 0, 0);
    headingLayout->setSpacing(2);
    const QFileInfo image(imagePath);
    auto *name = new QLabel(image.fileName().isEmpty() ? i18n("DwarFS image") : image.fileName(), heading);
    name->setObjectName(QStringLiteral("integrityImageName"));
    name->setTextFormat(Qt::PlainText);
    name->setFont(dwarfsctl_kde::headingFont(name->font()));
    name->setWordWrap(true);
    name->setToolTip(image.absoluteFilePath());
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headingLayout->addWidget(name);
    auto *intro = new QLabel(i18n("Check this image for errors. Large images may take longer."), heading);
    intro->setTextFormat(Qt::PlainText);
    intro->setWordWrap(true);
    headingLayout->addWidget(intro);
    headerLayout->addWidget(heading, 1);
    root->addWidget(header);

    m_status = new dwarfsctl_kde::OperationStatus(this, QStringLiteral("integrityStatusText"));
    m_status->setCompactCompletion(true);
    m_status->hide();
    root->addWidget(m_status);
    m_resultDetail = new QLabel(this);
    m_resultDetail->setObjectName(QStringLiteral("integrityResultDetail"));
    m_resultDetail->setTextFormat(Qt::PlainText);
    m_resultDetail->setWordWrap(true);
    m_resultDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_resultDetail->hide();
    root->addWidget(m_resultDetail);
    m_warnings = new BannerWidget(this);
    m_warnings->setSeverity(BannerWidget::Severity::Warning);
    root->addWidget(m_warnings);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("integrityCloseButton"));
    m_runBtn = buttons->addButton(i18n("Check Image"), QDialogButtonBox::ActionRole);
    m_runBtn->setObjectName(QStringLiteral("integrityRunButton"));
    m_cancelBtn = buttons->addButton(i18n("Cancel"), QDialogButtonBox::ActionRole);
    m_cancelBtn->setObjectName(QStringLiteral("integrityCancelButton"));
    m_cancelBtn->hide();
    m_detailsBtn = buttons->addButton(i18n("Technical Details…"), QDialogButtonBox::ActionRole);
    m_detailsBtn->setObjectName(QStringLiteral("integrityDetailsButton"));
    m_detailsBtn->setEnabled(false);
    m_detailsBtn->hide();
    connect(m_runBtn, &QPushButton::clicked, this, &IntegrityCheckDialog::start);
    connect(m_cancelBtn, &QPushButton::clicked, this, &IntegrityCheckDialog::requestCancel);
    connect(m_detailsBtn, &QPushButton::clicked, this, [this] {
        if (!m_hasEnv || m_job) return;
        DetailsDialog dialog(m_lastEnv, i18n("Integrity Check Details"), this);
        dialog.exec();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &IntegrityCheckDialog::reject);
    root->addWidget(buttons);
}
void IntegrityCheckDialog::start()
{
    if (m_job) return;
    m_cancelling = false;
    m_closeWhenStopped = false;
    m_hasEnv = false;
    m_runBtn->setEnabled(false);
    m_detailsBtn->setEnabled(false);
    m_detailsBtn->hide();
    m_cancelBtn->setEnabled(true);
    m_cancelBtn->show();
    m_resultDetail->hide();
    m_warnings->hide();
    m_status->show();
    m_status->begin(i18n("Checking image integrity…"));
    auto *job = new DwarfsctlJob({i18n("Integrity Check"),
        {QStringLiteral("fsck"), m_imagePath, QStringLiteral("--arg"), QStringLiteral("--check-integrity")}, 0, false}, this);
    m_job = job;
    connect(job, &KJob::result, this, [this, job] {
        if (m_job != job) return;
        m_job = nullptr;
        m_lastEnv = job->envelope();
        m_hasEnv = true;
        m_runBtn->setEnabled(true);
        m_runBtn->setText(i18n("Check Again"));
        m_cancelBtn->hide();
        if (job->error() == KJob::KilledJobError || m_lastEnv.details.value(QStringLiteral("cancelled")).toBool()) {
            m_status->cancelled();
        } else if (job->error() || !m_lastEnv.ok) {
            QString message = m_lastEnv.error.message;
            if (message.isEmpty()) message = job->errorString();
            if (!message.isEmpty() && m_lastEnv.error.message.isEmpty()) {
                m_lastEnv.error.message = message;
                m_lastEnv.has_error = true;
            }
            m_status->fail(i18n("Integrity check failed."));
            if (!message.isEmpty()) {
                m_resultDetail->setText(message);
                m_resultDetail->show();
            }
        } else {
            m_status->succeed(i18n("Integrity check passed."));
        }
        const auto warnings = warningMessages(m_lastEnv);
        if (!warnings.isEmpty()) {
            m_warnings->setTitleAndBody(i18np("1 warning", "%1 warnings", warnings.size()),
                warnings.mid(0, 3).join(QStringLiteral(" · ")));
            if (warnings.size() > 3) m_warnings->setDetails(i18n("More in Technical Details"));
            else m_warnings->setDetails({});
            m_warnings->show();
        }
        const bool detailsAvailable = hasDetails(m_lastEnv)
            && m_status->state() != dwarfsctl_kde::OperationStatus::State::Cancelled;
        m_detailsBtn->setVisible(detailsAvailable);
        m_detailsBtn->setEnabled(detailsAvailable);
        if (m_closeWhenStopped) QDialog::reject();
    });
    job->start();
}
void IntegrityCheckDialog::requestCancel()
{
    if (!m_job || m_cancelling) return;
    m_cancelling = true;
    m_cancelBtn->setEnabled(false);
    m_status->cancelling();
    Q_EMIT operationCancelRequested();
    const auto job = m_job;
    QTimer::singleShot(0, this, [this, job] {
        if (job && m_job == job) job->kill(KJob::EmitResult);
    });
}
void IntegrityCheckDialog::reject()
{
    if (m_job) { m_closeWhenStopped = true; requestCancel(); }
    else QDialog::reject();
}
void IntegrityCheckDialog::closeEvent(QCloseEvent *event)
{
    if (m_job) { event->ignore(); m_closeWhenStopped = true; requestCancel(); }
    else QDialog::closeEvent(event);
}
}
