// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
class QDialog;
class QThread;
namespace dwarfsctl_tray {
// Owns one non-cancellable executor call. The callback must not access GUI objects.
class DirectMountOperation final : public QObject {
    Q_OBJECT
public:
    using Work = std::function<QByteArray()>;
    explicit DirectMountOperation(QObject *parent = nullptr);
    ~DirectMountOperation() override;
    bool start(const QString &title, const QString &running, const QString &success, Work work);
    bool isActive() const { return m_thread != nullptr; }
    void showCurrent();
Q_SIGNALS:
    void finished(const QByteArray &result);
private:
    QThread *m_thread = nullptr;
    QPointer<QDialog> m_dialog;
};
}
