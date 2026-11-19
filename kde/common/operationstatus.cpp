// SPDX-License-Identifier: GPL-3.0-or-later
#include "operationstatus.h"
#include "uipresentation.h"
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QVBoxLayout>
#include <KLocalizedString>
namespace dwarfsctl_kde {
OperationStatus::OperationStatus(QWidget *parent, const QString &textObjectName) : QWidget(parent)
{
    setObjectName(QStringLiteral("operationStatus"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(layoutSpacing(this));
    m_text = new QLabel(this);
    m_text->setObjectName(textObjectName);
    m_text->setTextFormat(Qt::PlainText);
    m_text->setWordWrap(true);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_text);
    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("operationProgress"));
    m_progress->hide();
    layout->addWidget(m_progress);
    m_elapsed = new QLabel(this);
    m_elapsed->setObjectName(QStringLiteral("operationElapsed"));
    m_elapsed->setTextFormat(Qt::PlainText);
    m_elapsed->hide();
    layout->addWidget(m_elapsed);
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &OperationStatus::updateElapsed);
}
bool OperationStatus::isActive() const
{
    return m_state == State::Running || m_state == State::Cancelling;
}
void OperationStatus::begin(const QString &text)
{
    m_state = State::Running;
    m_text->setText(text);
    m_progress->setRange(0, 0);
    m_progress->show();
    m_clock.start();
    updateElapsed();
    m_elapsed->show();
    m_timer->start(1000);
}
void OperationStatus::phase(const QString &text)
{
    if (m_state == State::Running) m_text->setText(text);
}
void OperationStatus::cancelling()
{
    if (m_state != State::Running) return;
    m_state = State::Cancelling;
    m_text->setText(i18n("Cancelling…"));
}
void OperationStatus::finish(State state, const QString &text)
{
    if (!isActive()) return;
    m_state = state;
    m_timer->stop();
    m_text->setText(text);
    if (m_compactCompletion) {
        m_progress->hide();
        if (m_clock.elapsed() < 1000) m_elapsed->hide();
        else updateElapsed();
    } else {
        if (state == State::Succeeded) {
            m_progress->setRange(0, 100);
            m_progress->setValue(100);
        } else m_progress->hide();
        updateElapsed();
    }
    layout()->invalidate();
    layout()->activate();
    updateGeometry();
}
void OperationStatus::succeed(const QString &text) { finish(State::Succeeded, text); }
void OperationStatus::fail(const QString &text) { finish(State::Failed, text); }
void OperationStatus::cancelled(const QString &text)
{
    finish(State::Cancelled, text.isEmpty() ? i18n("Cancelled") : text);
}
void OperationStatus::updateElapsed()
{
    const auto seconds = m_clock.elapsed() / 1000;
    m_elapsed->setText(isActive()
        ? i18np("Elapsed: 1 second", "Elapsed: %1 seconds", seconds)
        : i18np("Finished after 1 second", "Finished after %1 seconds", seconds));
}
}
