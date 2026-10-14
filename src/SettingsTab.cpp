#include "SettingsTab.h"

#include "I18n.h"
#include "Storage.h"
#include "Themes.h"
#include "UiHelpers.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

SettingsTab::SettingsTab(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
}

void SettingsTab::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    // --- general preferences ---
    auto *general = new QGroupBox(I18n::t(QStringLiteral("settings.title")));
    auto *form = new QFormLayout(general);

    // Language row: combo + "Restart now" (changes apply at the next start), with
    // a small hint below.
    m_language = new QComboBox;
    m_restart = new QPushButton(I18n::t(QStringLiteral("settings.restart_now")));
    m_languageHint = new QLabel(I18n::t(QStringLiteral("settings.applies_next_start")));
    m_languageHint->setEnabled(false);
    {
        QFont hintFont = m_languageHint->font();
        hintFont.setItalic(true);
        m_languageHint->setFont(hintFont);
    }
    auto *langRow = new QHBoxLayout;
    langRow->setContentsMargins(0, 0, 0, 0);
    langRow->addWidget(m_language);
    langRow->addWidget(m_restart);
    langRow->addStretch();
    auto *langCol = new QVBoxLayout;
    langCol->setContentsMargins(0, 0, 0, 0);
    langCol->setSpacing(2);
    langCol->addLayout(langRow);
    langCol->addWidget(m_languageHint);
    form->addRow(I18n::t(QStringLiteral("settings.language")), langCol);

    m_defaultTheme = new QComboBox;
    m_defaultLayer = new QComboBox;
    m_defaultLayer->addItem(I18n::t(QStringLiteral("settings.layer_top")), QStringLiteral("top"));
    m_defaultLayer->addItem(I18n::t(QStringLiteral("settings.layer_bottom")), QStringLiteral("bottom"));

    m_defaultWidth = new QSpinBox;
    m_defaultWidth->setRange(120, 2000);
    m_defaultWidth->setSuffix(QStringLiteral(" px"));
    m_defaultHeight = new QSpinBox;
    m_defaultHeight->setRange(100, 2000);
    m_defaultHeight->setSuffix(QStringLiteral(" px"));

    m_fontFamily = new QComboBox;
    m_fontFamily->setEditable(true);
    for (const QString &f : {QStringLiteral("DejaVu Sans"), QStringLiteral("Noto Sans"),
                             QStringLiteral("Ubuntu"), QStringLiteral("Comic Sans MS"),
                             QStringLiteral("Monospace")})
        m_fontFamily->addItem(f);
    m_fontSize = new QSpinBox;
    m_fontSize->setRange(7, 28);

    m_autostart = new QCheckBox(I18n::t(QStringLiteral("settings.autostart")));
    m_confirmDelete = new QCheckBox(I18n::t(QStringLiteral("settings.confirm_delete")));

    form->addRow(I18n::t(QStringLiteral("settings.default_theme")), m_defaultTheme);
    form->addRow(I18n::t(QStringLiteral("settings.default_position")), m_defaultLayer);

    auto *sizeRow = new QHBoxLayout;
    sizeRow->setContentsMargins(0, 0, 0, 0);
    sizeRow->addWidget(m_defaultWidth);
    sizeRow->addWidget(new QLabel(QStringLiteral("X")));
    sizeRow->addWidget(m_defaultHeight);
    sizeRow->addStretch();
    form->addRow(I18n::t(QStringLiteral("settings.new_note_size")), sizeRow);

    form->addRow(I18n::t(QStringLiteral("settings.note_font")), m_fontFamily);
    form->addRow(I18n::t(QStringLiteral("settings.font_size")), m_fontSize);

    // Date/time format used for new note titles and for the trash dates:
    // exposed as a setting to ease a future i18n.
    m_dateFormat = new QLineEdit;
    m_dateFormat->setPlaceholderText(UiHelpers::defaultDateFormat());
    m_dateFormat->setToolTip(I18n::t(QStringLiteral("settings.date_format_tooltip")));
    form->addRow(I18n::t(QStringLiteral("settings.date_format")), m_dateFormat);

    form->addRow(QString(), m_autostart);
    form->addRow(QString(), m_confirmDelete);

    auto *path = new QLabel(I18n::t(QStringLiteral("settings.data_path"))
                                .arg(Storage::instance().databasePath()));
    {
        QFont pathFont = path->font();
        pathFont.setItalic(true);
        path->setFont(pathFont);
    }
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QString(), path);

    // --- theme editor ---
    auto *themesBox = new QGroupBox(I18n::t(QStringLiteral("settings.themes_title")));
    auto *tl = new QHBoxLayout(themesBox);

    m_themeList = new QListWidget;
    m_themeList->setFixedWidth(220);
    tl->addWidget(m_themeList);

    auto *editor = new QFormLayout;
    m_themeName = new QLineEdit;
    m_themeBody = new QPushButton;
    m_themeTitle = new QPushButton;
    m_themeText = new QPushButton;
    for (QPushButton *b : {m_themeBody, m_themeTitle, m_themeText}) {
        b->setFixedHeight(26);
        b->setCursor(Qt::PointingHandCursor);
    }
    editor->addRow(I18n::t(QStringLiteral("settings.theme_name")), m_themeName);
    editor->addRow(I18n::t(QStringLiteral("settings.theme_body")), m_themeBody);
    editor->addRow(I18n::t(QStringLiteral("settings.theme_title_bar")), m_themeTitle);
    editor->addRow(I18n::t(QStringLiteral("settings.theme_text")), m_themeText);

    m_themeNew = UiHelpers::makeCardButton(I18n::t(QStringLiteral("settings.theme_new")));
    m_themeDelete = UiHelpers::makeCardButton(I18n::t(QStringLiteral("delete")));
    m_themeSave = UiHelpers::makeCardButton(I18n::t(QStringLiteral("settings.theme_save")));
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_themeNew);
    buttons->addWidget(m_themeDelete);
    buttons->addStretch();
    buttons->addWidget(m_themeSave);
    editor->addRow(QString(), buttons);

    tl->addLayout(editor, 1);

    root->addWidget(general);
    root->addWidget(themesBox, 1);

    // --- connections ---
    auto persist = [this] {
        Storage::instance().setSetting(QStringLiteral("language"), m_language->currentData().toString());
        Storage::instance().setSetting(QStringLiteral("default_theme"), m_defaultTheme->currentData().toString());
        Storage::instance().setSetting(QStringLiteral("default_layer"), m_defaultLayer->currentData().toString());
        Storage::instance().setSetting(QStringLiteral("default_width"), QString::number(m_defaultWidth->value()));
        Storage::instance().setSetting(QStringLiteral("default_height"), QString::number(m_defaultHeight->value()));
        Storage::instance().setSetting(QStringLiteral("font_family"), m_fontFamily->currentText());
        Storage::instance().setSetting(QStringLiteral("font_size"), QString::number(m_fontSize->value()));
        Storage::instance().setSetting(QStringLiteral("date_format"), m_dateFormat->text().trimmed());
        Storage::instance().setSetting(QStringLiteral("autostart"), m_autostart->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
        Storage::instance().setSetting(QStringLiteral("confirm_delete"), m_confirmDelete->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
        emit settingsChanged();
    };
    // currentIndexChanged does not fire when the user re-selects the already
    // active item (e.g. the first theme, shown as preselected): in that case the
    // value would not be saved. activated also covers re-selection.
    connect(m_language, &QComboBox::activated, this, persist);
    connect(m_defaultTheme, &QComboBox::activated, this, persist);
    connect(m_defaultLayer, &QComboBox::activated, this, persist);
    connect(m_defaultWidth, &QSpinBox::valueChanged, this, persist);
    connect(m_defaultHeight, &QSpinBox::valueChanged, this, persist);
    connect(m_fontFamily, &QComboBox::currentTextChanged, this, persist);
    connect(m_fontSize, &QSpinBox::valueChanged, this, persist);
    connect(m_dateFormat, &QLineEdit::textChanged, this, [this, persist] {
        persist();
        // The date/time format also shows in the trash: request a redraw.
        emit dateFormatChanged();
    });
    connect(m_autostart, &QCheckBox::toggled, this, persist);
    connect(m_confirmDelete, &QCheckBox::toggled, this, persist);

    // Language change needs a restart: save and relaunch with "--restart" so the
    // new process skips the single-instance probe.
    connect(m_restart, &QPushButton::clicked, this, [this, persist] {
        persist();
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                {QStringLiteral("--restart")});
        QCoreApplication::quit();
    });

    connect(m_themeList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) {
        if (it)
            loadThemeIntoEditor(it->data(Qt::UserRole).toString());
    });
    connect(m_themeBody, &QPushButton::clicked, this, [this] { pickColorFor(m_themeBody); });
    connect(m_themeTitle, &QPushButton::clicked, this, [this] { pickColorFor(m_themeTitle); });
    connect(m_themeText, &QPushButton::clicked, this, [this] { pickColorFor(m_themeText); });
    connect(m_themeSave, &QPushButton::clicked, this, &SettingsTab::saveCurrentTheme);
    connect(m_themeNew, &QPushButton::clicked, this, [this] {
        Theme t;
        t.id = QStringLiteral("tema-") + QString::number(QDateTime::currentMSecsSinceEpoch());
        t.name = I18n::t(QStringLiteral("settings.theme_new"));
        t.body = QStringLiteral("#FCFC97");
        t.titleBar = Themes::darken(t.body, 0.92);
        t.text = QStringLiteral("#45353B");
        t.builtin = false;
        t.sort = Storage::instance().themes().size();
        Storage::instance().upsertTheme(t);
        refresh();
        for (int i = 0; i < m_themeList->count(); ++i)
            if (m_themeList->item(i)->data(Qt::UserRole).toString() == t.id)
                m_themeList->setCurrentRow(i);
    });
    connect(m_themeDelete, &QPushButton::clicked, this, [this] {
        QListWidgetItem *it = m_themeList->currentItem();
        if (!it)
            return;
        const QString id = it->data(Qt::UserRole).toString();
        const Theme t = Storage::instance().theme(id);
        if (t.builtin) {
            QMessageBox::information(this, I18n::t(QStringLiteral("settings.theme_dialog_title")),
                                     I18n::t(QStringLiteral("settings.theme_builtin_locked")));
            return;
        }
        Storage::instance().deleteTheme(id);
        refresh();
    });
}

