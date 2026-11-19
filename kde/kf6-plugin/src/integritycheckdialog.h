// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include <QPointer>
#include "dwarfsctljson.h"
class QCloseEvent;
class QLabel;
class QPushButton;
namespace dwarfsctl_kde { class OperationStatus; }
namespace dwarfsplugin {
class DwarfsctlJob;
class BannerWidget;
class IntegrityCheckDialog final : public QDialog {
    Q_OBJECT
public:
    explicit IntegrityCheckDialog(const QString &imagePath, QWidget *parent = nullptr);
public Q_SLOTS:
    void reject() override;
Q_SIGNALS:
    void operationCancelRequested();
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void start();
    void requestCancel();
    QString m_imagePath;
    Envelope m_lastEnv;
    bool m_hasEnv = false;
    bool m_cancelling = false;
    bool m_closeWhenStopped = false;
    QPointer<DwarfsctlJob> m_job;
    dwarfsctl_kde::OperationStatus *m_status;
    QLabel *m_resultDetail;
    BannerWidget *m_warnings;
    QPushButton *m_runBtn;
    QPushButton *m_cancelBtn;
    QPushButton *m_detailsBtn;
};
}
