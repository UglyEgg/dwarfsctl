// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QLabel>
#include <QPainterPath>
#include <QVector>
#include <QWidget>

class ElidingLabel final : public QLabel {
public:
    explicit ElidingLabel(QWidget *parent = nullptr);
    void setFullText(const QString &text);
    QString fullText() const;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateElision();
    QString m_fullText;
};

class CompressionOverviewWidget final : public QWidget {
public:
    explicit CompressionOverviewWidget(QWidget *parent = nullptr);
    void setValues(qint64 originalBytes, qint64 imageBytes);
    qint64 originalBytes() const;
    qint64 imageBytes() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    qint64 m_originalBytes = -1;
    qint64 m_imageBytes = -1;
};

class ContentRingsWidget final : public QWidget {
public:
    explicit ContentRingsWidget(QWidget *parent = nullptr);
    void setLoading();
    void setError(const QString &message);
    bool setUsage(const QJsonObject &usage);
    quint64 totalBytes() const;
    int topLevelCount() const;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Node {
        QString name;
        QString path;
        QString type;
        quint64 bytes = 0;
        quint64 files = 0;
        quint64 directories = 0;
        QVector<Node> children;
    };
    struct HitRegion {
        QPainterPath shape;
        QString name;
        QString path;
        quint64 bytes = 0;
        quint64 files = 0;
        quint64 directories = 0;
    };
    enum class State { Loading, Ready, Error };

    static Node parseNode(const QJsonObject &object);
    void drawCenteredMessage(QPainter &painter, const QString &message) const;

    State m_state = State::Loading;
    QString m_message;
    Node m_root;
    QVector<HitRegion> m_hits;
};