void SettingsTab::refresh()
{
    loadIntoUi();
}

void SettingsTab::applyColorToButton(QPushButton *b, const QString &hex)
{
    b->setProperty("hex", hex);
    // The picker shows only the chosen color: no hex code in the text (it stays
    // in the tooltip for those looking for it).
    b->setText(QString());
    b->setToolTip(hex);
    b->setMinimumWidth(48);
    b->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #888;").arg(hex));
}

void SettingsTab::pickColorFor(QPushButton *button)
{
    const QColor cur(button->property("hex").toString());
    const QColor c = QColorDialog::getColor(cur.isValid() ? cur : Qt::yellow, this,
                                            I18n::t(QStringLiteral("settings.choose_color")));
    if (c.isValid())
        applyColorToButton(button, c.name());
}

void SettingsTab::loadThemeIntoEditor(const QString &themeId)
{
    const Theme t = Storage::instance().theme(themeId);
    if (!t.isValid())
        return;
    m_editingThemeId = t.id;
    m_themeName->setText(Themes::displayName(t));
    m_themeName->setEnabled(!t.builtin);
    applyColorToButton(m_themeBody, t.body);
    applyColorToButton(m_themeTitle, t.titleBar);
    applyColorToButton(m_themeText, t.text);
    m_themeDelete->setEnabled(!t.builtin);
}

