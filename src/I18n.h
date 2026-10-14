#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QTranslator>

/// A language available for translation, with its native name (e.g.
/// {"it", "Italiano"}).
struct LanguageInfo
{
    QString code;
    QString name;
};

/// Runtime translation support.
///
/// One JSON file per language is embedded as a Qt resource
/// (`:/i18n/<code>.json`): a flat map whose keys are symbolic, lowercase,
/// dot-grouped identifiers (e.g. `"notes.your_notes"`) and whose values are the
/// translations (plain text, no HTML). Call sites look strings up with
/// `I18n::t("symbolic.key")`. The `_meta` object (`{"code", "name"}`) describes
/// the language and is not part of the map.
///
/// English (`en.json`) is always installed as the base translator; the selected
/// language is installed on top of it, so a missing key falls back to English.
namespace I18n
{
/// Languages discovered in `:/i18n/*.json` (files without a valid `_meta` are
/// skipped). Sorted with "en" first, then by native name.
QList<LanguageInfo> availableLanguages();

/// Installs the translators for \a code. English (`en.json`) is always installed
/// as the fallback base; the selected language, when present and not English, is
/// installed on top of it (so it wins and English fills the gaps). `"system"`
/// resolves through the system locale. Returns the effective code actually
/// applied (the override, or "en" when only the base is available).
QString applyLanguage(const QString &code);

/// Effective code from the last applyLanguage().
QString currentCode();

/// Translates the symbolic \a key through the installed translators. The
/// context is fixed ("i18n"); translators look up the key as the source text.
QString t(const QString &key);
} // namespace I18n

/// QTranslator backed by a flat map (symbolic key -> translation).
class JsonTranslator : public QTranslator
{
public:
    explicit JsonTranslator(const QHash<QString, QString> &map, QObject *parent = nullptr);

    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override;
    bool isEmpty() const override;

private:
    QHash<QString, QString> m_map;
};
