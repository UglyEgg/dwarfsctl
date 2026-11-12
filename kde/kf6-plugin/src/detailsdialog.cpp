// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "detailsdialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <KLocalizedString>

namespace dwarfsplugin {

static QPlainTextEdit *makeReadOnlyText(const QString &text, QWidget *parent)
{
    auto *edit = new QPlainTextEdit(parent);
    edit->setReadOnly(true);
    // Wrap long lines to the dialog width so users don't lose content off-screen.
    edit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    edit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QFont f = edit->font();
    f.setFamily(QStringLiteral("monospace"));
    edit->setFont(f);
    edit->setPlainText(text);
    return edit;
}

static QString jsonPretty(const QJsonValue &v)
{
    if (v.isUndefined() || v.isNull()) {
        return QString();
    }
    if (v.isObject()) {
        return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented));
    }
    if (v.isArray()) {
        return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Indented));
    }
    if (v.isString()) {
        return v.toString();
    }
    // For numbers/bools, render as compact JSON.
    QJsonArray a;
    a.append(v);
    const auto b = QJsonDocument(a).toJson(QJsonDocument::Compact);
    // Strip [ ] wrapper.
    QString s = QString::fromUtf8(b);
    if (s.startsWith(QLatin1Char('[')) && s.endsWith(QLatin1Char(']'))) {
        s = s.mid(1, s.size() - 2);
    }
    return s;
}

static QJsonObject toolPayloadsObject(const Envelope &env)
{
    // Preferred: details.tools (object).
    const QJsonValue toolsV = env.details.value(QStringLiteral("tools"));
    if (toolsV.isObject()) {
        return toolsV.toObject();
    }
    // Fallback: details.tool (single payload).
    const QJsonValue toolV = env.details.value(QStringLiteral("tool"));
    if (toolV.isObject()) {
        QJsonObject o;
        o.insert(QStringLiteral("tool"), toolV.toObject());
        return o;
    }
    return {};
}

