// SPDX-License-Identifier: GPL-3.0-or-later
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include "dwarfsctl_exec.h"

class OpenImageWindow : public QDialog {
public:
    explicit OpenImageWindow(const QString &image, const QString &backend = dwarfsctl_kde::dwarfsctlExe()) {
        setWindowTitle(tr("Open DwarFS image"));
        setMinimumWidth(440);
        auto *layout = new QVBoxLayout(this);
        status = new QLabel(tr("Mounting %1…").arg(QFileInfo(image).fileName()), this);
        status->setObjectName(QStringLiteral("mountStatus"));
        status->setTextFormat(Qt::PlainText);
        status->setWordWrap(true);
        layout->addWidget(status);
        progress = new QProgressBar(this);
        progress->setRange(0, 0);
        layout->addWidget(progress);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        closeButton = buttons->button(QDialogButtonBox::Close);
        closeButton->setEnabled(false);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
        layout->addWidget(buttons);
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, this, [this] {
            process.kill();
            fail(tr("Mounting did not finish in time. Check the tray for the image's current state."));
        });
        connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) fail(tr("Cannot start the DwarFS backend: %1").arg(process.errorString()));
        });
        connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exitStatus) {
            if (!busy) return;
            timer.stop();
            const auto raw = process.readAllStandardOutput();
            QJsonParseError parseError;
            const auto envelope = QJsonDocument::fromJson(raw, &parseError).object();
            if (exitStatus != QProcess::NormalExit || code != 0 || parseError.error != QJsonParseError::NoError || !envelope.value(QStringLiteral("ok")).toBool()) {
                auto message = envelope.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
                if (message.isEmpty()) message = tr("The image could not be mounted.");
                fail(message, QString::fromUtf8(raw.left(32768)) + QString::fromUtf8(process.readAllStandardError().left(32768)));
                return;
            }
            mountpoint = envelope.value(QStringLiteral("details")).toObject().value(QStringLiteral("mountpoint")).toString();
            if (mountpoint.isEmpty() || !QFileInfo(mountpoint).isDir()) {
                fail(tr("The backend did not return an accessible mount folder."));
                return;
            }
            busy = false;
            progress->setRange(0, 100);
            progress->setValue(100);
            status->setText(tr("Opening mounted volume…"));
            if (openFolder()) {
                accept();
            }
        });
        show();
        timer.start(300000);
        process.start(backend, {QStringLiteral("mount"), QFileInfo(image).absoluteFilePath()});
    }
protected:
    void reject() override { if (!busy) QDialog::reject(); }
    void closeEvent(QCloseEvent *event) override { if (busy) event->ignore(); else QDialog::closeEvent(event); }
private:
    bool openFolder() {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(mountpoint))) {
            QMessageBox::critical(this, tr("Open mounted image"), tr("The image is mounted at %1, but the file manager could not be opened.").arg(mountpoint));
            reject();
            return false;
        }
        return true;
    }
    void fail(const QString &message, const QString &details = {}) {
        if (!busy) return;
        busy = false;
        timer.stop();
        progress->hide();
        status->setText(tr("Mount failed"));
        closeButton->setEnabled(true);
        QMessageBox box(QMessageBox::Critical, tr("Cannot mount DwarFS image"), message, QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.setDetailedText(details);
        box.exec();
        reject();
    }
    bool busy = true;
    QString mountpoint;
    QLabel *status;
    QProgressBar *progress;
    QPushButton *closeButton;
    QProcess process;
    QTimer timer;
};
#ifndef DWARFSCTL_OPEN_TEST
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (app.arguments().size() != 2) {
        QMessageBox::critical(nullptr, QObject::tr("Open DwarFS image"), QObject::tr("Select one local DwarFS image to open."));
        return 2;
    }
    OpenImageWindow window(app.arguments().at(1));
    return app.exec();
}
#endif
