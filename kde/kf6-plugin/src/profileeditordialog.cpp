// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "profileeditordialog.h"

#include "bannerwidget.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QLineEdit>
#include <QSpinBox>
#include <QStackedWidget>
#include <QScrollArea>
#include <QSignalBlocker>
#include <optional>
#include <QVBoxLayout>

#include <KLocalizedString>

#include "dwarfsctljson.h"
#include "dwarfsctlrunner.h"
#include "logging.h"

namespace dwarfsplugin {

namespace {

using OptionDef = MkdwarfsOptionDef;

struct CategoryDef {
    QString name;
    QList<MkdwarfsOptionDef> opts;
};

static bool runDwarfsctlJson(QWidget *parent, const QStringList &args, Envelope *outEnv, QString *outErr);

/// Map a schema `kind` string to an editor kind.
///
/// Returns `std::nullopt` for an unrecognised kind rather than guessing. Guessing
/// meant a schema that grew a new kind (or a typo in the CLI's schema output)
/// rendered as a plain text field, and the resulting value would be passed back
/// as argv for an option the editor never really understood. Callers skip
/// unrecognised options and surface them, so schema evolution fails visibly.
static std::optional<MkdwarfsOptionDef::Kind> kindFromString(const QString &s)
{
    using K = MkdwarfsOptionDef::Kind;
    if (s == QStringLiteral("Flag")) {
        return K::Flag;
    }
    if (s == QStringLiteral("Int")) {
        return K::Int;
    }
    if (s == QStringLiteral("Double")) {
        return K::Double;
    }
    if (s == QStringLiteral("String")) {
        return K::String;
    }
    if (s == QStringLiteral("Enum")) {
        return K::Enum;
    }
    if (s == QStringLiteral("MultiString")) {
        return K::MultiString;
    }
    if (s == QStringLiteral("OptionalEnum")) {
        return K::OptionalEnum;
    }
    if (s == QStringLiteral("OptionalString")) {
        return K::OptionalString;
    }
    if (s == QStringLiteral("CompressionSpec")) {
        return K::CompressionSpec;
    }
    return std::nullopt;
}

static QList<CategoryDef> loadSchemaFromDwarfsctl(QWidget *parent, QString *outErr)
{
    Envelope env;
    QString err;
    if (!runDwarfsctlJson(parent, {QStringLiteral("mkdwarfs"), QStringLiteral("schema")}, &env, &err)) {
        if (outErr) {
            *outErr = err;
        }
        return {};
    }

    const auto schemaVal = env.details.value(QStringLiteral("schema"));
    if (!schemaVal.isObject()) {
        if (outErr) {
            *outErr = i18n("dwarfsctl returned an invalid schema payload (missing 'schema' object)");
        }
        return {};
    }

    const QJsonObject schemaObj = schemaVal.toObject();
    const QJsonArray categories = schemaObj.value(QStringLiteral("categories")).toArray();

    QList<CategoryDef> cats;
    cats.reserve(categories.size());
    for (const auto &catVal : categories) {
        if (!catVal.isObject()) {
            continue;
        }
        const QJsonObject catObj = catVal.toObject();
        CategoryDef cat;
        cat.name = catObj.value(QStringLiteral("name")).toString();
        const QJsonArray options = catObj.value(QStringLiteral("options")).toArray();
        for (const auto &optVal : options) {
            if (!optVal.isObject()) {
                continue;
            }
            const QJsonObject o = optVal.toObject();
            MkdwarfsOptionDef def;
            def.opt = o.value(QStringLiteral("opt")).toString();
            def.label = o.value(QStringLiteral("label")).toString();

            // An unrecognised kind means the schema is newer (or malformed) than
            // this dialog understands. Skip the option and tell the user rather
            // than rendering it as a plain string, which would let a value be
            // written back for an option whose semantics we do not know.
            const auto kindStr = o.value(QStringLiteral("kind")).toString();
            const auto kind = kindFromString(kindStr);
            if (!kind.has_value()) {
                if (outErr && outErr->isEmpty()) {
                    *outErr = i18n(
                        "mkdwarfs option '%1' has an unrecognised kind '%2'; "
                        "it is omitted from this dialog. The installed mkdwarfs "
                        "may be newer than this plugin.",
                        def.opt,
                        kindStr);
                }
                continue;
            }
            def.kind = *kind;

            const auto dv = o.value(QStringLiteral("default"));
            if (!dv.isNull() && !dv.isUndefined()) {
                def.defaultValue = dv.toVariant();
            }

            const auto enumArr = o.value(QStringLiteral("enum_values")).toArray();
            for (const auto &ev : enumArr) {
                def.enumValues.push_back(ev.toString());
            }

            const auto minInt = o.value(QStringLiteral("min_int"));
            if (!minInt.isNull() && !minInt.isUndefined()) {
                def.minInt = minInt.toInt(def.minInt);
            }
            const auto maxInt = o.value(QStringLiteral("max_int"));
            if (!maxInt.isNull() && !maxInt.isUndefined()) {
                def.maxInt = maxInt.toInt(def.maxInt);
            }
            const auto minD = o.value(QStringLiteral("min_double"));
            if (!minD.isNull() && !minD.isUndefined()) {
                def.minDouble = minD.toDouble(def.minDouble);
            }
            const auto maxD = o.value(QStringLiteral("max_double"));
            if (!maxD.isNull() && !maxD.isUndefined()) {
                def.maxDouble = maxD.toDouble(def.maxDouble);
            }

            def.repeatable = o.value(QStringLiteral("repeatable")).toBool(false);
            def.optionalValue = o.value(QStringLiteral("optional_value")).toBool(false);
            def.simpleControl = o.value(QStringLiteral("simple_control")).toString();

            if (!def.opt.isEmpty()) {
                cat.opts.push_back(def);
            }
        }
        if (!cat.name.isEmpty() && !cat.opts.isEmpty()) {
            cats.push_back(cat);
        }
    }

    return cats;
}

static bool runDwarfsctlJson(QWidget *parent, const QStringList &args, Envelope *outEnv, QString *outErr)
{
    const auto rr = DwarfsctlRunner::run(args, DwarfsctlRunner::defaultTimeoutMs());
    if (rr.crashed) {
        if (outErr)
            *outErr = i18n("dwarfsctl crashed");
        return false;
    }
    const auto pr = parseEnvelope(rr.stdout_bytes);
    if (!pr.parsed) {
        if (outErr)
            *outErr = i18n("Failed to parse dwarfsctl JSON: %1", pr.error_message);
        return false;
    }
    if (!pr.envelope.ok) {
        if (outErr)
            *outErr = pr.envelope.has_error ? pr.envelope.error.message : i18n("dwarfsctl failed");
        return false;
    }
    if (outEnv)
        *outEnv = pr.envelope;
    Q_UNUSED(parent);
    return true;
}

static QStringList splitLines(const QString &s)
{
    QStringList out;
    for (const auto &line : s.split(QRegularExpression(QStringLiteral("\\r?\\n")), Qt::SkipEmptyParts)) {
        const auto t = line.trimmed();
        if (!t.isEmpty())
            out << t;
    }
    return out;
}

static QString joinLines(const QStringList &lines)
{
    return lines.join(QLatin1Char('\n'));
}

} // namespace

ProfileEditorDialog::ProfileEditorDialog(QWidget *parent, const QString &initialProfileName)
    : QDialog(parent)
    , m_initialName(initialProfileName.trimmed())
{
    setWindowTitle(i18n("Compression Profile"));
    setModal(true);
    setMinimumSize(860, 560);

    buildUi();
    loadProfileArgs(m_initialName.isEmpty() ? QStringLiteral("default") : m_initialName);
}

void ProfileEditorDialog::showWarningBanner(const QString &title, const QString &body)
{
    if (!m_banner) {
        return;
    }

    m_banner->setSeverity(BannerWidget::Severity::Warning);
    m_banner->setTitleAndBody(title, body);
    m_banner->setDetails({});
    m_banner->setVisible(true);
    if (m_bannerDismiss) {
        m_bannerDismiss->setVisible(true);
    }
}

void ProfileEditorDialog::showInfoBanner(const QString &title, const QString &body)
{
    if (!m_banner) {
        return;
    }

    m_banner->setSeverity(BannerWidget::Severity::Info);
    m_banner->setTitleAndBody(title, body);
    m_banner->setDetails({});
    m_banner->setVisible(true);
    if (m_bannerDismiss) {
        m_bannerDismiss->setVisible(true);
    }
}

void ProfileEditorDialog::buildUi()
{
    using OptionDef = MkdwarfsOptionDef;

    auto *root = new QVBoxLayout(this);

    m_banner = new BannerWidget(this);
    m_banner->setSeverity(BannerWidget::Severity::Warning);
    m_bannerDismiss = m_banner->addActionButton(i18n("Dismiss"));
    QObject::connect(m_bannerDismiss, &QAction::triggered, this, [this]() {
        if (m_banner)
            m_banner->setVisible(false);
    });
    root->addWidget(m_banner);

    auto *intro = new QLabel(
        i18n("Choose a practical starting point, then adjust exact DwarFS settings only when you need them."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *nameRow = new QWidget(this);
    auto *nameLayout = new QHBoxLayout(nameRow);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->addWidget(new QLabel(i18n("Profile name"), nameRow));
    m_nameEdit = new QLineEdit(nameRow);
    m_nameEdit->setObjectName(QStringLiteral("profileName"));
    m_nameEdit->setPlaceholderText(i18n("e.g. media, fast, archival"));
    nameLayout->addWidget(m_nameEdit, 1);
    root->addWidget(nameRow);

    // The practical starting point is identified by presentation metadata in
    // dwarfsctl's schema; the KDE UI does not duplicate the option inventory.
    auto *compressionGroup = new QGroupBox(i18n("Starting point"), this);
    auto *compressionLayout = new QVBoxLayout(compressionGroup);
    auto *levelLabel = new QLabel(i18n("Priority"), compressionGroup);
    compressionLayout->addWidget(levelLabel);
    m_simpleLevel = new QComboBox(compressionGroup);
    m_simpleLevel->setObjectName(QStringLiteral("simpleCompressionLevel"));
    m_simpleLevel->setAccessibleName(i18n("Compression priority"));
    compressionLayout->addWidget(m_simpleLevel);
    auto *levelHelp = new QLabel(
        i18n("Balanced works well for most folders. Choose faster creation when time matters, or a smaller image when storage matters."),
        compressionGroup);
    levelHelp->setWordWrap(true);
    compressionLayout->addWidget(levelHelp);
    m_simpleSummary = new QLabel(compressionGroup);
    m_simpleSummary->setObjectName(QStringLiteral("exactSettingsSummary"));
    m_simpleSummary->setWordWrap(true);
    compressionLayout->addWidget(m_simpleSummary);
    root->addWidget(compressionGroup);

    auto *exactGroup = new QGroupBox(i18n("Exact DwarFS settings"), this);
    exactGroup->setObjectName(QStringLiteral("exactProfileSettings"));
    auto *advancedLayout = new QVBoxLayout(exactGroup);
    auto *expertHint = new QLabel(
        i18n("These controls map directly to the bundled DwarFS writer. Only enabled settings are stored."),
        exactGroup);
    expertHint->setWordWrap(true);
    advancedLayout->addWidget(expertHint);

    auto *split = new QWidget(exactGroup);
    auto *splitLayout = new QHBoxLayout(split);
    splitLayout->setContentsMargins(0, 0, 0, 0);
    m_nav = new QListWidget(split);
    m_nav->setObjectName(QStringLiteral("advancedOptionsNavigation"));
    m_nav->setMinimumWidth(220);
    splitLayout->addWidget(m_nav);
    auto *stack = new QStackedWidget(split);
    stack->setObjectName(QStringLiteral("advancedOptionsStack"));
    splitLayout->addWidget(stack, 1);
    advancedLayout->addWidget(split, 1);

    QString schemaErr;
    const auto cats = loadSchemaFromDwarfsctl(this, &schemaErr);
    if (cats.isEmpty()) {
        QMessageBox::critical(this,
                              i18n("Profile editor unavailable"),
                              schemaErr.isEmpty() ? i18n("Unable to load mkdwarfs option schema from dwarfsctl.") : schemaErr);
        reject();
        return;
    }

    for (const auto &cat : cats) {
        m_nav->addItem(cat.name);
        auto *page = new QWidget(stack);
        auto *pageLayout = new QVBoxLayout(page);
        auto *form = new QVBoxLayout();
        form->setContentsMargins(0, 0, 0, 0);

        for (const auto &od : cat.opts) {
            MkdwarfsOptionUi ui;
            ui.def = od;
            ui.cb = new QCheckBox(QStringLiteral("%1 (%2)").arg(od.label, od.opt), page);
            ui.cb->setObjectName(QStringLiteral("option_%1").arg(od.opt.mid(2).replace(QLatin1Char('-'), QLatin1Char('_'))));

            if (od.kind == OptionDef::Kind::Flag) {
                form->addWidget(ui.cb);
            } else {
                QWidget *editor = nullptr;
                switch (od.kind) {
                case OptionDef::Kind::Int: {
                    auto *sb = new QSpinBox(page);
                    sb->setRange(od.minInt, od.maxInt);
                    sb->setValue(od.defaultValue.isValid() ? od.defaultValue.toInt() : 0);
                    editor = sb;
                    break;
                }
                case OptionDef::Kind::Double: {
                    auto *dsb = new QDoubleSpinBox(page);
                    dsb->setRange(od.minDouble, od.maxDouble);
                    dsb->setDecimals(4);
                    dsb->setValue(od.defaultValue.isValid() ? od.defaultValue.toDouble() : 0.0);
                    editor = dsb;
                    break;
                }
                case OptionDef::Kind::Enum:
                case OptionDef::Kind::OptionalEnum: {
                    auto *cbx = new QComboBox(page);
                    cbx->addItems(od.enumValues);
                    if (od.defaultValue.isValid())
                        cbx->setCurrentText(od.defaultValue.toString());
                    editor = cbx;
                    break;
                }
                case OptionDef::Kind::MultiString: {
                    auto *pte = new QPlainTextEdit(page);
                    pte->setPlaceholderText(i18n("One entry per line"));
                    pte->setLineWrapMode(QPlainTextEdit::WidgetWidth);
                    pte->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                    editor = pte;
                    break;
                }
                case OptionDef::Kind::CompressionSpec:
                case OptionDef::Kind::String:
                case OptionDef::Kind::OptionalString: {
                    auto *le = new QLineEdit(page);
                    if (od.defaultValue.isValid())
                        le->setText(od.defaultValue.toString());
                    editor = le;
                    break;
                }
                default:
                    break;
                }

                ui.editor = editor;
                editor->setObjectName(QStringLiteral("editor_%1").arg(od.opt.mid(2).replace(QLatin1Char('-'), QLatin1Char('_'))));
                auto *row = new QWidget(page);
                auto *rowLayout = new QVBoxLayout(row);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                rowLayout->addWidget(ui.cb);
                rowLayout->addWidget(editor);
                form->addWidget(row);
                editor->setEnabled(false);
            }

            const int optionIndex = m_options.size();
            m_options.push_back(ui);
            if (od.simpleControl == QStringLiteral("compression-priority"))
                m_simpleOptionIndex = optionIndex;

            QObject::connect(ui.cb, &QCheckBox::toggled, this, [this, editor = ui.editor](bool checked) {
                if (editor)
                    editor->setEnabled(checked);
                updatePreview();
            });
            if (ui.editor) {
                if (auto *le = qobject_cast<QLineEdit *>(ui.editor)) {
                    QObject::connect(le, &QLineEdit::textChanged, this, [this]() { updatePreview(); });
                } else if (auto *cbx = qobject_cast<QComboBox *>(ui.editor)) {
                    QObject::connect(cbx, &QComboBox::currentTextChanged, this, [this]() { updatePreview(); });
                } else if (auto *sb = qobject_cast<QSpinBox *>(ui.editor)) {
                    QObject::connect(sb, qOverload<int>(&QSpinBox::valueChanged), this, [this]() { updatePreview(); });
                } else if (auto *dsb = qobject_cast<QDoubleSpinBox *>(ui.editor)) {
                    QObject::connect(dsb, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this]() { updatePreview(); });
                } else if (auto *pte = qobject_cast<QPlainTextEdit *>(ui.editor)) {
                    QObject::connect(pte, &QPlainTextEdit::textChanged, this, [this]() { updatePreview(); });
                }
            }
        }

        pageLayout->addLayout(form);
        pageLayout->addStretch(1);
        auto *scroll = new QScrollArea(split);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        stack->addWidget(scroll);
    }

    QObject::connect(m_nav, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);

    auto *prevLabel = new QLabel(i18n("Profile arguments"), exactGroup);
    advancedLayout->addWidget(prevLabel);
    m_preview = new QPlainTextEdit(exactGroup);
    m_preview->setObjectName(QStringLiteral("profileArgumentsPreview"));
    m_preview->setReadOnly(true);
    m_preview->setMaximumBlockCount(4096);
    m_preview->setMaximumHeight(120);
    m_preview->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_preview->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    advancedLayout->addWidget(m_preview);
    root->addWidget(exactGroup, 1);

    if (m_simpleOptionIndex >= 0) {
        const auto &def = m_options[m_simpleOptionIndex].def;
        m_simpleLevel->addItem(i18n("Balanced (recommended)"), QVariant());
        m_simpleLevel->addItem(i18n("Faster creation"), def.minInt);
        m_simpleLevel->addItem(i18n("Smaller image"), def.maxInt);
        m_simpleLevel->addItem(i18n("Custom settings (shown below)"), QStringLiteral("__custom__"));
    } else {
        m_simpleLevel->addItem(i18n("Unavailable with this DwarFS schema"));
        m_simpleLevel->setEnabled(false);
        if (schemaErr.isEmpty())
            schemaErr = i18n("The installed DwarFS schema does not identify a practical starting point. Exact settings remain available below.");
    }

    QObject::connect(m_simpleLevel, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
        applySimpleSelection();
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
    buttons->button(QDialogButtonBox::Ok)->setText(i18n("Save"));
    auto *resetBtn = buttons->addButton(i18n("Reset"), QDialogButtonBox::ResetRole);
    auto *deleteBtn = buttons->addButton(i18n("Delete"), QDialogButtonBox::DestructiveRole);
    root->addWidget(buttons);

    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        const QString name = m_nameEdit->text().trimmed();
        if (name.isEmpty()) {
            showWarningBanner(i18n("Save profile"), i18n("Profile name is required."));
            return;
        }
        if (name == QStringLiteral("default")) {
            showWarningBanner(i18n("Save profile"), i18n("'default' is reserved and cannot be modified."));
            return;
        }
        const auto args = buildArgsFromUi();
        QString err;
        if (!validateWithDwarfsctl(args, &err)) {
            QMessageBox::critical(this, i18n("Invalid profile"), err);
            return;
        }
        if (!saveProfile(name, args, &err)) {
            QMessageBox::critical(this, i18n("Save profile"), err);
            return;
        }
        m_result.saved = true;
        m_result.profileName = name;
        m_result.profileArgs = args;
        accept();
    });
    QObject::connect(resetBtn, &QPushButton::clicked, this, [this]() { setUiFromArgs({}); });
    QObject::connect(deleteBtn, &QPushButton::clicked, this, [this]() {
        const QString name = m_nameEdit->text().trimmed();
        if (name.isEmpty() || name == QStringLiteral("default")) {
            showWarningBanner(i18n("Delete profile"), i18n("Select a non-default profile to delete."));
            return;
        }
        if (QMessageBox::question(this, i18n("Delete profile"), i18n("Delete profile '%1'?", name)) != QMessageBox::Yes)
            return;
        QString err;
        if (!deleteProfile(name, &err)) {
            QMessageBox::critical(this, i18n("Delete profile"), err);
            return;
        }
        m_nameEdit->setText(QString());
        setUiFromArgs({});
        showInfoBanner(i18n("Delete profile"), i18n("Profile deleted."));
    });
    QObject::connect(m_nameEdit, &QLineEdit::textChanged, this, [this]() { updatePreview(); });

    if (!schemaErr.isEmpty()) {
        showWarningBanner(i18n("Some settings are unavailable"), schemaErr);
    }
}

void ProfileEditorDialog::loadProfileArgs(const QString &profileName)
{
    if (profileName.isEmpty() || profileName == QStringLiteral("default")) {
        m_nameEdit->setText(profileName == QStringLiteral("default") ? QString() : profileName);
        setUiFromArgs({});
        updatePreview();
        return;
    }

    // Load args from dwarfsctl
    Envelope env;
    QString err;
    if (!runDwarfsctlJson(this,
                          {QStringLiteral("config"), QStringLiteral("mkdwarfs-profiles"), QStringLiteral("get"), profileName},
                          &env,
                          &err)) {
        QMessageBox::critical(this, i18n("Load profile"), err);
        m_nameEdit->setText(profileName);
        setUiFromArgs({});
        updatePreview();
        return;
    }
    const auto prof = env.details.value(QStringLiteral("profile")).toObject();
    const auto argsArr = prof.value(QStringLiteral("args")).toArray();
    QStringList args;
    for (const auto &v : argsArr) {
        const auto s = v.toString();
        if (!s.isEmpty())
            args << s;
    }

    m_nameEdit->setText(profileName);
    setUiFromArgs(args);
    updatePreview();
}

void ProfileEditorDialog::setUiFromArgs(const QStringList &args)
{
    // Clear all
    m_repeatableValues.clear();
    for (auto &ui : m_options) {
        if (ui.cb)
            ui.cb->setChecked(false);
        if (!ui.editor)
            continue;

        switch (ui.def.kind) {
        case OptionDef::Kind::Int: {
            if (auto *sb = qobject_cast<QSpinBox *>(ui.editor)) {
                sb->setValue(ui.def.defaultValue.isValid() ? ui.def.defaultValue.toInt() : 0);
            }
            break;
        }
        case OptionDef::Kind::Double: {
            if (auto *dsb = qobject_cast<QDoubleSpinBox *>(ui.editor)) {
                dsb->setValue(ui.def.defaultValue.isValid() ? ui.def.defaultValue.toDouble() : 0.0);
            }
            break;
        }
        case OptionDef::Kind::Enum:
        case OptionDef::Kind::OptionalEnum: {
            if (auto *cbx = qobject_cast<QComboBox *>(ui.editor)) {
                if (ui.def.defaultValue.isValid())
                    cbx->setCurrentText(ui.def.defaultValue.toString());
            }
            break;
        }
        case OptionDef::Kind::MultiString: {
            if (auto *pte = qobject_cast<QPlainTextEdit *>(ui.editor)) {
                pte->setPlainText(QString());
            }
            break;
        }
        default: {
            if (auto *le = qobject_cast<QLineEdit *>(ui.editor)) {
                le->setText(ui.def.defaultValue.isValid() ? ui.def.defaultValue.toString() : QString());
            }
            break;
        }
        }
    }
    m_extraArgs.clear();

    // Parse tokens.
    // This is intentionally simple: the schema is explicit; unknown tokens go into m_extraArgs.
    auto optIndex = [this](const QString &tok) -> int {
        for (int idx = 0; idx < m_options.size(); ++idx) {
            if (m_options[idx].def.opt == tok) {
                return idx;
            }
        }
        return -1;
    };

    for (int i = 0; i < args.size(); ++i) {
        const QString tok = args[i];

        QString opt = tok;
        QString val;
        bool hasInline = false;
        const int eq = tok.indexOf(QLatin1Char('='));
        if (eq > 0) {
            opt = tok.left(eq);
            val = tok.mid(eq + 1);
            hasInline = true;
        }

        const int idx = optIndex(opt);
        if (idx < 0) {
            m_extraArgs << tok;
            continue;
        }

        auto &ui = m_options[idx];
        ui.cb->setChecked(true);

        if (ui.def.kind == OptionDef::Kind::Flag) {
            continue;
        }

        // A repeatable option may appear several times, and every occurrence
        // matters: mkdwarfs accepts `--filter a --filter b` and the schema marks
        // those options repeatable. Collect each value and append, so a profile
        // opened and saved is not silently reduced to its last occurrence.
        const bool repeatable = ui.def.repeatable;

        // Determine value (inline or next token). A leading minus does not by
        // itself make a token an option: several supported numeric values are
        // negative, including the default incompressible ZSTD level.
        QString effective = val;
        if (!hasInline && i + 1 < args.size()) {
            const QString next = args[i + 1];
            const QString nextOpt = next.section(QLatin1Char('='), 0, 0);
            const bool knownOption = optIndex(nextOpt) >= 0;
            const bool unknownLongOption = next.startsWith(QStringLiteral("--"));
            if (!knownOption && !unknownLongOption) {
                effective = next;
                i += 1;
            }
        }

        if (ui.def.kind == OptionDef::Kind::MultiString) {
            // Append to multi-line list.
            auto *pte = qobject_cast<QPlainTextEdit *>(ui.editor);
            QStringList existing = splitLines(pte->toPlainText());
            if (!effective.isEmpty())
                existing << effective;
            pte->setPlainText(joinLines(existing));
            continue;
        }

        if (repeatable) {
            // Scalar editors hold one value, so repeated occurrences are kept in
            // a side list and rendered/re-emitted as a unit. Without this the
            // earlier values would be overwritten by the last one seen.
            if (!effective.isEmpty()) {
                auto &acc = m_repeatableValues[idx];
                acc << effective;
            }
            continue;
        }

        if (auto *le = qobject_cast<QLineEdit *>(ui.editor)) {
            if (!effective.isEmpty())
                le->setText(effective);
        } else if (auto *cbx = qobject_cast<QComboBox *>(ui.editor)) {
            if (!effective.isEmpty())
                cbx->setCurrentText(effective);
        } else if (auto *sb = qobject_cast<QSpinBox *>(ui.editor)) {
            if (!effective.isEmpty())
                sb->setValue(effective.toInt());
        } else if (auto *dsb = qobject_cast<QDoubleSpinBox *>(ui.editor)) {
            if (!effective.isEmpty())
                dsb->setValue(effective.toDouble());
        }
    }

    // Show accumulated repeatable values so the user can see and edit them.
    for (auto it = m_repeatableValues.constBegin(); it != m_repeatableValues.constEnd(); ++it) {
        const int idx = it.key();
        if (idx < 0 || idx >= m_options.size())
            continue;
        auto *le = qobject_cast<QLineEdit *>(m_options[idx].editor);
        if (le)
            le->setText(it.value().join(QLatin1Char(' ')));
    }
    syncSimpleFromOptions();
    updateSimpleSummary();
}

QStringList ProfileEditorDialog::buildArgsFromUi() const
{
    QStringList args;

    // Indexed rather than range-based: repeatable values are keyed by option
    // index, and MkdwarfsOptionUi has no operator== to search with.
    for (int optIdx = 0; optIdx < m_options.size(); ++optIdx) {
        const auto &ui = m_options[optIdx];
        if (!ui.cb || !ui.cb->isChecked()) {
            continue;
        }

        const auto opt = ui.def.opt;

        if (ui.def.kind == OptionDef::Kind::Flag) {
            args << opt;
            continue;
        }

        if (ui.def.kind == OptionDef::Kind::MultiString) {
            auto *pte = qobject_cast<QPlainTextEdit *>(ui.editor);
            const QStringList lines = splitLines(pte->toPlainText());
            if (lines.isEmpty()) {
                continue;
            }
            // Drop default-only (single-line) entries.
            if (lines.size() == 1 && ui.def.defaultValue.isValid() && lines[0] == ui.def.defaultValue.toString() && !ui.def.optionalValue) {
                continue;
            }
            for (const auto &l : lines) {
                args << QStringLiteral("%1=%2").arg(opt, l);
            }
            continue;
        }

        // Non-repeatable values.
        QString value;
        if (auto *le = qobject_cast<QLineEdit *>(ui.editor)) {
            value = le->text().trimmed();
        } else if (auto *cbx = qobject_cast<QComboBox *>(ui.editor)) {
            value = cbx->currentText().trimmed();
        } else if (auto *sb = qobject_cast<QSpinBox *>(ui.editor)) {
            value = QString::number(sb->value());
        } else if (auto *dsb = qobject_cast<QDoubleSpinBox *>(ui.editor)) {
            value = QString::number(dsb->value(), 'g', 12);
        }

        // A repeatable option is written once per value. The editor presents the
        // accumulated values space-separated, so split on whitespace to recover
        // the individual occurrences the user intends.
        if (ui.def.repeatable) {
            const QStringList parts = value.split(QRegularExpression(QStringLiteral("\\s+")),
                                                  Qt::SkipEmptyParts);
            if (parts.isEmpty()) {
                // Nothing entered: fall back to whatever was loaded, so opening
                // and saving a profile does not drop values the editor could not
                // display.
                const auto loaded = m_repeatableValues.value(optIdx);
                for (const auto &v : loaded)
                    args << QStringLiteral("%1=%2").arg(opt, v);
                continue;
            }
            for (const auto &p : parts)
                args << QStringLiteral("%1=%2").arg(opt, p);
            continue;
        }

        if (!ui.def.optionalValue) {
            // Omit defaults.
            if (ui.def.defaultValue.isValid() && value == ui.def.defaultValue.toString()) {
                continue;
            }
            if (value.isEmpty()) {
                continue;
            }
            args << QStringLiteral("%1=%2").arg(opt, value);
        } else {
            // Presence is the modification. If value equals default (or is blank), emit flag-only.
            const QString def = ui.def.defaultValue.isValid() ? ui.def.defaultValue.toString() : QString();
            if (value.isEmpty() || (!def.isEmpty() && value == def)) {
                args << opt;
            } else {
                args << QStringLiteral("%1=%2").arg(opt, value);
            }
        }
    }

    for (const auto &a : m_extraArgs) {
        const auto t = a.trimmed();
        if (!t.isEmpty())
            args << t;
    }

    return args;
}

bool ProfileEditorDialog::validateWithDwarfsctl(const QStringList &args, QString *err) const
{
    QStringList cmd;
    cmd << QStringLiteral("config") << QStringLiteral("mkdwarfs-profiles") << QStringLiteral("validate");
    for (const auto &a : args) {
        cmd << QStringLiteral("--arg") << a;
    }

    Envelope env;
    QString e;
    if (!runDwarfsctlJson(const_cast<ProfileEditorDialog *>(this), cmd, &env, &e)) {
        if (err)
            *err = e;
        return false;
    }
    if (err)
        *err = QString();
    Q_UNUSED(env);
    return true;
}

bool ProfileEditorDialog::saveProfile(const QString &name, const QStringList &args, QString *err)
{
    QStringList cmd;
    cmd << QStringLiteral("config") << QStringLiteral("mkdwarfs-profiles") << QStringLiteral("set") << name;
    for (const auto &a : args) {
        cmd << QStringLiteral("--arg") << a;
    }

    Envelope env;
    QString e;
    if (!runDwarfsctlJson(this, cmd, &env, &e)) {
        if (err)
            *err = e;
        return false;
    }
    if (err)
        *err = QString();
    Q_UNUSED(env);
    return true;
}

bool ProfileEditorDialog::deleteProfile(const QString &name, QString *err)
{
    QStringList cmd;
    cmd << QStringLiteral("config") << QStringLiteral("mkdwarfs-profiles") << QStringLiteral("delete") << name;

    Envelope env;
    QString e;
    if (!runDwarfsctlJson(this, cmd, &env, &e)) {
        if (err)
            *err = e;
        return false;
    }
    if (err)
        *err = QString();
    Q_UNUSED(env);
    return true;
}

void ProfileEditorDialog::applySimpleSelection()
{
    if (m_syncingSimpleControls || m_simpleOptionIndex < 0 || !m_simpleLevel)
        return;

    auto &ui = m_options[m_simpleOptionIndex];
    auto *spin = qobject_cast<QSpinBox *>(ui.editor);
    if (!ui.cb || !spin)
        return;

    const QVariant selected = m_simpleLevel->currentData();
    if (selected.toString() == QStringLiteral("__custom__"))
        return;
    ui.cb->setChecked(selected.isValid());
    if (selected.isValid())
        spin->setValue(selected.toInt());
    updatePreview();
}

void ProfileEditorDialog::syncSimpleFromOptions()
{
    if (!m_simpleLevel || m_simpleOptionIndex < 0 || m_syncingSimpleControls)
        return;

    const auto &ui = m_options[m_simpleOptionIndex];
    const auto *spin = qobject_cast<QSpinBox *>(ui.editor);
    if (!ui.cb || !spin)
        return;

    m_syncingSimpleControls = true;
    const QSignalBlocker blocker(m_simpleLevel);
    int comboIndex = 0;
    if (ui.cb->isChecked()) {
        const int candidate = m_simpleLevel->findData(spin->value());
        if (candidate >= 0)
            comboIndex = candidate;
        else
            comboIndex = m_simpleLevel->findData(QStringLiteral("__custom__"));
    }
    m_simpleLevel->setCurrentIndex(comboIndex);
    m_syncingSimpleControls = false;
}

void ProfileEditorDialog::updateSimpleSummary()
{
    if (!m_simpleSummary)
        return;

    int advancedSettings = 0;
    for (int i = 0; i < m_options.size(); ++i) {
        if (i != m_simpleOptionIndex && m_options[i].cb && m_options[i].cb->isChecked())
            ++advancedSettings;
    }
    if (m_simpleOptionIndex >= 0 && m_options[m_simpleOptionIndex].cb
        && m_options[m_simpleOptionIndex].cb->isChecked()
        && m_simpleLevel->currentData().toString() == QStringLiteral("__custom__")) {
        ++advancedSettings;
    }
    advancedSettings += m_extraArgs.size();

    if (advancedSettings == 0) {
        m_simpleSummary->setText(
            i18n("Everything else will follow the bundled DwarFS defaults."));
        return;
    }

    m_simpleSummary->setText(i18np(
        "This profile also uses one exact setting shown below.",
        "This profile also uses %1 exact settings shown below.",
        advancedSettings));
}

void ProfileEditorDialog::updatePreview()
{
    const auto args = buildArgsFromUi();
    if (!m_preview) {
        return;
    }
    m_preview->setPlainText(args.join(QLatin1Char('\n')));
    syncSimpleFromOptions();
    updateSimpleSummary();
}

} // namespace dwarfsplugin