DetailsDialog::DetailsDialog(const Envelope &env, const QString &title, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(title);
    setModal(true);
    resize(860, 560);

    auto *root = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    root->addWidget(tabs, 1);

    // Summary tab
    {
        auto *w = new QWidget(this);
        auto *form = new QFormLayout(w);
        form->addRow(i18n("Command"), new QLabel(env.command.isEmpty() ? i18n("(unknown)") : env.command, w));
        form->addRow(i18n("OK"), new QLabel(env.ok ? i18n("yes") : i18n("no"), w));
        form->addRow(i18n("Image"), new QLabel(env.image.isEmpty() ? i18n("(none)") : env.image, w));

        QString err;
        if (env.has_error) {
            if (!env.error.kind.isEmpty() && !env.error.message.isEmpty()) {
                err = env.error.kind + QStringLiteral(": ") + env.error.message;
            } else if (!env.error.message.isEmpty()) {
                err = env.error.message;
            } else if (!env.error.kind.isEmpty()) {
                err = env.error.kind;
            } else {
                err = i18n("code: %1", env.error.code);
            }
        } else {
            err = i18n("(none)");
        }
        form->addRow(i18n("Error"), new QLabel(err, w));
        form->addRow(i18n("Warnings"), new QLabel(i18n("%1", env.warnings.size() + env.warning_objects.size()), w));
        tabs->addTab(w, i18n("Summary"));
    }

    // Warnings tab
    {
        auto *w = new QWidget(this);
        auto *layout = new QVBoxLayout(w);

        if (env.warnings.isEmpty() && env.warning_objects.isEmpty()) {
            layout->addWidget(new QLabel(i18n("(none)"), w));
        } else {
            if (!env.warnings.isEmpty()) {
                QStringList lines;
                for (const auto &s : env.warnings) {
                    lines << s;
                }
                layout->addWidget(makeReadOnlyText(lines.join(QStringLiteral("\n")), w));
            }
            if (!env.warning_objects.isEmpty()) {
                auto *table = new QTableWidget(w);
                table->setColumnCount(2);
                table->setHorizontalHeaderLabels({i18n("Code"), i18n("Message")});
                table->horizontalHeader()->setStretchLastSection(true);
                table->verticalHeader()->setVisible(false);
                table->setEditTriggers(QAbstractItemView::NoEditTriggers);
                table->setSelectionBehavior(QAbstractItemView::SelectRows);
                table->setRowCount(env.warning_objects.size());
                int r = 0;
                for (const auto &wo : env.warning_objects) {
                    table->setItem(r, 0, new QTableWidgetItem(wo.code));
                    table->setItem(r, 1, new QTableWidgetItem(wo.message));
                    r++;
                }
                layout->addWidget(table, 1);
            }
        }
        tabs->addTab(w, i18n("Warnings"));
    }

    // Error tab
    {
        auto *w = new QWidget(this);
        auto *form = new QFormLayout(w);
        if (!env.has_error) {
            form->addRow(new QLabel(i18n("(none)"), w));
        } else {
            form->addRow(i18n("Code"), new QLabel(QString::number(env.error.code), w));
            form->addRow(i18n("Kind"), new QLabel(env.error.kind.isEmpty() ? i18n("(none)") : env.error.kind, w));
            form->addRow(i18n("Message"), new QLabel(env.error.message.isEmpty() ? i18n("(none)") : env.error.message, w));
        }
        tabs->addTab(w, i18n("Error"));
    }

    // Tools tab
    {
        auto *w = new QWidget(this);
        auto *layout = new QVBoxLayout(w);
        const QJsonObject tools = toolPayloadsObject(env);
        if (tools.isEmpty()) {
            layout->addWidget(new QLabel(i18n("(none)"), w));
        } else {
            auto *toolTabs = new QTabWidget(w);
            for (auto it = tools.begin(); it != tools.end(); ++it) {
                const QString toolName = it.key();
                auto *tw = new QWidget(toolTabs);
                auto *tl = new QVBoxLayout(tw);
                const QJsonObject payload = it.value().toObject();

                const QString argv = jsonPretty(payload.value(QStringLiteral("argv")));
                const QString stdoutText = jsonPretty(payload.value(QStringLiteral("stdout")));
                const QString stderrText = jsonPretty(payload.value(QStringLiteral("stderr")));
                const QString jsonText = jsonPretty(payload.value(QStringLiteral("json")));

                if (!argv.isEmpty()) {
                    tl->addWidget(new QLabel(i18n("argv:"), tw));
                    tl->addWidget(makeReadOnlyText(argv, tw));
                }
                if (!jsonText.isEmpty()) {
                    tl->addWidget(new QLabel(i18n("json:"), tw));
                    tl->addWidget(makeReadOnlyText(jsonText, tw), 1);
                }
                if (!stdoutText.isEmpty()) {
                    tl->addWidget(new QLabel(i18n("stdout:"), tw));
                    tl->addWidget(makeReadOnlyText(stdoutText, tw), 1);
                }
                if (!stderrText.isEmpty()) {
                    tl->addWidget(new QLabel(i18n("stderr:"), tw));
                    tl->addWidget(makeReadOnlyText(stderrText, tw), 1);
                }

                toolTabs->addTab(tw, toolName);
            }
            layout->addWidget(toolTabs, 1);
        }
        tabs->addTab(w, i18n("Tools"));
    }

    // Raw JSON tab
    {
        auto *w = new QWidget(this);
        auto *layout = new QVBoxLayout(w);
        QJsonObject rootObj;
        rootObj.insert(QStringLiteral("ok"), env.ok);
        rootObj.insert(QStringLiteral("command"), env.command);
        rootObj.insert(QStringLiteral("image"), env.image);
        rootObj.insert(QStringLiteral("details"), env.details);
        QJsonArray wa;
        for (const auto &wo : env.warning_objects) {
            QJsonObject o;
            o.insert(QStringLiteral("code"), wo.code);
            o.insert(QStringLiteral("message"), wo.message);
            wa.append(o);
        }
        rootObj.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(env.warnings));
        rootObj.insert(QStringLiteral("warning_objects"), wa);
        if (env.has_error) {
            QJsonObject e;
            e.insert(QStringLiteral("code"), env.error.code);
            e.insert(QStringLiteral("kind"), env.error.kind);
            e.insert(QStringLiteral("message"), env.error.message);
                rootObj.insert(QStringLiteral("error"), e);
        } else {
            rootObj.insert(QStringLiteral("error"), QJsonValue::Null);
        }
        const auto text = QString::fromUtf8(QJsonDocument(rootObj).toJson(QJsonDocument::Indented));
        layout->addWidget(makeReadOnlyText(text, w), 1);
        tabs->addTab(w, i18n("Raw"));
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

bool criticalModalWithDetails(QWidget *parent, const QString &title, const QString &message, const Envelope &env)
{
    QMessageBox box(QMessageBox::Critical, title, message, QMessageBox::Ok, parent);
    auto *detailsBtn = box.addButton(i18n("Show details"), QMessageBox::ActionRole);
    box.exec();
    if (box.clickedButton() == detailsBtn) {
        DetailsDialog dlg(env, title + QStringLiteral(": ") + i18n("Details"), parent);
        dlg.exec();
        return true;
    }
    return false;
}

} // namespace dwarfsplugin
