#include "I18n.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>

#include <algorithm>
#include <utility>

namespace
{
// Installed translators, in install order (English base first, override last).
QList<JsonTranslator *> g_translators;
QString g_currentCode = QStringLiteral("en");

/// Builds the flat source -> translation map from a JSON resource file.
QHash<QString, QString> loadMap(const QString &path)
{
    QHash<QString, QString> map;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return map;

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return map;

    const QJsonObject obj = doc.object();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        if (it.key() == QLatin1String("_meta"))
            continue;
        if (it.value().isString())
            map.insert(it.key(), it.value().toString());
    }
    return map;
}
} // namespace

JsonTranslator::JsonTranslator(const QHash<QString, QString> &map, QObject *parent)
    : QTranslator(parent)
    , m_map(map)
{
}

QString JsonTranslator::translate(const char *context, const char *sourceText,
                                  const char *disambiguation, int n) const
{
    Q_UNUSED(context);
    Q_UNUSED(disambiguation);
    Q_UNUSED(n);

    if (!sourceText)
        return QString();

    const auto it = m_map.constFind(QString::fromUtf8(sourceText));
    // A null QString makes Qt fall back to the source (English) string.
    return it == m_map.constEnd() ? QString() : it.value();
}

bool JsonTranslator::isEmpty() const
{
    return m_map.isEmpty();
}

QList<LanguageInfo> I18n::availableLanguages()
{
    QList<LanguageInfo> langs;

    QDir dir(QStringLiteral(":/i18n"));
    if (!dir.exists())
        return langs;

    const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString &fileName : files) {
        QFile f(dir.filePath(fileName));
        if (!f.open(QIODevice::ReadOnly))
            continue;

        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject())
            continue;

        const QJsonObject meta = doc.object().value(QStringLiteral("_meta")).toObject();
        const QString code = meta.value(QStringLiteral("code")).toString();
        if (code.isEmpty())
            continue;   // no "_meta": not a language file

        LanguageInfo info;
        info.code = code;
        info.name = meta.value(QStringLiteral("name")).toString();
        if (info.name.isEmpty())
            info.name = code;
        langs << info;
    }

    // "en" first (the source language), then by native name.
    std::sort(langs.begin(), langs.end(), [](const LanguageInfo &a, const LanguageInfo &b) {
        const bool aEn = a.code == QLatin1String("en");
        const bool bEn = b.code == QLatin1String("en");
        if (aEn != bEn)
            return aEn;
        return a.name.localeAwareCompare(b.name) < 0;
    });

    return langs;
}

QString I18n::applyLanguage(const QString &code)
{
    QString wanted = code;
    if (wanted.isEmpty() || wanted == QLatin1String("system"))
        wanted = QLocale::system().name().section(QLatin1Char('_'), 0, 0);

    // English is always installed as the base/fallback.
    const bool hasEnglish = QFile::exists(QStringLiteral(":/i18n/en.json"));

    // The override is the requested language, if it exists and is not English.
    QString overrideCode;
    if (wanted != QLatin1String("en")
        && QFile::exists(QStringLiteral(":/i18n/%1.json").arg(wanted)))
        overrideCode = wanted;

    // Replace the previously installed translators.
    for (JsonTranslator *t : std::as_const(g_translators))
        QCoreApplication::removeTranslator(t);
    qDeleteAll(g_translators);
    g_translators.clear();

    // Qt::installTranslator() prepends: install English FIRST, then the
    // override, so the override wins and English remains the fallback.
    if (hasEnglish) {
        auto *en = new JsonTranslator(loadMap(QStringLiteral(":/i18n/en.json")));
        QCoreApplication::installTranslator(en);
        g_translators << en;
    }
    if (!overrideCode.isEmpty()) {
        auto *tr = new JsonTranslator(loadMap(QStringLiteral(":/i18n/%1.json").arg(overrideCode)));
        QCoreApplication::installTranslator(tr);
        g_translators << tr;
    }

    g_currentCode = overrideCode.isEmpty() ? QStringLiteral("en") : overrideCode;
    return g_currentCode;
}

QString I18n::currentCode()
{
    return g_currentCode;
}

QString I18n::t(const QString &key)
{
    return QCoreApplication::translate("i18n", key.toUtf8().constData());
}
