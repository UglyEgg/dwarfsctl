// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include <memory>
#include "dwarfsctljson.h"
class QLabel;
class QPushButton;
class QFrame;
class QGridLayout;
class QProgressBar;
class QTemporaryDir;
namespace dwarfsctl_kde { class OperationStatus; }
namespace dwarfsplugin {
class BuildFeedback final : public QDialog {
    Q_OBJECT
public:
    explicit BuildFeedback(const QString &output, QWidget *parent = nullptr, bool preview = false);
    ~BuildFeedback() override;
    QString previewStage() const;
    void phase(const QString &value);
    void complete(const Envelope &env);
    void published(const Envelope &env);
    void publishFailed(const QString &message, const Envelope &env = {});
    void failed(const QString &message = {}, const Envelope &env = {});
    void cancelled();
Q_SIGNALS:
    void cancelRequested();
    void createRequested(const QString &candidate, qint64 measuredBytes);
protected:
    void reject() override;
    void closeEvent(QCloseEvent *event) override;
private:
    void requestCancel(bool closeWhenStopped);
    void finish();
    void discardStage();
    bool m_cancelling = false;
    bool m_closeWhenStopped = false;
    bool m_preview = false;
    bool m_publishing = false;
    QString m_output;
    QString m_candidate;
    qint64 m_measuredBytes = 0;
    std::unique_ptr<QTemporaryDir> m_stage;
    int m_stageFd = -1;
    Envelope m_envelope;
    dwarfsctl_kde::OperationStatus *m_status;
    QLabel *m_summary;
    QFrame *m_metrics;
    QLabel *m_originalValue;
    QLabel *m_imageValue;
    QLabel *m_savedValue;
    QProgressBar *m_savingsBar;
    QWidget *m_typeSection;
    QGridLayout *m_typeGrid;
    QPushButton *m_close;
    QPushButton *m_cancel;
    QPushButton *m_reveal;
    QPushButton *m_details;
    QPushButton *m_publish;
};
}
