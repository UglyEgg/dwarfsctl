// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KMessageWidget>

class QAction;

namespace dwarfsplugin {

// KDE-native inline feedback. KMessageWidget derives all colours, icons,
// spacing, contrast, and high-contrast behaviour from the active Plasma theme.
class BannerWidget final : public KMessageWidget
{
    Q_OBJECT

public:
    enum class Severity {
        Info,
        Warning,
    };

    explicit BannerWidget(QWidget *parent = nullptr);

    void setSeverity(Severity severity);
    void setTitleAndBody(const QString &title, const QString &body);
    void setDetails(const QString &details);
    QAction *addActionButton(const QString &text);

private:
    void refreshText();

    QString m_title;
    QString m_body;
    QString m_details;
};

} // namespace dwarfsplugin
