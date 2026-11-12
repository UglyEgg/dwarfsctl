// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QImage>
#include <QSize>
#include <QString>

namespace dwarfsctl_tray {

QImage renderSymbolicIcon(const QString& path, const QColor& foreground, const QSize& pixelSize);

} // namespace dwarfsctl_tray
