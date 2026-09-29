#pragma once

#include <QHash>
#include <QString>
#include <QVector>

struct DictionaryEntry {
    QString text;
    QString code;
    double weight = 0;
};

class Dictionary {
public:
    bool load(const QString &directory, QString &error);
    QVector<DictionaryEntry> lookup(const QString &input, int limit = 70) const;
    qsizetype size() const { return entries_.size(); }
    static QString normalize(QString code);

private:
    QVector<DictionaryEntry> entries_;
};
