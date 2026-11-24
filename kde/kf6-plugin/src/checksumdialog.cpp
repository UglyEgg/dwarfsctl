// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "checksumdialog.h"

#include <QComboBox>
#include <QCloseEvent>
#include <QTimer>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <KLocalizedString>

#include "operationstatus.h"
#include "detailsdialog.h"
#include "dwarfsctljob.h"

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


    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    root->addLayout(form);

    m_algo = new QComboBox(this);
    m_algo->setObjectName(QStringLiteral("checksumAlgorithm"));
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
        m_exportPath->setObjectName(QStringLiteral("checksumExportPath"));
        m_exportPath->setPlaceholderText(i18n("Choose an output file…"));
        QObject::connect(m_exportPath, &QLineEdit::textEdited, this, [this](const QString &) {
            m_exportPathTouched = true;
        });
        row->addWidget(m_exportPath);
        m_exportBrowse = addBrowseButton(this, row, m_exportPath, i18n("Export checksums to…"), false);
        m_exportBrowse->setObjectName(QStringLiteral("checksumExportBrowseButton"));
        form->addRow(i18n("Export"), row);
    }

    // Verify input
    {
        auto *row = new QHBoxLayout();
        m_verifyInput = new QLineEdit(this);
        m_verifyInput->setObjectName(QStringLiteral("checksumVerifyInput"));
        m_verifyInput->setPlaceholderText(i18n("Choose a checksum file…"));
        row->addWidget(m_verifyInput);
        m_inputBrowse = addOpenFileButton(this, row, m_verifyInput, i18n("Select checksum file…"));
        m_inputBrowse->setObjectName(QStringLiteral("checksumInputBrowseButton"));
        form->addRow(i18n("Verify file"), row);
    }

    // Verify base directory (optional)
    {
        auto *row = new QHBoxLayout();
        m_verifyDir = new QLineEdit(this);
        m_verifyDir->setObjectName(QStringLiteral("checksumVerifyDirectory"));
        m_verifyDir->setPlaceholderText(i18n("Optional (defaults to checksum file directory)…"));
        row->addWidget(m_verifyDir);
        m_dirBrowse = addBrowseButton(this, row, m_verifyDir, i18n("Select base directory…"), true);
        m_dirBrowse->setObjectName(QStringLiteral("checksumDirectoryBrowseButton"));
        form->addRow(i18n("Verify in"), row);
    }

    m_status = new dwarfsctl_kde::OperationStatus(this, QStringLiteral("checksumStatusText"));
    root->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("checksumCloseButton"));
    m_exportBtn = buttons->addButton(i18n("Export"), QDialogButtonBox::ActionRole);
    m_verifyBtn = buttons->addButton(i18n("Verify"), QDialogButtonBox::ActionRole);
    m_detailsBtn = buttons->addButton(i18n("Details…"), QDialogButtonBox::ActionRole);
    m_exportBtn->setObjectName(QStringLiteral("checksumExportButton"));
    m_verifyBtn->setObjectName(QStringLiteral("checksumVerifyButton"));
    m_detailsBtn->setObjectName(QStringLiteral("checksumDetailsButton"));
    m_detailsBtn->setEnabled(false);
    m_cancelBtn = buttons->addButton(i18n("Cancel"), QDialogButtonBox::ActionRole);
    m_cancelBtn->setObjectName(QStringLiteral("checksumCancelButton"));
    m_cancelBtn->hide();
    connect(m_cancelBtn, &QPushButton::clicked, this, &ChecksumDialog::requestCancel);

    connect(m_exportBtn, &QPushButton::clicked, this, [this]() { startExport(); });
    connect(m_verifyBtn, &QPushButton::clicked, this, [this]() { startVerify(); });
    connect(m_detailsBtn, &QPushButton::clicked, this, [this]() {
        if (!m_hasEnv || m_job) {
            return;
        }
        DetailsDialog dlg(m_lastEnv, i18n("Checksum Details"), this);
        dlg.exec();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &ChecksumDialog::reject);
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
    const bool supported = algo().compare(QStringLiteral("sha512-256"), Qt::CaseInsensitive) != 0;
    m_verifyBtn->setEnabled(!m_job && supported);
    m_verifyBtn->setToolTip(supported ? QString() : i18n("Verify is not supported for this algorithm (no standard *sum --check tool)."));
}

