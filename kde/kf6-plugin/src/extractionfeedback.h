// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include "dwarfsctljson.h"
class QLabel;
class QPushButton;
namespace dwarfsctl_kde { class OperationStatus; }
class ExtractionFeedback final : public QDialog {
    Q_OBJECT
public:
    explicit ExtractionFeedback(const QString &destination, bool selected, QWidget *parent = nullptr);
    void complete(const dwarfsplugin::Envelope &env = {});
    void failed(const QString &message, const dwarfsplugin::Envelope &env = {});
    void cancelled();
Q_SIGNALS:
    void cancelRequested();
protected:
    void reject() override;
    void closeEvent(QCloseEvent *event) override;
private:
    void requestCancel(bool closeWhenStopped);
    void finish();
    QString m_destination;
    bool m_cancelling = false;
    bool m_closeWhenStopped = false;
    dwarfsplugin::Envelope m_envelope;
    dwarfsctl_kde::OperationStatus *m_status;
    QLabel *m_summary;
    QPushButton *m_cancel;
    QPushButton *m_close;
    QPushButton *m_open;
    QPushButton *m_details;
};
