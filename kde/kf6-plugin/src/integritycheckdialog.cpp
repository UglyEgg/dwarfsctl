// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "integritycheckdialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <KJobWidgets>
#include <KLocalizedString>

#include "bannerwidget.h"
#include "detailsdialog.h"
#include "dwarfsctljob.h"
#include "envelopetext.h"

namespace dwarfsplugin {

IntegrityCheckDialog::IntegrityCheckDialog(const QString &imagePath, QWidget *parent)
    : QDialog(parent), m_imagePath(imagePath)
{
    setWindowTitle(i18n("Integrity Check"));
    setModal(true);
    resize(560, 240);

    auto *root = new QVBoxLayout(this);

    m_banner = new BannerWidget(this);
    m_banner->setVisible(false);
    root->addWidget(m_banner);

    auto *intro = new QLabel(
        i18n("Runs the official DwarFS filesystem integrity check. This may take a while for large images."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    m_summary = new QLabel(i18n("Not started."), this);
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_summary);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_runBtn = buttons->addButton(i18n("Run"), QDialogButtonBox::ActionRole);
    m_detailsBtn = buttons->addButton(i18n("Details…"), QDialogButtonBox::ActionRole);
    m_detailsBtn->setEnabled(false);

    connect(m_runBtn, &QPushButton::clicked, this, [this]() { start(); });
    connect(m_detailsBtn, &QPushButton::clicked, this, [this]() {
        if (!m_hasEnv) {
            return;
        }
        DetailsDialog dlg(m_lastEnv, i18n("Integrity Check Details"), this);
        dlg.exec();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

void IntegrityCheckDialog::start()
{
    m_runBtn->setEnabled(false);
    m_detailsBtn->setEnabled(false);
    m_summary->setText(i18n("Running…"));
    if (m_banner) {
        m_banner->setVisible(false);
    }

    QStringList args;
    // dwarfsctl uses a global flag; pass it first to avoid any clap parsing ambiguity.
    args << QStringLiteral("fsck") << m_imagePath << QStringLiteral("--arg")
         << QStringLiteral("--check-integrity");

    auto *job = new DwarfsctlJob(DwarfsctlJob::Request{i18n("Integrity Check"), args, 0}, this);
    KJobWidgets::setWindow(job, this);

    connect(job, &KJob::result, this, [this, job](KJob *j) {
        Q_UNUSED(j);
        const Envelope env = job->envelope();
        if (job->error() != 0) {
            const QString msg = !env.error.message.isEmpty() ? env.error.message : job->errorText();
            criticalModalWithDetails(this,
                                     i18n("Integrity Check"),
                                     msg.isEmpty() ? i18n("Operation failed") : msg,
                                     env);
            const QString firstLine = firstLineFromToolPayload(env);
            if (!firstLine.isEmpty()) {
                m_summary->setText(firstLine);
            } else {
                m_summary->setText(msg.isEmpty() ? i18n("Integrity check failed.") : msg);
            }
            m_runBtn->setEnabled(true);
            m_lastEnv = env;
            m_hasEnv = true;
            m_detailsBtn->setEnabled(true);
            return;
        }
        onFinished(env);
    });

    job->start();
}

void IntegrityCheckDialog::onFinished(const Envelope &env)
{
    m_lastEnv = env;
    m_hasEnv = true;
    m_detailsBtn->setEnabled(true);
    m_runBtn->setEnabled(true);

    if (env.ok) {
        if (m_banner) {
            m_banner->setSeverity(BannerWidget::Severity::Info);
            m_banner->setTitleAndBody(i18n("Integrity check passed"), i18n("dwarfsck reported no integrity errors."));
            m_banner->setDetails({});
            m_banner->setVisible(true);
        }
        m_summary->setText(i18n("Integrity check passed."));
    } else {
        const QString msg = !env.error.message.isEmpty() ? env.error.message : i18n("Integrity check failed.");
        const QString firstLine = firstLineFromToolPayload(env);
        m_summary->setText(firstLine.isEmpty() ? msg : firstLine);
    }
}

} // namespace dwarfsplugin