QString ChecksumDialog::algo() const
{
    return m_algo->currentText().trimmed();
}

void ChecksumDialog::setBusy(bool busy)
{
    for (auto *widget : {static_cast<QWidget *>(m_algo), static_cast<QWidget *>(m_exportPath),
             static_cast<QWidget *>(m_verifyInput), static_cast<QWidget *>(m_verifyDir),
             static_cast<QWidget *>(m_exportBrowse), static_cast<QWidget *>(m_inputBrowse), static_cast<QWidget *>(m_dirBrowse)})
        widget->setEnabled(!busy);
    m_exportBtn->setEnabled(!busy);
    m_verifyBtn->setEnabled(!busy && algo() != QLatin1String("sha512-256"));
    m_detailsBtn->setEnabled(!busy && m_hasEnv);
    m_cancelBtn->setVisible(busy);
    m_cancelBtn->setEnabled(busy);
}

void ChecksumDialog::startExport()
{
    if (m_job) return;
    const QString out = m_exportPath->text().trimmed();
    if (out.isEmpty()) {
        m_hasEnv = false;
        m_detailsBtn->setEnabled(false);
        m_status->begin(i18n("Export checksums"));
        m_status->fail(i18n("Export output path is required."));
        return;
    }
    startJob({QStringLiteral("checksum"), QStringLiteral("export"), m_imagePath,
        QStringLiteral("--algo"), algo(), QStringLiteral("--output"), out},
        i18n("Exporting checksums…"), i18n("Checksums exported."));
}

void ChecksumDialog::startVerify()
{
    if (m_job || algo() == QLatin1String("sha512-256")) return;
    const QString input = m_verifyInput->text().trimmed();
    if (input.isEmpty()) {
        m_hasEnv = false;
        m_detailsBtn->setEnabled(false);
        m_status->begin(i18n("Verify checksums"));
        m_status->fail(i18n("Checksum file is required for verification."));
        return;
    }
    QStringList args{QStringLiteral("checksum"), QStringLiteral("verify"), m_imagePath,
        QStringLiteral("--algo"), algo(), QStringLiteral("--input"), input};
    const QString dir = m_verifyDir->text().trimmed();
    if (!dir.isEmpty()) args << QStringLiteral("--dir") << dir;
    startJob(args, i18n("Verifying checksums…"), i18n("Verification succeeded."));
}

void ChecksumDialog::startJob(const QStringList &args, const QString &running, const QString &success)
{
    if (m_job) return;
    m_cancelling = false;
    m_closeWhenStopped = false;
    m_hasEnv = false;
    m_status->begin(running);
    auto *job = new DwarfsctlJob({i18n("Checksums"), args, 0, false}, this);
    m_job = job;
    setBusy(true);
    const QString failure = args.value(1) == QLatin1String("verify")
        ? i18n("The checksums could not be verified. Open Details for more information.")
        : i18n("The checksums could not be exported. Open Details for more information.");
    connect(job, &KJob::result, this, [this, job, success, failure] {
        if (m_job != job) return;
        m_job = nullptr;
        m_lastEnv = job->envelope();
        m_hasEnv = true;
        setBusy(false);
        if (job->error() == KJob::KilledJobError || m_lastEnv.details.value(QStringLiteral("cancelled")).toBool()) {
            m_status->cancelled();
        } else if (job->error() || !m_lastEnv.ok) {
            QString message = m_lastEnv.error.message;
            if (message.isEmpty()) message = job->errorString();
            if (m_lastEnv.error.message.isEmpty()) {
                m_lastEnv.error.message = message;
                m_lastEnv.has_error = true;
            }
            m_status->fail(failure);
        } else {
            m_status->succeed(success);
        }
        if (m_closeWhenStopped) QDialog::reject();
    });
    job->start();
}

void ChecksumDialog::requestCancel()
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

void ChecksumDialog::reject()
{
    if (m_job) { m_closeWhenStopped = true; requestCancel(); }
    else QDialog::reject();
}

void ChecksumDialog::closeEvent(QCloseEvent *event)
{
    if (m_job) { event->ignore(); m_closeWhenStopped = true; requestCancel(); }
    else QDialog::closeEvent(event);
}

} // namespace dwarfsplugin
