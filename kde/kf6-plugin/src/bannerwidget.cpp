// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bannerwidget.h"

#include <QAction>

namespace dwarfsplugin {

BannerWidget::BannerWidget(QWidget *parent)
    : KMessageWidget(parent)
{
    setCloseButtonVisible(false);
    setPosition(KMessageWidget::Inline);
    setTextFormat(Qt::RichText);
    setWordWrap(true);
    setVisible(false);
}

void BannerWidget::setSeverity(Severity severity)
{
    setMessageType(severity == Severity::Warning ? KMessageWidget::Warning
                                                 : KMessageWidget::Positive);
}

void BannerWidget::setTitleAndBody(const QString &title, const QString &body)
{
    m_title = title;
    m_body = body;
    refreshText();
}

void BannerWidget::setDetails(const QString &details)
{
    m_details = details;
    refreshText();
}

QAction *BannerWidget::addActionButton(const QString &text)
{
    auto *action = new QAction(text, this);
    addAction(action);
    return action;
}

void BannerWidget::refreshText()
{
    QStringList sections;
    if (!m_title.isEmpty()) {
        sections << QStringLiteral("<strong>%1</strong>").arg(m_title.toHtmlEscaped());
    }
    if (!m_body.isEmpty()) {
        sections << m_body.toHtmlEscaped();
    }
    if (!m_details.isEmpty()) {
        sections << QStringLiteral("<small>%1</small>").arg(m_details.toHtmlEscaped());
    }
    setText(sections.join(QStringLiteral("<br>")));
}

} // namespace dwarfsplugin
