// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "visualizationwidgets.h"
#include "uipresentation.h"

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

QColor ringColor(int index, const QPalette &palette)
{
    const QColor accent = palette.color(QPalette::Highlight);
    const qreal hue = accent.hslHueF() < 0 ? 0.56 : accent.hslHueF();
    const qreal saturation = qBound(0.45, accent.hslSaturationF(), 0.75);
    const bool dark = palette.color(QPalette::Window).lightnessF() < 0.5;
    return QColor::fromHslF(std::fmod(hue + index * 0.381966, 1.0), saturation, dark ? 0.64 : 0.42);
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
QSize CompressionOverviewWidget::minimumSizeHint() const
{
    const qreal row = fontMetrics().height();
    const qreal bar = qMax<qreal>(8, row * 0.55);
    const int height = static_cast<int>(std::ceil(16 + 4.3 * row + 2 * bar
                                                + 5 * dwarfsctl_kde::layoutSpacing(this)));
    return QSize(120, qMax(150, height));
}

QSize CompressionOverviewWidget::sizeHint() const
{
    return QSize(640, minimumSizeHint().height());
}

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
    const double ratio = qBound(0.0, static_cast<double>(m_imageBytes) / static_cast<double>(m_originalBytes), 1.0);
    const qreal gap = dwarfsctl_kde::layoutSpacing(this);
    const qreal rowHeight = fontMetrics().height();
    const qreal barHeight = qMax<qreal>(8, rowHeight * 0.55);
    const qreal rowStep = rowHeight + barHeight + 2 * gap;
    const qreal left = area.left();
    const qreal width = area.width();

    const QString savings = saved > 0
        ? i18n("%1 smaller", QLocale().toString(savedPercent, 'f', 1) + QStringLiteral("%"))
        : i18n("No size reduction");
    const QFont heading = dwarfsctl_kde::headingFont(font());
    const QFontMetrics headingMetrics(heading);
    const qreal savingsWidth = qMin<qreal>(width, headingMetrics.horizontalAdvance(savings));
    painter.setFont(heading);
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(QRectF(left, area.top(), width - savingsWidth - gap, rowHeight * 1.3),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     headingMetrics.elidedText(i18n("Whole-image compression"), Qt::ElideRight,
                                               qMax(0, static_cast<int>(width - savingsWidth - gap))));
    painter.drawText(QRectF(area.right() - savingsWidth, area.top(), savingsWidth, rowHeight * 1.3),
                     Qt::AlignRight | Qt::AlignVCenter, savings);

    painter.setFont(font());
    const auto drawRow = [&](const QString &label, const QString &value, qreal top) {
        const qreal valueWidth = fontMetrics().horizontalAdvance(value);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(QRectF(left, top, qMax<qreal>(0, width - valueWidth - gap), rowHeight),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(label, Qt::ElideRight,
                                                qMax(0, static_cast<int>(width - valueWidth - gap))));
        painter.drawText(QRectF(left, top, width, rowHeight), Qt::AlignRight | Qt::AlignVCenter, value);
    };
    const qreal firstTop = area.top() + rowHeight * 1.3 + gap;
    drawRow(i18n("Original size"), dataSize(m_originalBytes), firstTop);
    drawRow(i18n("DwarFS image"), dataSize(m_imageBytes), firstTop + rowStep);

    const QColor accent = palette().color(QPalette::Highlight);
    QColor originalColor = accent;
    originalColor.setAlphaF(0.45);
    const QRectF originalBar(left, firstTop + rowHeight + gap, width, barHeight);
    const QRectF imageTrack(left, originalBar.top() + rowStep, width, barHeight);
    painter.setPen(Qt::NoPen);
    painter.setBrush(originalColor);
    painter.drawRoundedRect(originalBar, barHeight / 2, barHeight / 2);
    painter.setBrush(palette().color(QPalette::AlternateBase));
    painter.drawRoundedRect(imageTrack, barHeight / 2, barHeight / 2);
    painter.setBrush(accent);
    painter.drawRoundedRect(QRectF(imageTrack.topLeft(), QSizeF(width * ratio, barHeight)),
                            barHeight / 2, barHeight / 2);

    painter.setPen(palette().color(QPalette::PlaceholderText));
    painter.drawText(QRectF(left, imageTrack.bottom() + gap, width, rowHeight),
                     Qt::AlignRight | Qt::AlignVCenter,
                     saved > 0 ? i18n("%1 saved", dataSize(saved)) : QString());
}

ContentRingsWidget::ContentRingsWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("contentRings"));
    setMinimumHeight(160);
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

bool ContentRingsWidget::hasUsage() const { return m_state == State::Ready; }
quint64 ContentRingsWidget::totalBytes() const { return m_root.bytes; }
int ContentRingsWidget::topLevelCount() const { return m_root.children.size(); }
QSize ContentRingsWidget::sizeHint() const { return QSize(700, 410); }

