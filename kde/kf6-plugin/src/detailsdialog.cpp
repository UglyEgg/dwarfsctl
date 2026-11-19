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
#include <QScrollArea>
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
        if (v.toObject().isEmpty()) return {};
        return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented));
    }
    if (v.isArray()) {
        if (v.toArray().isEmpty()) return {};
        return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Indented));
    }
    if (v.isString()) {
        return v.toString().trimmed().isEmpty() ? QString() : v.toString();
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

static void addSummaryField(QVBoxLayout *layout, QWidget *parent,
                            const QString &caption, const QString &value)
{
    auto *field = new QWidget(parent);
    auto *fieldLayout = new QVBoxLayout(field);
    fieldLayout->setContentsMargins(0, 0, 0, 0);
    fieldLayout->setSpacing(2);
    auto *title = new QLabel(caption, field);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    fieldLayout->addWidget(title);
    auto *description = new QLabel(value, field);
    description->setTextFormat(Qt::PlainText);
    description->setWordWrap(true);
    description->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fieldLayout->addWidget(description);
    layout->addWidget(field);
}

DetailsDialog::DetailsDialog(const Envelope &env, const QString &title, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(title);
    setModal(true);
    resize(680, 340);

    QStringList warningLines;
    for (const auto &warning : env.warnings) {
        if (!warning.trimmed().isEmpty()) warningLines.append(warning);
    }
    QVector<WarningInfo> warningObjects;
    for (const auto &warning : env.warning_objects) {
        if (!warning.code.trimmed().isEmpty() || !warning.message.trimmed().isEmpty())
            warningObjects.append(warning);
    }
    const int warningCount = warningLines.size() + warningObjects.size();

    auto *root = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    root->addWidget(tabs, 1);

    // Summary tab
    {
        auto *scroll = new QScrollArea(tabs);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        auto *w = new QWidget(scroll);
        auto *summary = new QVBoxLayout(w);
        summary->setSpacing(12);
        addSummaryField(summary, w, i18n("Result"), env.ok ? i18n("Passed") : i18n("Failed"));
        if (!env.command.isEmpty()) addSummaryField(summary, w, i18n("Command"), env.command);
        if (!env.image.isEmpty()) addSummaryField(summary, w, i18n("Image"), env.image);

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
        }
        if (env.has_error) addSummaryField(summary, w, i18n("Error"), err);
        if (warningCount > 0)
            addSummaryField(summary, w, i18n("Warnings"), QString::number(warningCount));
        summary->addStretch(1);
        scroll->setWidget(w);
        tabs->addTab(scroll, i18n("Summary"));
    }

    // Warnings tab
    if (warningCount > 0) {
        auto *w = new QWidget(this);
        auto *layout = new QVBoxLayout(w);
        if (!warningLines.isEmpty())
            layout->addWidget(makeReadOnlyText(warningLines.join(QStringLiteral("\n")), w));
        if (!warningObjects.isEmpty()) {
            auto *table = new QTableWidget(w);
            table->setColumnCount(2);
            table->setHorizontalHeaderLabels({i18n("Code"), i18n("Message")});
            table->horizontalHeader()->setStretchLastSection(true);
            table->verticalHeader()->setVisible(false);
            table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            table->setSelectionBehavior(QAbstractItemView::SelectRows);
            table->setRowCount(warningObjects.size());
            int r = 0;
            for (const auto &wo : warningObjects) {
                table->setItem(r, 0, new QTableWidgetItem(wo.code));
                table->setItem(r, 1, new QTableWidgetItem(wo.message));
                r++;
            }
            layout->addWidget(table, 1);
        }
        tabs->addTab(w, i18n("Warnings"));
    }

    // Error tab
    if (env.has_error) {
        auto *w = new QWidget(this);
        auto *form = new QFormLayout(w);
        form->addRow(i18n("Code"), new QLabel(QString::number(env.error.code), w));
        if (!env.error.kind.isEmpty()) {
            auto *kindLabel = new QLabel(env.error.kind, w);
            kindLabel->setTextFormat(Qt::PlainText);
            kindLabel->setWordWrap(true);
            form->addRow(i18n("Kind"), kindLabel);
        }
        if (!env.error.message.isEmpty()) {
            auto *messageLabel = new QLabel(env.error.message, w);
            messageLabel->setTextFormat(Qt::PlainText);
            messageLabel->setWordWrap(true);
            form->addRow(i18n("Message"), messageLabel);
        }
        tabs->addTab(w, i18n("Error"));
    }

    // Tools tab
    {
        auto *w = new QWidget(this);
        auto *layout = new QVBoxLayout(w);
        const QJsonObject tools = toolPayloadsObject(env);
        if (!tools.isEmpty()) {
            auto *toolTabs = new QTabWidget(w);
            for (auto it = tools.begin(); it != tools.end(); ++it) {
                const QString toolName = it.key();
                const QJsonObject payload = it.value().toObject();

                const QString argv = jsonPretty(payload.value(QStringLiteral("argv")));
                const QString stdoutText = jsonPretty(payload.value(QStringLiteral("stdout")));
                const QString stderrText = jsonPretty(payload.value(QStringLiteral("stderr")));
                const QString jsonText = jsonPretty(payload.value(QStringLiteral("json")));
                if (argv.isEmpty() && stdoutText.isEmpty() && stderrText.isEmpty() && jsonText.isEmpty())
                    continue;

                auto *tw = new QWidget(toolTabs);
                auto *tl = new QVBoxLayout(tw);

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
            if (toolTabs->count() > 0) layout->addWidget(toolTabs, 1);
            else delete toolTabs;
        }
        if (layout->count() > 0) tabs->addTab(w, i18n("Tools"));
        else delete w;
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
    box.setTextFormat(Qt::PlainText);
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
