// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "aboutdialog.h"

#include <KLocalizedString>
#include <KPageWidget>
#include <KPageWidgetItem>
#include <KPageWidgetModel>

#include <QFile>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QtTest>

class AboutDialogTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void releaseHistoryIsTheFinalTab()
    {
        KLocalizedString::setApplicationDomain("dwarfsctl");
        QFile canonical(qEnvironmentVariable("DWARFSCTL_ABOUT_TEST_CHANGELOG",
                                             QStringLiteral(DWARFSCTL_TEST_CHANGELOG)));
        QFile embedded(QStringLiteral(":/dwarfsctl/CHANGELOG.md"));
        QVERIFY(canonical.open(QIODevice::ReadOnly));
        QVERIFY(embedded.open(QIODevice::ReadOnly));
        const QByteArray markdown = canonical.readAll();
        QCOMPARE(embedded.readAll(), markdown);

        AboutDialog dialog;
        auto *browser = dialog.findChild<QTextBrowser *>(QStringLiteral("changeLogBrowser"));
        QVERIFY(browser);
        QVERIFY(browser->isReadOnly());
        QTextDocument expected;
        expected.setMarkdown(QString::fromUtf8(markdown));
        QCOMPARE(browser->toPlainText(), expected.toPlainText());

        auto *pages = dialog.findChild<KPageWidget *>();
        QVERIFY(pages);
        auto *model = qobject_cast<KPageWidgetModel *>(pages->model());
        QVERIFY(model);
        const auto lastIndex = model->index(model->rowCount() - 1, 0);
        auto *last = model->item(lastIndex);
        QVERIFY(last);
        QCOMPARE(last->name(), i18n("Change Log"));
        QCOMPARE(last->widget(), browser);
        dialog.setCurrentPage(last);
        dialog.show();
        QTRY_VERIFY(browser->isVisible());
        QTRY_VERIFY(browser->verticalScrollBar()->maximum() > 0);

        const QString capture = qEnvironmentVariable("DWARFSCTL_ABOUT_CAPTURE_DIR");
        if (!capture.isEmpty()) {
            QVERIFY(dialog.grab().save(capture + QStringLiteral("/about-changelog.png")));
        }
        dialog.resize(620, 480);
        QTest::qWait(100);
        QVERIFY(browser->parentWidget()->rect().contains(browser->geometry()));
        if (!capture.isEmpty()) {
            QVERIFY(dialog.grab().save(capture + QStringLiteral("/about-changelog-narrow.png")));
        }
    }
};

QTEST_MAIN(AboutDialogTest)
#include "tst_about_dialog.moc"