void ContentRingsWidget::drawCenteredMessage(QPainter &painter, const QString &message) const
{
    painter.setPen(palette().color(QPalette::PlaceholderText));
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
    const qreal spacing = dwarfsctl_kde::layoutSpacing(this);
    const qreal rowHeight = fontMetrics().height();
    const qreal dotSize = qMax<qreal>(10, rowHeight * 0.55);
    qreal valueWidth = 0;
    for (const auto &node : m_root.children)
        valueWidth = qMax<qreal>(valueWidth, fontMetrics().horizontalAdvance(dataSize(node.bytes)));
    const qreal desiredLegendWidth = qMax<qreal>(220, dotSize + 4 * spacing + valueWidth
                                                   + fontMetrics().horizontalAdvance(i18n("Documents")));
    const bool showLegend = area.width() >= qMax<qreal>(620, desiredLegendWidth + 260);
    const qreal legendWidth = showLegend ? desiredLegendWidth : 0;
    const QRectF chartArea(area.left() + legendWidth, area.top(), area.width() - legendWidth, area.height());
    const qreal radius = qMax<qreal>(70.0, qMin(chartArea.height() * 0.47, chartArea.width() * 0.47));
    const QPointF center(chartArea.center().x(), chartArea.center().y() + 4.0);
    const QFont totalFont = dwarfsctl_kde::headingFont(font());
    const QFontMetrics totalMetrics(totalFont);
    const qreal centerWidth = qMax(totalMetrics.horizontalAdvance(dataSize(m_root.bytes)),
                                   fontMetrics().horizontalAdvance(i18n("contents")));
    const qreal hole = qMin(radius * 0.56, qMax(radius * 0.31, centerWidth / 2 + spacing));
    const qreal middle = qMax(radius * 0.65, hole + radius * 0.12);
    const qreal gap = qMax<qreal>(0.7, 110.0 / radius);
    const quint64 total = m_root.bytes;
    const qreal equalSpan = 360.0 / static_cast<qreal>(m_root.children.size());
    qreal start = 90.0;

    for (int index = 0; index < m_root.children.size(); ++index) {
        const Node &node = m_root.children.at(index);
        const qreal span = total > 0 ? 360.0 * static_cast<qreal>(node.bytes) / static_cast<qreal>(total) : equalSpan;
        const qreal visibleSpan = qMax<qreal>(0.15, span - gap);
        QColor color = ringColor(index, palette());
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
    painter.setFont(totalFont);
    const qreal totalHeight = totalMetrics.height();
    const qreal centerTop = center.y() - (totalHeight + rowHeight + spacing) / 2;
    const qreal textWidth = qMax<qreal>(0, 2 * hole - 2 * spacing);
    painter.drawText(QRectF(center.x() - textWidth / 2, centerTop, textWidth, totalHeight),
                     Qt::AlignCenter,
                     totalMetrics.elidedText(dataSize(m_root.bytes), Qt::ElideRight, static_cast<int>(textWidth)));
    painter.setFont(font());
    painter.setPen(palette().color(QPalette::PlaceholderText));
    painter.drawText(QRectF(center.x() - textWidth / 2, centerTop + totalHeight + spacing, textWidth, rowHeight),
                     Qt::AlignCenter,
                     fontMetrics().elidedText(i18n("contents"), Qt::ElideRight, static_cast<int>(textWidth)));

    if (showLegend) {
        painter.setFont(totalFont);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(QRectF(area.left(), area.top(), legendWidth - 2 * spacing, totalHeight),
                         Qt::AlignLeft | Qt::AlignVCenter, i18n("Content Rings"));
        painter.setFont(font());
        const QString hint = i18n("Point to a ring to see its path and logical size.");
        const qreal hintWidth = legendWidth - 2 * spacing;
        const int hintHeight = fontMetrics().boundingRect(QRect(0, 0, static_cast<int>(hintWidth), 1000),
                                                         Qt::TextWordWrap, hint).height();
        qreal y = area.top() + totalHeight + 2 * spacing;
        const int availableRows = qMax(0, static_cast<int>((area.bottom() - hintHeight - spacing - y)
                                                          / (rowHeight + spacing)));
        const int rows = qMin(qMin(7, availableRows), static_cast<int>(m_root.children.size()));
        for (int index = 0; index < rows; ++index) {
            const Node &node = m_root.children.at(index);
            painter.setPen(Qt::NoPen);
            painter.setBrush(ringColor(index, palette()));
            painter.drawRoundedRect(QRectF(area.left(), y + (rowHeight - dotSize) / 2, dotSize, dotSize), 3, 3);
            painter.setPen(palette().color(QPalette::Text));
            const qreal nameLeft = area.left() + dotSize + spacing;
            const qreal valueLeft = area.left() + legendWidth - valueWidth - spacing;
            const QRectF nameRect(nameLeft, y, valueLeft - nameLeft - spacing, rowHeight);
            painter.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                             fontMetrics().elidedText(node.name.isEmpty() ? i18n("Unnamed") : node.name,
                                                      Qt::ElideRight, static_cast<int>(nameRect.width())));
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(QRectF(valueLeft, y, valueWidth, rowHeight),
                             Qt::AlignRight | Qt::AlignVCenter, dataSize(node.bytes));
            y += rowHeight + spacing;
        }
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(QRectF(area.left(), area.bottom() - hintHeight, hintWidth, hintHeight),
                         Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap, hint);
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
