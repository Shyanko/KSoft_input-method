#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace Wire {
constexpr int MaxFrame = 2 * 1024 * 1024;
QString serverName(const QString &dictionaryPath, const QString &endpoint);
QByteArray encode(const QJsonObject &message);
bool extract(QByteArray &buffer, QVector<QJsonObject> &messages, QString &error);
}
