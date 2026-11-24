// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest>
#include <QJsonDocument>
#include "recommendation.h"
class RecommendationTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void preservesBackendFlags() {
        const auto doc = QJsonDocument::fromJson(R"({"schema":"dwarfsctl.recommend.v1","recommendation":{"flags_only":["--example=value"],"reasons":["reason"],"notes":["method"],"warnings":["warning"],"confidence":"low"}})").object();
        const auto r = dwarfsplugin::parseRecommendation(doc);
        QVERIFY(r.valid);
        QCOMPARE(r.flags, QStringList{QStringLiteral("--example=value")});
        QCOMPARE(r.reasons, QStringList{QStringLiteral("reason")});
        QCOMPARE(r.notes, QStringList{QStringLiteral("method")});
        QCOMPARE(r.warnings, QStringList{QStringLiteral("warning")});
    }
    void classifiesLegacyHeuristicAsNote() {
        const auto doc = QJsonDocument::fromJson(R"({"schema":"dwarfsctl.recommend.v1","recommendation":{"flags_only":["--example=value"],"reasons":["reason"],"warnings":["Metadata heuristic only: no benchmark was run.","Some entries could not be inspected."],"confidence":"low"}})").object();
        const auto r = dwarfsplugin::parseRecommendation(doc);
        QVERIFY(r.valid);
        QCOMPARE(r.notes.size(), 1);
        QCOMPARE(r.warnings, QStringList{QStringLiteral("Some entries could not be inspected.")});
    }
    void refusesMalformedResults() {
        for (const auto &json : {"{}", R"({"schema":"dwarfsctl.recommend.v2"})", R"({"schema":"dwarfsctl.recommend.v1","recommendation":{"flags_only":[42],"reasons":[],"warnings":[],"confidence":"high"}})"}) {
            QVERIFY(!dwarfsplugin::parseRecommendation(QJsonDocument::fromJson(json).object()).valid);
        }
    }
};
QTEST_GUILESS_MAIN(RecommendationTest)
#include "tst_recommendation.moc"
