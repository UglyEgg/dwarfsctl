// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFont>
#include <QMargins>
#include <QStyle>
#include <QWidget>

namespace dwarfsctl_kde {

inline int layoutSpacing(const QWidget *widget)
{
    return qMax(6, widget->style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing, nullptr, widget));
}

inline QMargins contentMargins(const QWidget *widget)
{
    const int horizontal = qMax(9, widget->style()->pixelMetric(QStyle::PM_LayoutLeftMargin, nullptr, widget));
    const int vertical = qMax(9, widget->style()->pixelMetric(QStyle::PM_LayoutTopMargin, nullptr, widget));
    return QMargins(2 * horizontal, 2 * vertical, 2 * horizontal, 2 * vertical);
}

inline QFont headingFont(QFont font)
{
    if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * 1.2);
    else if (font.pixelSize() > 0) font.setPixelSize(qRound(font.pixelSize() * 1.2));
    font.setBold(true);
    return font;
}

} // namespace dwarfsctl_kde
