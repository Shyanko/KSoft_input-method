#include "dictionary.h"

#include <QDir>
#include <QFile>
#include <QSet>
#include <QTextStream>
#include <algorithm>

QString Dictionary::normalize(QString code)
{
    code = code.toLower();
    code.replace(QChar(0x00fc), QLatin1Char('v'));
    code.remove(QLatin1Char(' '));
    code.remove(QLatin1Char('\''));
    return code;
}

bool Dictionary::load(const QString &directory, QString &error)
{
    entries_.clear();
    const QDir root(directory);
    QStringList files = {QStringLiteral("8105.dict.yaml"), QStringLiteral("41448.dict.yaml"),
                         QStringLiteral("base.dict.yaml"), QStringLiteral("ext.dict.yaml"),
                         QStringLiteral("others.dict.yaml"), QStringLiteral("tencent.dict.yaml")};
    for (const auto &name : root.entryList({QStringLiteral("*.dict.yaml")}, QDir::Files)) {
        if (!files.contains(name))
            files.append(name);
    }
    QHash<QString, DictionaryEntry> readings;
    QVector<DictionaryEntry> unannotated;
    bool opened = false;
    for (const auto &name : files) {
        QFile file(root.filePath(name));
        if (!file.exists())
            continue;
        if (!file.open(QIODevice::ReadOnly)) {
            error = name + QStringLiteral(": ") + file.errorString();
            return false;
        }
        opened = true;
        bool body = false;
        bool columns = false;
        QStringList columnNames;
        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;
            if (!body) {
                if (line == QStringLiteral("...")) {
                    body = true;
                } else if (line == QStringLiteral("columns:")) {
                    columns = true;
                } else if (columns && line.startsWith(QStringLiteral("- "))) {
                    columnNames.append(line.mid(2).trimmed());
                }
                continue;
            }
            const auto fields = line.split(QLatin1Char('\t'));
            if (fields.isEmpty())
                continue;
            const int textColumn = columnNames.isEmpty() ? 0 : columnNames.indexOf(QStringLiteral("text"));
            const int codeColumn = columnNames.isEmpty() ? 1 : columnNames.indexOf(QStringLiteral("code"));
            const int weightColumn = columnNames.isEmpty() ? 2 : columnNames.indexOf(QStringLiteral("weight"));
            const QString text = fields.value(textColumn);
            if (text.isEmpty())
                continue;
            const QString code = normalize(fields.value(codeColumn));
            QString rawWeight = fields.value(weightColumn);
            rawWeight.remove(QLatin1Char('%'));
            bool validWeight = false;
            const double weight = rawWeight.toDouble(&validWeight);
            DictionaryEntry entry{text, code, validWeight ? weight : 0};
            if (code.isEmpty()) {
                unannotated.append(entry);
                continue;
            }
            entries_.append(entry);
            if (text.toUcs4().size() == 1) {
                const auto existing = readings.constFind(text);
                if (existing == readings.cend() || existing->weight < entry.weight)
                    readings.insert(text, entry);
            }
        }
    }
    for (auto &entry : unannotated) {
        bool complete = true;
        for (const auto character : entry.text.toUcs4()) {
            const char32_t scalar = static_cast<char32_t>(character);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const QString glyph = QString::fromUcs4(&scalar, 1);
#else
            const uint legacyScalar = static_cast<uint>(scalar);
            const QString glyph = QString::fromUcs4(&legacyScalar, 1);
#endif
            const auto reading = readings.constFind(glyph);
            if (reading == readings.cend()) {
                complete = false;
                break;
            }
            entry.code += reading->code;
        }
        if (complete && !entry.code.isEmpty())
            entries_.append(std::move(entry));
    }
    std::stable_sort(entries_.begin(), entries_.end(), [](const auto &left, const auto &right) {
        if (left.code != right.code)
            return left.code < right.code;
        return left.text < right.text;
    });
    entries_.erase(std::unique(entries_.begin(), entries_.end(), [](const auto &left, const auto &right) {
        return left.code == right.code && left.text == right.text;
    }), entries_.end());
    if (!opened || entries_.isEmpty()) {
        error = QStringLiteral("未找到可用的 *.dict.yaml 词库：") + directory;
        return false;
    }
    return true;
}

QVector<DictionaryEntry> Dictionary::lookup(const QString &input, int limit) const
{
    const QString code = normalize(input);
    if (code.isEmpty() || limit <= 0)
        return {};
    auto iterator = std::lower_bound(entries_.cbegin(), entries_.cend(), code,
        [](const auto &entry, const QString &value) { return entry.code < value; });
    QVector<const DictionaryEntry *> matches;
    while (iterator != entries_.cend() && iterator->code.startsWith(code)) {
        matches.append(&*iterator);
        ++iterator;
    }
    const auto better = [&code](const auto *left, const auto *right) {
        const bool leftExact = left->code == code;
        const bool rightExact = right->code == code;
        if (leftExact != rightExact)
            return leftExact;
        if (left->weight != right->weight)
            return left->weight > right->weight;
        if (left->code.size() != right->code.size())
            return left->code.size() < right->code.size();
        return left->text < right->text;
    };
    std::make_heap(matches.begin(), matches.end(), [&better](auto left, auto right) { return better(right, left); });
    QVector<DictionaryEntry> result;
    QSet<QString> seen;
    while (!matches.isEmpty() && result.size() < limit) {
        std::pop_heap(matches.begin(), matches.end(), [&better](auto left, auto right) { return better(right, left); });
        const auto *entry = matches.takeLast();
        if (!seen.contains(entry->text)) {
            seen.insert(entry->text);
            result.append(*entry);
        }
    }
    return result;
}
