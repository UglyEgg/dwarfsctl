// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QPointer>

#include "dwarfsctljson.h"

class QComboBox;
class QLineEdit;
class QPushButton;
class QCloseEvent;
namespace dwarfsctl_kde { class OperationStatus; }

namespace dwarfsplugin {

class DwarfsctlJob;

// UI for vanilla-style checksum export and optional verification.
// Export wraps: dwarfsctl checksum export <image> --algo <algo> --output <file>
// Verify wraps: dwarfsctl checksum verify <image> --algo <algo> --input <file> [--dir <dir>]
class ChecksumDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ChecksumDialog(const QString &imagePath, QWidget *parent = nullptr);

public Q_SLOTS:
    void reject() override;
Q_SIGNALS:
    void operationCancelRequested();
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    QString algo() const;

    void updateVerifySupport();

    void updateDefaultExportPath();

    void startExport();
    void startVerify();
    void startJob(const QStringList &args, const QString &running, const QString &success);
    void requestCancel();

    void setBusy(bool busy);

    QString m_imagePath;

    Envelope m_lastEnv;
    bool m_hasEnv{false};

    QComboBox *m_algo{nullptr};
    QLineEdit *m_exportPath{nullptr};
    QLineEdit *m_verifyInput{nullptr};
    QLineEdit *m_verifyDir{nullptr};

    dwarfsctl_kde::OperationStatus *m_status{nullptr};
    QPointer<DwarfsctlJob> m_job;
    bool m_cancelling{false};
    bool m_closeWhenStopped = false;
    QPushButton *m_cancelBtn{nullptr};
    QPushButton *m_exportBrowse{nullptr};
    QPushButton *m_inputBrowse{nullptr};
    QPushButton *m_dirBrowse{nullptr};
    QPushButton *m_exportBtn{nullptr};
    QPushButton *m_verifyBtn{nullptr};
    QPushButton *m_detailsBtn{nullptr};

    bool m_exportPathTouched{false};
};

} // namespace dwarfsplugin
