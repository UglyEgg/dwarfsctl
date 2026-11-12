// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "checksumdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <KJobWidgets>
#include <KLocalizedString>

#include "bannerwidget.h"
#include "detailsdialog.h"
#include "dwarfsctljob.h"
#include "envelopetext.h"

namespace dwarfsplugin {

static QPushButton *addBrowseButton(QWidget *parent, QHBoxLayout *row, QLineEdit *target, const QString &title, bool directories)
{
    auto *btn = new QPushButton(i18n("Browse…"), parent);
    row->addWidget(btn);
    QObject::connect(btn, &QPushButton::clicked, parent, [parent, target, title, directories]() {
        const QString picked = directories ? QFileDialog::getExistingDirectory(parent, title)
                                           : QFileDialog::getSaveFileName(parent, title);
        if (!picked.isEmpty()) {
            target->setText(picked);
        }
    });
    return btn;
}

static QPushButton *addOpenFileButton(QWidget *parent, QHBoxLayout *row, QLineEdit *target, const QString &title)
{
    auto *btn = new QPushButton(i18n("Browse…"), parent);
    row->addWidget(btn);
    QObject::connect(btn, &QPushButton::clicked, parent, [parent, target, title]() {
        const QString picked = QFileDialog::getOpenFileName(parent, title);
        if (!picked.isEmpty()) {
            target->setText(picked);
        }
    });
    return btn;
}

ChecksumDialog::ChecksumDialog(const QString &imagePath, QWidget *parent)
    : QDialog(parent), m_imagePath(imagePath)
{
    setWindowTitle(i18n("Checksums"));
    setModal(true);
    resize(640, 340);

    auto *root = new QVBoxLayout(this);

    m_banner = new BannerWidget(this);
    m_banner->setVisible(false);
    root->addWidget(m_banner);

    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    root->addLayout(form);

    m_algo = new QComboBox(this);
    // Keep the list conservative; this maps cleanly to standard *sum tools for verify.
    m_algo->addItem(QStringLiteral("sha256"));
    m_algo->addItem(QStringLiteral("sha512"));
    m_algo->addItem(QStringLiteral("sha512-256"));
    form->addRow(i18n("Algorithm"), m_algo);

    QObject::connect(m_algo, &QComboBox::currentTextChanged, this, [this]() {
        updateDefaultExportPath();
        updateVerifySupport();
    });

    // Export
    {
        auto *row = new QHBoxLayout();
        m_exportPath = new QLineEdit(this);
        m_exportPath->setPlaceholderText(i18n("Choose an output file…"));
        QObject::connect(m_exportPath, &QLineEdit::textEdited, this, [this](const QString &) {
            m_exportPathTouched = true;
        });
        row->addWidget(m_exportPath);
        addBrowseButton(this, row, m_exportPath, i18n("Export checksums to…"), false);
        form->addRow(i18n("Export"), row);
    }

    // Verify input
    {
        auto *row = new QHBoxLayout();
        m_verifyInput = new QLineEdit(this);
        m_verifyInput->setPlaceholderText(i18n("Choose a checksum file…"));
        row->addWidget(m_verifyInput);
        addOpenFileButton(this, row, m_verifyInput, i18n("Select checksum file…"));
        form->addRow(i18n("Verify file"), row);
    }

    // Verify base directory (optional)
    {
        auto *row = new QHBoxLayout();
        m_verifyDir = new QLineEdit(this);
        m_verifyDir->setPlaceholderText(i18n("Optional (defaults to checksum file directory)…"));
        row->addWidget(m_verifyDir);
        addBrowseButton(this, row, m_verifyDir, i18n("Select base directory…"), true);
        form->addRow(i18n("Verify in"), row);
    }

    m_summary = new QLabel(i18n("Choose an action."), this);
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_summary);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_exportBtn = buttons->addButton(i18n("Export"), QDialogButtonBox::ActionRole);
    m_verifyBtn = buttons->addButton(i18n("Verify"), QDialogButtonBox::ActionRole);
    m_detailsBtn = buttons->addButton(i18n("Details…"), QDialogButtonBox::ActionRole);
    m_detailsBtn->setEnabled(false);

