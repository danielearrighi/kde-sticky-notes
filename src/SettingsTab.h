#pragma once

#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

/// "Settings" tab: general preferences + theme editor.
class SettingsTab : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsTab(QWidget *parent = nullptr);

    /// Reloads preferences and theme list from Storage.
    void refresh();

signals:
    void settingsChanged();
    /// Emitted when the date/time format changed: the trash dates must be redrawn.
    void dateFormatChanged();

private:
    void buildUi();
    void loadIntoUi();
    void loadThemeIntoEditor(const QString &themeId);
    void saveCurrentTheme();
    void pickColorFor(QPushButton *button);
    static void applyColorToButton(QPushButton *b, const QString &hex);

    // general preferences
    QComboBox *m_language = nullptr;
    QPushButton *m_restart = nullptr;
    QLabel *m_languageHint = nullptr;
    QComboBox *m_defaultTheme = nullptr;
    QComboBox *m_defaultLayer = nullptr;
    QSpinBox *m_defaultWidth = nullptr;
    QSpinBox *m_defaultHeight = nullptr;
    QComboBox *m_fontFamily = nullptr;
    QSpinBox *m_fontSize = nullptr;
    QLineEdit *m_dateFormat = nullptr;
    QCheckBox *m_autostart = nullptr;
    QCheckBox *m_confirmDelete = nullptr;

    // theme editor
    QListWidget *m_themeList = nullptr;
    QLineEdit *m_themeName = nullptr;
    QPushButton *m_themeBody = nullptr;
    QPushButton *m_themeTitle = nullptr;
    QPushButton *m_themeText = nullptr;
    QPushButton *m_themeNew = nullptr;
    QPushButton *m_themeDelete = nullptr;
    QPushButton *m_themeSave = nullptr;
    QString m_editingThemeId;
};
