// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "visualizationwidgets.h"

#include <QFontMetrics>
#include <QJsonArray>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QToolTip>

#include <KLocalizedString>

#include <algorithm>
#include <cmath>

namespace {

QPainterPath ringSegment(const QPointF &center, qreal innerRadius, qreal outerRadius,
                         qreal startDegrees, qreal spanDegrees)
{
    const QRectF outer(center.x() - outerRadius, center.y() - outerRadius,
                       outerRadius * 2.0, outerRadius * 2.0);
    const QRectF inner(center.x() - innerRadius, center.y() - innerRadius,
                       innerRadius * 2.0, innerRadius * 2.0);
    QPainterPath path;
    path.arcMoveTo(outer, startDegrees);
    path.arcTo(outer, startDegrees, spanDegrees);
    path.arcTo(inner, startDegrees + spanDegrees, -spanDegrees);
    path.closeSubpath();
    return path;
}

QColor ringColor(int index)
{
    static const QColor colors[] = {
        QColor(QStringLiteral("#3daee9")), QColor(QStringLiteral("#577ee8")),
        QColor(QStringLiteral("#9b6bd6")), QColor(QStringLiteral("#f0b84b")),
        QColor(QStringLiteral("#49b77b")), QColor(QStringLiteral("#e06c75")),
        QColor(QStringLiteral("#48bfc5")), QColor(QStringLiteral("#c787d9")),
    };
    return colors[index % std::size(colors)];
}

quint64 jsonUnsigned(const QJsonValue &value)
{
    return value.isDouble() && value.toDouble() > 0.0
        ? static_cast<quint64>(value.toDouble()) : 0;
}

QString dataSize(quint64 bytes)
{
    return QLocale().formattedDataSize(static_cast<qint64>(std::min<quint64>(bytes, std::numeric_limits<qint64>::max())));
}

} // namespace

ElidingLabel::ElidingLabel(QWidget *parent)
    : QLabel(parent)
{
    setTextFormat(Qt::PlainText);
    setTextInteractionFlags(Qt::TextSelectableByMouse);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

void ElidingLabel::setFullText(const QString &text)
{
    m_fullText = text;
    setToolTip(text);
    setAccessibleName(text);
    updateElision();
}

QString ElidingLabel::fullText() const
{
    return m_fullText;
}

void ElidingLabel::resizeEvent(QResizeEvent *event)
{
    QLabel::resizeEvent(event);
    updateElision();
}

void ElidingLabel::updateElision()
{
    QLabel::setText(fontMetrics().elidedText(m_fullText, Qt::ElideMiddle, qMax(0, width())));
}

CompressionOverviewWidget::CompressionOverviewWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("compressionOverview"));
    setMinimumHeight(150);
    setAccessibleName(i18n("Compression overview"));
}

void CompressionOverviewWidget::setValues(qint64 originalBytes, qint64 imageBytes)
{
    m_originalBytes = originalBytes;
    m_imageBytes = imageBytes;
    update();
}

qint64 CompressionOverviewWidget::originalBytes() const { return m_originalBytes; }
qint64 CompressionOverviewWidget::imageBytes() const { return m_imageBytes; }
QSize CompressionOverviewWidget::sizeHint() const { return QSize(640, 170); }

void CompressionOverviewWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF area = rect().adjusted(4, 8, -4, -8);
    if (m_originalBytes <= 0 || m_imageBytes < 0) {
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(area, Qt::AlignCenter, i18n("Compression information is unavailable."));
        return;
    }

    const qint64 saved = qMax<qint64>(0, m_originalBytes - m_imageBytes);
    const double savedPercent = 100.0 * static_cast<double>(saved) / static_cast<double>(m_originalBytes);
    const qreal barLeft = area.left() + 10.0;
    const qreal barWidth = qMax<qreal>(120.0, area.width() - 20.0);
    const qreal barHeight = 34.0;
    const QRectF originalBar(barLeft, area.top() + 32.0, barWidth, barHeight);
    const qreal imageRatio = qBound(0.0, static_cast<double>(m_imageBytes) / static_cast<double>(m_originalBytes), 1.0);
    const QRectF imageTrack(barLeft, area.top() + 101.0, barWidth, barHeight);
    const QRectF imageBar(imageTrack.left(), imageTrack.top(), imageTrack.width() * imageRatio, imageTrack.height());
    const QRectF savedBar(imageBar.right(), imageTrack.top(), imageTrack.right() - imageBar.right(), imageTrack.height());

    QColor originalColor(QStringLiteral("#577ee8"));
    QColor imageColor(QStringLiteral("#3daee9"));
    QColor savedColor(QStringLiteral("#49b77b"));
    painter.setPen(Qt::NoPen);
    painter.setBrush(originalColor);
    painter.drawRoundedRect(originalBar, 7, 7);
    painter.setBrush(palette().color(QPalette::AlternateBase));
    painter.drawRoundedRect(imageTrack, 7, 7);
    painter.setBrush(imageColor);
    painter.drawRoundedRect(imageBar, 7, 7);
    if (savedBar.width() > 1.0) {
        painter.setBrush(savedColor);
        painter.drawRoundedRect(savedBar, 7, 7);
    }

    QFont labelFont = font();
    labelFont.setBold(true);
    painter.setFont(labelFont);
    painter.setPen(Qt::white);
    painter.drawText(originalBar.adjusted(12, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft, i18n("Original"));
    painter.drawText(originalBar.adjusted(12, 0, -12, 0), Qt::AlignVCenter | Qt::AlignRight,
                     QLocale().formattedDataSize(m_originalBytes));
    painter.drawText(imageBar.adjusted(12, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft, i18n("DwarFS image"));
    if (imageBar.width() > 210.0) {
        painter.drawText(imageBar.adjusted(12, 0, -12, 0), Qt::AlignVCenter | Qt::AlignRight,
                         QLocale().formattedDataSize(m_imageBytes));
    }

    painter.setPen(palette().color(QPalette::Text));
    painter.setFont(font());
    painter.drawText(QRectF(barLeft, area.top(), barWidth * 0.55, 24), Qt::AlignLeft | Qt::AlignVCenter,
                     i18n("Whole-image compression"));
    painter.setPen(saved > 0 ? savedColor : palette().color(QPalette::Text));
    QFont savingsFont = font();
    savingsFont.setBold(true);
    savingsFont.setPointSize(savingsFont.pointSize() + 2);
    painter.setFont(savingsFont);
    const QString savings = saved > 0
        ? i18n("%1 smaller", QLocale().toString(savedPercent, 'f', 1) + QStringLiteral("%"))
        : i18n("No size reduction");
    painter.drawText(QRectF(barLeft + barWidth * 0.5, area.top(), barWidth * 0.5, 24),
                     Qt::AlignRight | Qt::AlignVCenter, savings);

    painter.setFont(font());
    painter.setPen(palette().color(QPalette::PlaceholderText));
    painter.drawText(QRectF(barLeft, imageTrack.bottom() + 5, barWidth, 22), Qt::AlignRight | Qt::AlignVCenter,
                     saved > 0 ? i18n("%1 saved", QLocale().formattedDataSize(saved)) : QString());
}

ContentRingsWidget::ContentRingsWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("contentRings"));
    setMinimumHeight(300);
    setMouseTracking(true);
    setAccessibleName(i18n("Image contents by size"));
}

void ContentRingsWidget::setLoading()
{
    m_state = State::Loading;
    m_message = i18n("Reading image contents…");
    m_root = {};
    m_hits.clear();
    update();
}

void ContentRingsWidget::setError(const QString &message)
{
    m_state = State::Error;
    m_message = message.isEmpty() ? i18n("Image contents could not be summarized.") : message;
    m_root = {};
    m_hits.clear();
    update();
}

