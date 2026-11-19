// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCheckBox>
#include <QDialog>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QWidget>

class QListWidget;
class QPlainTextEdit;
class QLineEdit;
class QComboBox;
class QLabel;
class QStackedWidget;
class QAction;
class QGroupBox;
class QDialogButtonBox;
class QPushButton;
class QJsonObject;

namespace dwarfsplugin {

class BannerWidget;

struct MkdwarfsOptionDef {
    enum class Kind {
        Flag,
        Int,
        Double,
        String,
        Enum,
        MultiString,
        OptionalEnum,
        OptionalString,
        CompressionSpec,
    };

    QString opt;
    QString label;
    Kind kind = Kind::String;
    QVariant defaultValue;
    QStringList enumValues;
    int minInt = -2147483648;
    int maxInt = 2147483647;
    double minDouble = -1e9;
    double maxDouble = 1e9;
    bool repeatable = false;
    bool optionalValue = false;
    QString simpleControl;
};

struct MkdwarfsOptionUi {
    MkdwarfsOptionDef def;
    QCheckBox *cb = nullptr;
    QWidget *editor = nullptr;
};

class ProfileEditorDialog : public QDialog {
    Q_OBJECT

public:
    struct Result {
        bool saved = false;
        QString profileName;
        QStringList profileArgs;
    };

    explicit ProfileEditorDialog(QWidget *parent, const QString &initialProfileName);
    Result result() const { return m_result; }

private:
    void showWarningBanner(const QString &title, const QString &body);
    void showInfoBanner(const QString &title, const QString &body);
    void buildUi();
    bool populateSchema(const QJsonObject &schema);
    void loadPrerequisites();
    void loadFailed(const QString &error);
    void updateAvailability();
    void saveEdits(const QString &name, const QStringList &args);
    void deleteEdits(const QString &name);
    void mutationUnconfirmed(const QString &name, const QStringList &args, bool deleting, const QString &error);
    void reconcileMutation();
    void loadProfileArgs(const QString &profileName);
    void setUiFromArgs(const QStringList &args);
    QStringList buildArgsFromUi() const;
    void updatePreview();
    void applySimpleSelection();
    void syncSimpleFromOptions();
    void updateSimpleSummary();

    QString m_initialName;
    Result m_result;

    bool m_busy = false;
    bool m_ready = false;
    bool m_reconcile = false;
    bool m_pendingDelete = false;
    QString m_pendingName;
    QStringList m_pendingArgs;
    QString m_schemaWarning;
    QGroupBox *m_compressionGroup = nullptr;
    QGroupBox *m_exactGroup = nullptr;
    QWidget *m_split = nullptr;
    QStackedWidget *m_stack = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    QPushButton *m_reset = nullptr;
    QPushButton *m_delete = nullptr;
    QPushButton *m_retry = nullptr;
    QLabel *m_status = nullptr;
    QListWidget *m_nav = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QPlainTextEdit *m_preview = nullptr;
    QComboBox *m_simpleLevel = nullptr;
    QLabel *m_simpleSummary = nullptr;
    int m_simpleOptionIndex = -1;
    bool m_syncingSimpleControls = false;

    BannerWidget *m_banner = nullptr;
    QAction *m_bannerDismiss = nullptr;

    QList<MkdwarfsOptionUi> m_options;
    QStringList m_extraArgs;

    /// Values of repeatable options, keyed by index into m_options.
    ///
    /// A scalar editor can only display one value, so repeated occurrences
    /// (`--filter a --filter b`) are accumulated here while loading a profile and
    /// re-emitted one per occurrence when saving.
    QHash<int, QStringList> m_repeatableValues;
};

} // namespace dwarfsplugin
