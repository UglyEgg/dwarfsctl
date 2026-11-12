// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

#include "dwarfsctljson.h"

class QLabel;
class QPushButton;

namespace dwarfsplugin {

class BannerWidget;

// Dedicated UI for vanilla DwarFS integrity checking.
// Runs: dwarfsctl fsck <image> --arg --check-integrity
// The underlying dwarfsck output is surfaced via the Envelope tool payload.
class IntegrityCheckDialog final : public QDialog {
    Q_OBJECT
public:
    explicit IntegrityCheckDialog(const QString &imagePath, QWidget *parent = nullptr);

private:
    void start();
    void onFinished(const Envelope &env);

    QString m_imagePath;
    Envelope m_lastEnv;
    bool m_hasEnv{false};

    QLabel *m_summary{nullptr};
    BannerWidget *m_banner{nullptr};
    QPushButton *m_runBtn{nullptr};
    QPushButton *m_detailsBtn{nullptr};
};

} // namespace dwarfsplugin
