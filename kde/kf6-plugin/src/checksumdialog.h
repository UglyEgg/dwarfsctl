// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

#include "dwarfsctljson.h"

class QComboBox;
class QLineEdit;
class QPushButton;
class QLabel;

namespace dwarfsplugin {

class BannerWidget;

// UI for vanilla-style checksum export and optional verification.
// Export wraps: dwarfsctl checksum export <image> --algo <algo> --output <file>
// Verify wraps: dwarfsctl checksum verify <image> --algo <algo> --input <file> [--dir <dir>]
class ChecksumDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ChecksumDialog(const QString &imagePath, QWidget *parent = nullptr);

private:
    QString algo() const;

    void updateVerifySupport();

    void updateDefaultExportPath();

    void startExport();
    void startVerify();
    void onFinished(const Envelope &env);

    void setBusy(bool busy);

    QString m_imagePath;

    Envelope m_lastEnv;
    bool m_hasEnv{false};

    QComboBox *m_algo{nullptr};
    QLineEdit *m_exportPath{nullptr};
    QLineEdit *m_verifyInput{nullptr};
    QLineEdit *m_verifyDir{nullptr};

    QLabel *m_summary{nullptr};
    BannerWidget *m_banner{nullptr};
    QPushButton *m_exportBtn{nullptr};
    QPushButton *m_verifyBtn{nullptr};
    QPushButton *m_detailsBtn{nullptr};

    bool m_exportPathTouched{false};
};

} // namespace dwarfsplugin
