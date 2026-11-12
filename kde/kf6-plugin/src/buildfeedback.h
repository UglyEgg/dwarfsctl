// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include <QElapsedTimer>
#include "dwarfsctljson.h"
class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;
namespace dwarfsplugin {
class BuildFeedback final : public QDialog {
public:
    explicit BuildFeedback(const QString &output, QWidget *parent = nullptr);
    void phase(const QString &value);
    void complete(const Envelope &env);
    void failed();
protected:
    void reject() override;
    void closeEvent(QCloseEvent *event) override;
private:
    bool m_running = true;
    QString m_output;
    QLabel *m_status;
    QLabel *m_elapsed;
    QLabel *m_summary;
    QProgressBar *m_progress;
    QPushButton *m_close;
    QPushButton *m_reveal;
    QTimer *m_timer;
    QElapsedTimer m_clock;
};
}
