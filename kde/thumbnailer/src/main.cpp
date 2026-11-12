// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QGuiApplication>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QStandardPaths>

#include "dwarfsctl_exec.h"

static bool getMountedState(const QString &imagePath, bool &mountedOut) {
    QProcess p;
    QStringList args;
    // The thumbnailer should be best-effort and must not require *all* DwarFS tools.
    // Use the mount registry status endpoint (no dependency gating).
    args << "mount" << "status" << imagePath;

    p.start(dwarfsctl_kde::dwarfsctlExe(), args);
    if (!p.waitForFinished(3000)) {
        p.kill();
        return false;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        return false;
    }
    const QByteArray out = p.readAllStandardOutput();
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(out, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    const QJsonObject root = doc.object();
    const QJsonObject details = root.value("details").toObject();
    mountedOut = details.value("mounted").toBool(false);
    return true;
}

static QImage renderThumb(int sizePx, bool mounted) {
    QImage img(sizePx, sizePx, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Match the installed D-Mark document icon. Drawing paths directly avoids
    // font discovery in Dolphin's thumbnail process and keeps the result stable
    // in both desktop and headless package tests.
    const qreal s = sizePx / 64.0;
    QPainterPath page;
    page.moveTo(12 * s, 3 * s);
    page.lineTo(39 * s, 3 * s);
    page.lineTo(52 * s, 16 * s);
    page.lineTo(52 * s, 59 * s);
    page.quadTo(52 * s, 61 * s, 50 * s, 61 * s);
    page.lineTo(14 * s, 61 * s);
    page.quadTo(12 * s, 61 * s, 12 * s, 59 * s);
    page.closeSubpath();
    painter.setPen(QPen(QColor("#60768d"), qMax<qreal>(1.0, 2 * s)));
    painter.setBrush(QColor("#f4f7fa"));
    painter.drawPath(page);

    QPainterPath fold;
    fold.moveTo(39 * s, 3 * s);
    fold.lineTo(39 * s, 16 * s);
    fold.lineTo(52 * s, 16 * s);
    fold.closeSubpath();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#cbd8e4"));
    painter.drawPath(fold);

    QLinearGradient navy(0, 22 * s, 0, 52 * s);
    navy.setColorAt(0, QColor("#31465e"));
    navy.setColorAt(1, QColor("#172335"));
    painter.setBrush(navy);
    painter.drawRoundedRect(QRectF(17 * s, 22 * s, 30 * s, 30 * s), 7 * s, 7 * s);

    QPainterPath mark;
    mark.setFillRule(Qt::OddEvenFill);
    mark.moveTo(22 * s, 26 * s);
    mark.lineTo(32 * s, 26 * s);
    mark.cubicTo(40 * s, 26 * s, 45 * s, 30 * s, 45 * s, 37 * s);
    mark.cubicTo(45 * s, 44 * s, 40 * s, 48 * s, 32 * s, 48 * s);
    mark.lineTo(22 * s, 48 * s);
    mark.closeSubpath();
    mark.moveTo(29 * s, 32 * s);
    mark.lineTo(29 * s, 42 * s);
    mark.lineTo(32 * s, 42 * s);
    mark.cubicTo(36 * s, 42 * s, 38 * s, 40 * s, 38 * s, 37 * s);
    mark.cubicTo(38 * s, 34 * s, 36 * s, 32 * s, 32 * s, 32 * s);
    mark.closeSubpath();
    QLinearGradient orange(22 * s, 26 * s, 45 * s, 48 * s);
    orange.setColorAt(0, QColor("#ffb14e"));
    orange.setColorAt(1, QColor("#ec5f45"));
    painter.setBrush(orange);
    painter.drawPath(mark);

    painter.setBrush(QColor("#fff3df"));
    painter.drawRect(QRectF(31 * s, 34 * s, 8 * s, qMax<qreal>(1.0, 2 * s)));
    painter.drawRect(QRectF(31 * s, 39 * s, 8 * s, qMax<qreal>(1.0, 2 * s)));

    if (mounted) {
        // Small badge in bottom-right
        const qreal badgeSize = sizePx * 0.28;
        const QRectF badge(sizePx - badgeSize - 4, sizePx - badgeSize - 4, badgeSize, badgeSize);
        painter.setBrush(QColor(20, 160, 60, 240));
        painter.setPen(QPen(QColor(255, 255, 255, 220), 2));
        painter.drawEllipse(badge);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(255, 255, 255), qMax<qreal>(1.5, 2 * s), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath check;
        check.moveTo(badge.left() + badge.width() * .25, badge.center().y());
        check.lineTo(badge.left() + badge.width() * .44, badge.top() + badge.height() * .68);
        check.lineTo(badge.left() + badge.width() * .76, badge.top() + badge.height() * .32);
        painter.drawPath(check);
    }

    painter.end();
    return img;
}

int main(int argc, char **argv) {
    // QImage/QPainter platform setup still requires a GUI application.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    const QStringList a = app.arguments();
    if (a.size() < 4) {
        return 2;
    }
    const QString inPath = a.at(1);
    const QString outPath = a.at(2);
    bool ok = false;
    const int sizePx = a.at(3).toInt(&ok);
    if (!ok || sizePx <= 0 || sizePx > 4096) {
        return 2;
    }

    bool mounted = false;
    // Best-effort. If status fails, render base icon only.
    getMountedState(inPath, mounted);

    const QImage img = renderThumb(sizePx, mounted);
    if (!img.save(outPath, "PNG")) {
        return 1;
    }
    return 0;
}
