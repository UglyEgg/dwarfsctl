// SPDX-License-Identifier: GPL-3.0-or-later
#include "directmountoperation.h"
#include "operationstatus.h"
#include "uipresentation.h"
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>
#include <KLocalizedString>
#include <exception>
#include <memory>

namespace dwarfsctl_tray {
namespace {
class Feedback final : public QDialog {
public:
    explicit Feedback(const QString &title, const QString &running)
    {
        setObjectName(QStringLiteral("directMountFeedback"));
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(title);
        setMinimumWidth(440);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(dwarfsctl_kde::contentMargins(this));
        status = new dwarfsctl_kde::OperationStatus(this);
        status->begin(running);
        layout->addWidget(status);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        closeButton = buttons->button(QDialogButtonBox::Close);
        closeButton->setObjectName(QStringLiteral("closeMountOperation"));
        closeButton->setEnabled(false);
        connect(buttons, &QDialogButtonBox::rejected, this, &Feedback::reject);
        layout->addWidget(buttons);
    }
    void reject() override { if (!status->isActive()) QDialog::reject(); }
    void closeEvent(QCloseEvent *event) override
    {
        if (status->isActive()) event->ignore(); else QDialog::closeEvent(event);
    }
    dwarfsctl_kde::OperationStatus *status;
    QPushButton *closeButton;
};
QByteArray failedResult(const QString &message)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("ok"), false},
        {QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), message}}}}).toJson(QJsonDocument::Compact);
}
}
DirectMountOperation::DirectMountOperation(QObject *parent) : QObject(parent) {}
DirectMountOperation::~DirectMountOperation()
{
    // Session shutdown may bypass the disabled Quit action. Let the owned,
    // bounded executor finish; never destroy a running thread or fake a cancel.
    if (m_thread) { m_thread->wait(); delete m_thread; }
    delete m_dialog.data();
}
void DirectMountOperation::showCurrent()
{
    if (m_dialog) { m_dialog->show(); m_dialog->raise(); m_dialog->activateWindow(); }
}
bool DirectMountOperation::start(const QString &title, const QString &running,
                                const QString &success, Work work)
{
    if (isActive()) { showCurrent(); return false; }
    if (!work) return false;
    if (m_dialog) m_dialog->close();
    auto *feedback = new Feedback(title, running);
    m_dialog = feedback;
    auto result = std::make_shared<QByteArray>();
    m_thread = QThread::create([work = std::move(work), result] {
        try { *result = work(); }
        catch (const std::exception &error) { *result = failedResult(QString::fromUtf8(error.what())); }
        catch (...) { *result = failedResult(i18n("The mount operation could not be completed.")); }
    });
    connect(m_thread, &QThread::finished, this, [this, result, success] {
        m_thread->wait();
        m_thread->deleteLater();
        m_thread = nullptr;
        const auto doc = QJsonDocument::fromJson(*result);
        const auto envelope = doc.object();
        auto *feedback = static_cast<Feedback *>(m_dialog.data());
        if (feedback) {
            if (doc.isObject() && envelope.value(QStringLiteral("ok")).isBool()
                && envelope.value(QStringLiteral("ok")).toBool()) feedback->status->succeed(success);
            else {
                const auto message = envelope.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
                feedback->status->fail(message.isEmpty() ? i18n("The mount service returned an invalid response. Check the current mount state before trying again.") : message);
            }
            feedback->closeButton->setEnabled(true);
            feedback->closeButton->setDefault(true);
        }
        Q_EMIT finished(*result);
    });
    feedback->show();
    m_thread->start();
    return true;
}
}
