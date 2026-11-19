// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QElapsedTimer>
#include <QWidget>
class QLabel;
class QProgressBar;
class QTimer;
namespace dwarfsctl_kde {
// Shared presentation only: the owning job controls when cleanup is complete.
class OperationStatus final : public QWidget {
    Q_OBJECT
public:
    enum class State { Idle, Running, Cancelling, Succeeded, Failed, Cancelled };
    explicit OperationStatus(QWidget *parent = nullptr,
        const QString &textObjectName = QStringLiteral("operationStatusText"));
    void begin(const QString &text);
    void phase(const QString &text);
    void cancelling();
    void succeed(const QString &text);
    void fail(const QString &text);
    void cancelled(const QString &text = {});
    bool isActive() const;
    State state() const { return m_state; }
    void setCompactCompletion(bool compact) { m_compactCompletion = compact; }
private:
    void finish(State state, const QString &text);
    void updateElapsed();
    State m_state = State::Idle;
    bool m_compactCompletion = false;
    QLabel *m_text;
    QLabel *m_elapsed;
    QProgressBar *m_progress;
    QTimer *m_timer;
    QElapsedTimer m_clock;
};
}