ContentRingsWidget::Node ContentRingsWidget::parseNode(const QJsonObject &object)
{
    Node node;
    node.name = object.value(QStringLiteral("name")).toString();
    node.path = object.value(QStringLiteral("path")).toString();
    node.type = object.value(QStringLiteral("type")).toString();
    node.bytes = jsonUnsigned(object.value(QStringLiteral("size")));
    node.files = jsonUnsigned(object.value(QStringLiteral("files")));
    node.directories = jsonUnsigned(object.value(QStringLiteral("directories")));
    const QJsonArray children = object.value(QStringLiteral("children")).toArray();
    node.children.reserve(children.size());
    for (const QJsonValue &child : children) {
        if (child.isObject()) node.children.append(parseNode(child.toObject()));
    }
    return node;
}

bool ContentRingsWidget::setUsage(const QJsonObject &usage)
{
    if (usage.value(QStringLiteral("schema")).toString() != QStringLiteral("dwarfsctl.usage.v1")
        || !usage.value(QStringLiteral("root")).isObject()) {
        setError(i18n("The image returned an unsupported contents summary."));
        return false;
    }
    m_root = parseNode(usage.value(QStringLiteral("root")).toObject());
    m_state = State::Ready;
    m_message.clear();
    m_hits.clear();
    update();
    return true;
}

quint64 ContentRingsWidget::totalBytes() const { return m_root.bytes; }
int ContentRingsWidget::topLevelCount() const { return m_root.children.size(); }
QSize ContentRingsWidget::sizeHint() const { return QSize(700, 410); }

void ContentRingsWidget::drawCenteredMessage(QPainter &painter, const QString &message) const
{
    painter.setPen(m_state == State::Error ? palette().color(QPalette::Link)
                                           : palette().color(QPalette::PlaceholderText));
    painter.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap, message);
}

void ContentRingsWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    m_hits.clear();
    if (m_state != State::Ready) {
        drawCenteredMessage(painter, m_message);
        return;
    }
    if (m_root.children.isEmpty()) {
        drawCenteredMessage(painter, i18n("This image contains no files."));
        return;
    }

    const QRectF area = rect().adjusted(14, 12, -14, -12);
    const bool showLegend = area.width() >= 620.0;
    const qreal legendWidth = showLegend ? qMin<qreal>(230.0, area.width() * 0.31) : 0.0;
    const QRectF chartArea(area.left() + legendWidth, area.top(), area.width() - legendWidth, area.height());
    const qreal radius = qMax<qreal>(70.0, qMin(chartArea.height() * 0.47, chartArea.width() * 0.47));
    const QPointF center(chartArea.center().x(), chartArea.center().y() + 4.0);
    const qreal hole = radius * 0.31;
    const qreal middle = radius * 0.65;
    const qreal gap = qMax<qreal>(0.7, 110.0 / radius);
    const quint64 total = m_root.bytes;
    const qreal equalSpan = 360.0 / static_cast<qreal>(m_root.children.size());
    qreal start = 90.0;

    for (int index = 0; index < m_root.children.size(); ++index) {
        const Node &node = m_root.children.at(index);
        const qreal span = total > 0 ? 360.0 * static_cast<qreal>(node.bytes) / static_cast<qreal>(total) : equalSpan;
        const qreal visibleSpan = qMax<qreal>(0.15, span - gap);
        QColor color = ringColor(index);
        QPainterPath inner = ringSegment(center, hole, middle - 1.5, start + gap * 0.5, visibleSpan);
        painter.setPen(QPen(palette().color(QPalette::Base), 1.2));
        painter.setBrush(color);
        painter.drawPath(inner);
        m_hits.append({inner, node.name, node.path, node.bytes, node.files, node.directories});

        if (!node.children.isEmpty()) {
            qreal childStart = start;
            for (int childIndex = 0; childIndex < node.children.size(); ++childIndex) {
                const Node &child = node.children.at(childIndex);
                const qreal childSpan = node.bytes > 0
                    ? span * static_cast<qreal>(child.bytes) / static_cast<qreal>(node.bytes)
                    : span / static_cast<qreal>(node.children.size());
                QColor childColor = color.lighter(112 + (childIndex % 3) * 8);
                QPainterPath outer = ringSegment(center, middle + 1.5, radius,
                                                 childStart + gap * 0.5, qMax<qreal>(0.12, childSpan - gap));
                painter.setBrush(childColor);
                painter.drawPath(outer);
                m_hits.append({outer, child.name, child.path, child.bytes, child.files, child.directories});
                childStart += childSpan;
            }
        } else {
            QPainterPath outer = ringSegment(center, middle + 1.5, radius,
                                             start + gap * 0.5, visibleSpan);
            painter.setBrush(color.lighter(118));
            painter.drawPath(outer);
            m_hits.append({outer, node.name, node.path, node.bytes, node.files, node.directories});
        }
        start += span;
    }

    painter.setPen(palette().color(QPalette::Text));
    QFont totalFont = font();
    totalFont.setBold(true);
    totalFont.setPointSize(totalFont.pointSize() + 2);
    painter.setFont(totalFont);
    painter.drawText(QRectF(center.x() - hole, center.y() - 22, hole * 2, 24),
                     Qt::AlignCenter, dataSize(m_root.bytes));
    painter.setFont(font());
    painter.setPen(palette().color(QPalette::PlaceholderText));
    painter.drawText(QRectF(center.x() - hole, center.y() + 2, hole * 2, 20),
                     Qt::AlignCenter, i18n("contents"));

    if (showLegend) {
        QFont titleFont = font();
        titleFont.setBold(true);
        titleFont.setPointSize(titleFont.pointSize() + 1);
        painter.setFont(titleFont);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(QRectF(area.left(), area.top() + 4, legendWidth - 14, 26),
                         Qt::AlignLeft | Qt::AlignVCenter, i18n("Content Rings"));
        painter.setFont(font());
        qreal y = area.top() + 44.0;
        const int rows = qMin(7, m_root.children.size());
        for (int index = 0; index < rows; ++index) {
            const Node &node = m_root.children.at(index);
            painter.setPen(Qt::NoPen);
            painter.setBrush(ringColor(index));
            painter.drawRoundedRect(QRectF(area.left(), y + 3, 12, 12), 3, 3);
            painter.setPen(palette().color(QPalette::Text));
            const QRectF nameRect(area.left() + 21, y, legendWidth - 106, 20);
            painter.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                             fontMetrics().elidedText(node.name.isEmpty() ? i18n("Unnamed") : node.name,
                                                      Qt::ElideRight, static_cast<int>(nameRect.width())));
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(QRectF(area.left() + legendWidth - 90, y, 78, 20),
                             Qt::AlignRight | Qt::AlignVCenter, dataSize(node.bytes));
            y += 28.0;
        }
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(QRectF(area.left(), area.bottom() - 48, legendWidth - 14, 42),
                         Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap,
                         i18n("Point to a ring to see its path and logical size."));
    }
}

void ContentRingsWidget::mouseMoveEvent(QMouseEvent *event)
{
    for (auto it = m_hits.crbegin(); it != m_hits.crend(); ++it) {
        if (!it->shape.contains(event->position())) continue;
        QStringList details;
        details << (it->path.isEmpty() ? it->name : it->path)
                << dataSize(it->bytes)
                << i18np("1 file", "%1 files", it->files);
        if (it->directories > 0) details << i18np("1 folder", "%1 folders", it->directories);
        QToolTip::showText(event->globalPosition().toPoint(), details.join(QStringLiteral("\n")), this);
        return;
    }
    QToolTip::hideText();
}

void ContentRingsWidget::leaveEvent(QEvent *event)
{
    QToolTip::hideText();
    QWidget::leaveEvent(event);
}
