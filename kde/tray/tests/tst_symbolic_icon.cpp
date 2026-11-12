// SPDX-License-Identifier: GPL-3.0-or-later

#include "symboliciconrenderer.h"

#include <QColor>
#include <QImage>
#include <QTest>

using dwarfsctl_tray::renderSymbolicIcon;

class SymbolicIconTest : public QObject {
    Q_OBJECT

private slots:
    void followsPaletteColor_data()
    {
        QTest::addColumn<QColor>("foreground");
        QTest::newRow("light-mode") << QColor(QStringLiteral("#232629"));
        QTest::newRow("dark-mode") << QColor(QStringLiteral("#eff0f1"));
    }

    void followsPaletteColor()
    {
        QFETCH(QColor, foreground);
        const QImage image = renderSymbolicIcon(QStringLiteral(DWARFSCTL_TEST_ICON),
                                                foreground,
                                                QSize(88, 88));
        QVERIFY(!image.isNull());

        int opaquePixels = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() < 250) {
                    continue;
                }
                ++opaquePixels;
                QCOMPARE(pixel.red(), foreground.red());
                QCOMPARE(pixel.green(), foreground.green());
                QCOMPARE(pixel.blue(), foreground.blue());
            }
        }
        QVERIFY(opaquePixels > 500);
    }
};

QTEST_MAIN(SymbolicIconTest)
#include "tst_symbolic_icon.moc"
