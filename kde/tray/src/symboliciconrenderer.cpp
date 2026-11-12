// SPDX-License-Identifier: GPL-3.0-or-later

#include "symboliciconrenderer.h"

#include <QFile>
#include <QPainter>
#include <QSvgRenderer>

namespace dwarfsctl_tray {

QImage renderSymbolicIcon(const QString& path, const QColor& foreground, const QSize& pixelSize)
{
    if (!foreground.isValid() || pixelSize.isEmpty()) {
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QByteArray svg = file.readAll();
    // The SVG's value is a standards-compatible fallback for consumers that do
    // not apply KDE color schemes. Replace it with the palette color reported
    // by Qt/KDE before handing the pixels to the StatusNotifier host.
    svg.replace("#232629", foreground.name(QColor::HexRgb).toUtf8());

    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) {
        return {};
    }

    QImage image(pixelSize, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    renderer.render(&painter);
    return image;
}

} // namespace dwarfsctl_tray