void SettingsTab::saveCurrentTheme()
{
    if (m_editingThemeId.isEmpty())
        return;
    Theme t = Storage::instance().theme(m_editingThemeId);
    if (!t.isValid())
        return;
    // Builtin names are derived from the id: keep them stable, only custom
    // themes take the name from the field.
    if (!t.builtin)
        t.name = m_themeName->text().trimmed().isEmpty() ? t.name : m_themeName->text().trimmed();
    t.body = m_themeBody->property("hex").toString();
    t.titleBar = m_themeTitle->property("hex").toString();
    t.text = m_themeText->property("hex").toString();
    Storage::instance().upsertTheme(t);
    refresh();
    emit settingsChanged();
}

void SettingsTab::loadIntoUi()
{
    // Widgets emit signals while being populated: without blocking, the
    // transient values would be written back to the settings (clobbering).
    QSignalBlocker bTheme(m_defaultTheme);
    QSignalBlocker bLayer(m_defaultLayer);
    QSignalBlocker bLanguage(m_language);
    QSignalBlocker bWidth(m_defaultWidth);
    QSignalBlocker bHeight(m_defaultHeight);
    QSignalBlocker bFont(m_fontFamily);
    QSignalBlocker bSize(m_fontSize);
    QSignalBlocker bDate(m_dateFormat);
    QSignalBlocker bAuto(m_autostart);
    QSignalBlocker bConfirm(m_confirmDelete);
    QSignalBlocker bList(m_themeList);

    // language selector: "System" (resolved at startup) + the discovered ones
    m_language->clear();
    m_language->addItem(I18n::t(QStringLiteral("settings.system")), QStringLiteral("system"));
    for (const LanguageInfo &lang : I18n::availableLanguages())
        m_language->addItem(lang.name, lang.code);
    {
        const QString savedLang = Storage::instance().setting(QStringLiteral("language"), QStringLiteral("en"));
        int li = m_language->findData(savedLang);
        if (li < 0)
            li = m_language->findData(QStringLiteral("en"));
        m_language->setCurrentIndex(li >= 0 ? li : 0);
    }

    // themes in the combos
    m_defaultTheme->clear();
    for (const Theme &t : Storage::instance().themes())
        m_defaultTheme->addItem(UiHelpers::swatchIcon(t.body), Themes::displayName(t), t.id);
    m_defaultTheme->addItem(UiHelpers::randomSwatchIcon(),
                            I18n::t(QStringLiteral("settings.random_color")),
                            Themes::kRandomId);
    const QString defTheme = Storage::instance().setting(QStringLiteral("default_theme"), Themes::kRandomId);
    const int ti = m_defaultTheme->findData(defTheme);
    m_defaultTheme->setCurrentIndex(ti >= 0 ? ti : 0);

    const QString defLayer = Storage::instance().setting(QStringLiteral("default_layer"), QStringLiteral("bottom"));
    m_defaultLayer->setCurrentIndex(m_defaultLayer->findData(defLayer) >= 0
                                        ? m_defaultLayer->findData(defLayer)
                                        : 0);

    m_defaultWidth->setValue(Storage::instance().setting(QStringLiteral("default_width"), QStringLiteral("230")).toInt());
    m_defaultHeight->setValue(Storage::instance().setting(QStringLiteral("default_height"), QStringLiteral("190")).toInt());

    m_fontFamily->setCurrentText(Storage::instance().setting(QStringLiteral("font_family"),
                                                             QStringLiteral("DejaVu Sans")));
    m_fontSize->setValue(Storage::instance().setting(QStringLiteral("font_size"), QStringLiteral("9")).toInt());
    m_dateFormat->setText(Storage::instance().setting(QStringLiteral("date_format"), UiHelpers::defaultDateFormat()));
    m_autostart->setChecked(Storage::instance().setting(QStringLiteral("autostart"), QStringLiteral("0")) == QLatin1String("1"));
    m_confirmDelete->setChecked(Storage::instance().setting(QStringLiteral("confirm_delete"), QStringLiteral("1")) != QLatin1String("0"));

    // theme list
    const QString keep = m_editingThemeId;
    m_themeList->clear();
    for (const Theme &t : Storage::instance().themes()) {
        auto *it = new QListWidgetItem(UiHelpers::swatchIcon(t.body), Themes::displayName(t), m_themeList);
        it->setData(Qt::UserRole, t.id);
    }
    if (!keep.isEmpty())
        for (int i = 0; i < m_themeList->count(); ++i)
            if (m_themeList->item(i)->data(Qt::UserRole).toString() == keep) {
                m_themeList->setCurrentRow(i);
                break;
            }
    if (!m_themeList->currentItem() && m_themeList->count() > 0)
        m_themeList->setCurrentRow(0);
}
