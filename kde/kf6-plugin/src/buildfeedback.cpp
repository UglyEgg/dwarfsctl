// SPDX-License-Identifier: GPL-3.0-or-later
#include "buildfeedback.h"
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <KLocalizedString>
namespace dwarfsplugin {
BuildFeedback::BuildFeedback(const QString &output, QWidget *parent)
    : QDialog(parent), m_output(output)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(i18n("Create DwarFS image"));
    setMinimumWidth(480);
    auto *layout = new QVBoxLayout(this);
    auto *name = new QLabel(QFileInfo(output).fileName(), this);
    name->setTextFormat(Qt::PlainText);
    layout->addWidget(name);
    m_status = new QLabel(i18n("Preparing image…"), this);
    m_status->setWordWrap(true);
    m_status->setObjectName(QStringLiteral("buildStatus"));
    layout->addWidget(m_status);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    layout->addWidget(m_progress);
    m_elapsed = new QLabel(this);
    layout->addWidget(m_elapsed);
    m_summary = new QLabel(this);
    m_summary->setTextFormat(Qt::PlainText);
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_summary->setObjectName(QStringLiteral("buildSummary"));
    layout->addWidget(m_summary);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_close = buttons->button(QDialogButtonBox::Close);
    m_close->setEnabled(false);
    m_reveal = buttons->addButton(i18n("Open containing folder"), QDialogButtonBox::ActionRole);
    m_reveal->setEnabled(false);
    connect(m_reveal, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_output).absolutePath()));
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
    m_clock.start();
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, [this] {
        m_elapsed->setText(i18n("Elapsed: %1 seconds", m_clock.elapsed() / 1000));
    });
    m_timer->start(1000);
    show();
}
void BuildFeedback::phase(const QString &value)
{
    if (!m_running) return;
    if (value == QLatin1String("scanning")) m_status->setText(i18n("Scanning files…"));
    else if (value == QLatin1String("compressing")) m_status->setText(i18n("Compressing files…"));
    else if (value == QLatin1String("metadata")) m_status->setText(i18n("Writing image metadata…"));
    else if (value == QLatin1String("verifying")) m_status->setText(i18n("Checking the completed image…"));
}
void BuildFeedback::complete(const Envelope &env)
{
    m_running = false;
    m_timer->stop();
    m_progress->setRange(0, 100);
    m_progress->setValue(100);
    const auto summary = env.details.value(QStringLiteral("summary")).toObject();
    const bool created = !summary.isEmpty();
    m_status->setText(created ? i18n("Image created successfully") : i18n("Build preview complete — no image was created"));
    QFont statusFont = m_status->font();
    statusFont.setBold(true);
    m_status->setFont(statusFont);
    if (created) {
        const QLocale locale;
        const auto bytes = summary.value(QStringLiteral("image_bytes")).toInteger();
        const auto original = summary.value(QStringLiteral("original_bytes")).toInteger();
        QString text = i18n("%1 files\nFile contents: %2\nImage size: %3\nSaved to: %4",
            summary.value(QStringLiteral("files")).toInteger(), locale.formattedDataSize(original),
            locale.formattedDataSize(bytes), m_output);
        if (original > 0) text += i18n("\nImage is %1% of the file contents size", locale.toString(100.0 * bytes / original, 'f', 1));
        m_summary->setText(text);
        m_elapsed->setText(i18n("Completed in %1 seconds", locale.toString(summary.value(QStringLiteral("elapsed_ms")).toDouble() / 1000.0, 'f', 1)));
    }
    m_close->setEnabled(true);
    m_reveal->setEnabled(created);
    adjustSize();
    raise();
}
void BuildFeedback::failed()
{
    m_running = false;
    close();
}
void BuildFeedback::reject() { if (!m_running) QDialog::reject(); }
void BuildFeedback::closeEvent(QCloseEvent *event)
{
    if (m_running) event->ignore(); else QDialog::closeEvent(event);
}
}
