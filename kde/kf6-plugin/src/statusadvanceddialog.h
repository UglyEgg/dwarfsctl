// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dwarfsctljson.h"
#include "dwarfsctlmodel.h"

#include <KPageDialog>

class QListWidget;
class QPlainTextEdit;
class QStackedWidget;

namespace dwarfsplugin {

class StatusAdvancedDialog : public KPageDialog {
    Q_OBJECT
public:
    explicit StatusAdvancedDialog(const Envelope &env, const StatusDetails &details, QWidget *parent = nullptr);

private:
    QPlainTextEdit *m_jsonEdit = nullptr;

    static QString prettyJsonForEnvelope(const Envelope &env);
};

} // namespace dwarfsplugin