    connect(m_exportBtn, &QPushButton::clicked, this, [this]() { startExport(); });
    connect(m_verifyBtn, &QPushButton::clicked, this, [this]() { startVerify(); });
    connect(m_detailsBtn, &QPushButton::clicked, this, [this]() {
        if (!m_hasEnv) {
            return;
        }
        DetailsDialog dlg(m_lastEnv, i18n("Checksum Details"), this);
        dlg.exec();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    // Initialize a helpful default export filename next to the image.
    updateDefaultExportPath();
    updateVerifySupport();
}

void ChecksumDialog::updateDefaultExportPath()
{
    if (!m_exportPath || m_exportPathTouched) {
        return;
    }
    // Default to writing a checksum list alongside the image:
    //   <image>.<algo>.checksums
    const QString a = algo();
    if (a.isEmpty()) {
        return;
    }
    m_exportPath->setText(m_imagePath + QStringLiteral(".") + a + QStringLiteral(".checksums"));
}

void ChecksumDialog::updateVerifySupport()
{
    // Verify support is limited to algorithms with a widely available *sum --check tool.
    // dwarfsctl also enforces this; we reflect it here to avoid launching a job that will
    // inevitably error.
    const QString a = algo();
    const bool canVerify = !(a.compare(QStringLiteral("sha512-256"), Qt::CaseInsensitive) == 0);
    if (m_verifyBtn) {
        m_verifyBtn->setEnabled(canVerify && (m_exportBtn ? m_exportBtn->isEnabled() : true));
    }
    if (!canVerify) {
        // Do not overwrite other status messages mid-run.
        if (!m_exportBtn || m_exportBtn->isEnabled()) {
            m_summary->setText(i18n("Verify is not supported for %1 (no standard *sum --check tool).", a));
        }
    } else {
        if (!m_exportBtn || m_exportBtn->isEnabled()) {
            // Keep the summary neutral unless something else sets it.
            if (m_summary && m_summary->text().startsWith(i18n("Verify is not supported"))) {
                m_summary->setText(i18n("Choose an action."));
            }
        }
    }
}

QString ChecksumDialog::algo() const
{
    return m_algo->currentText().trimmed();
}

void ChecksumDialog::setBusy(bool busy)
{
    m_exportBtn->setEnabled(!busy);
    // Verify enablement is also gated by the chosen algorithm.
    if (m_verifyBtn) {
        m_verifyBtn->setEnabled(!busy);
    }
    if (!busy) {
        updateVerifySupport();
    }
}

void ChecksumDialog::startExport()
{
    const QString out = m_exportPath->text().trimmed();
    if (out.isEmpty()) {
        m_summary->setText(i18n("Export output path is required."));
        return;
    }
    setBusy(true);
    m_detailsBtn->setEnabled(false);
    m_summary->setText(i18n("Exporting…"));
    if (m_banner) {
        m_banner->setVisible(false);
    }

    QStringList args;
    // dwarfsctl uses a global flag; pass it first to avoid any clap parsing ambiguity.
    args << QStringLiteral("checksum") << QStringLiteral("export") << m_imagePath
         << QStringLiteral("--algo") << algo() << QStringLiteral("--output") << out;

    auto *job = new DwarfsctlJob(DwarfsctlJob::Request{i18n("Export checksums"), args, 0}, this);
    KJobWidgets::setWindow(job, this);
    connect(job, &KJob::result, this, [this, job](KJob *j) {
        Q_UNUSED(j);
        const Envelope env = job->envelope();
        if (job->error() != 0) {
            const QString msg = !env.error.message.isEmpty() ? env.error.message : job->errorText();
            criticalModalWithDetails(this,
                                     i18n("Export checksums"),
                                     msg.isEmpty() ? i18n("Operation failed") : msg,
                                     env);
            const QString firstLine = firstLineFromToolPayload(env);
            if (!firstLine.isEmpty()) {
                m_summary->setText(firstLine);
            } else {
                m_summary->setText(msg.isEmpty() ? i18n("Export failed.") : msg);
            }
            m_lastEnv = env;
            m_hasEnv = true;
            m_detailsBtn->setEnabled(true);
            setBusy(false);
            return;
        }
        onFinished(env);
    });
    job->start();
}

void ChecksumDialog::startVerify()
{
    const QString input = m_verifyInput->text().trimmed();
    if (input.isEmpty()) {
        m_summary->setText(i18n("Checksum file is required for verification."));
        return;
    }

    setBusy(true);
    m_detailsBtn->setEnabled(false);
    m_summary->setText(i18n("Verifying…"));
    if (m_banner) {
        m_banner->setVisible(false);
    }


    QStringList args;
    // dwarfsctl uses a global flag; pass it first to avoid any clap parsing ambiguity.
    args << QStringLiteral("checksum") << QStringLiteral("verify") << m_imagePath
         << QStringLiteral("--algo") << algo() << QStringLiteral("--input") << input;
    const QString dir = m_verifyDir->text().trimmed();
    if (!dir.isEmpty()) {
        args << QStringLiteral("--dir") << dir;
    }


    auto *job = new DwarfsctlJob(DwarfsctlJob::Request{i18n("Verify checksums"), args, 0}, this);
    KJobWidgets::setWindow(job, this);
    connect(job, &KJob::result, this, [this, job](KJob *j) {
        Q_UNUSED(j);
        const Envelope env = job->envelope();
        if (job->error() != 0) {
            const QString msg = !env.error.message.isEmpty() ? env.error.message : job->errorText();
            criticalModalWithDetails(this,
                                     i18n("Verify checksums"),
                                     msg.isEmpty() ? i18n("Operation failed") : msg,
                                     env);
            const QString firstLine = firstLineFromToolPayload(env);
            if (!firstLine.isEmpty()) {
                m_summary->setText(firstLine);
            } else {
                m_summary->setText(msg.isEmpty() ? i18n("Verification failed.") : msg);
            }
            m_lastEnv = env;
            m_hasEnv = true;
            m_detailsBtn->setEnabled(true);
            setBusy(false);
            return;
        }
        onFinished(env);
    });
    job->start();
}

void ChecksumDialog::onFinished(const Envelope &env)
{
    m_lastEnv = env;
    m_hasEnv = true;
    m_detailsBtn->setEnabled(true);
    setBusy(false);

    if (env.ok) {
        if (env.command == QStringLiteral("checksum_export")) {
            m_summary->setText(i18n("Checksums exported."));
            if (m_banner) {
                m_banner->setSeverity(BannerWidget::Severity::Info);
                m_banner->setTitleAndBody(i18n("Export completed"), i18n("Checksum list written successfully."));
                m_banner->setDetails({});
                m_banner->setVisible(true);
            }
        } else if (env.command == QStringLiteral("checksum_verify")) {
            m_summary->setText(i18n("Verification succeeded."));
            if (m_banner) {
                m_banner->setSeverity(BannerWidget::Severity::Info);
                m_banner->setTitleAndBody(i18n("Verification succeeded"), i18n("All checksums matched."));
                m_banner->setDetails({});
                m_banner->setVisible(true);
            }
        } else {
            m_summary->setText(i18n("Completed."));
        }
    } else {
        const QString msg = !env.error.message.isEmpty() ? env.error.message : i18n("Operation failed.");
        const QString firstLine = firstLineFromToolPayload(env);
        m_summary->setText(firstLine.isEmpty() ? msg : firstLine);
    }
}

} // namespace dwarfsplugin
