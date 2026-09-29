#include "protocol.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>

QString Wire::serverName(const QString &dictionaryPath, const QString &endpoint)
{
    const QString identity = QDir::homePath() + QLatin1Char('|')
        + QFileInfo(dictionaryPath).canonicalFilePath() + QLatin1Char('|') + endpoint;
    const auto digest = QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(24);
    return QStringLiteral("ksipl-v1-") + QString::fromLatin1(digest);
}

QByteArray Wire::encode(const QJsonObject &message)
{
    return QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
}

bool Wire::extract(QByteArray &buffer, QVector<QJsonObject> &messages, QString &error)
{
    for (;;) {
        const auto newline = buffer.indexOf('\n');
        if (newline < 0) {
            if (buffer.size() > MaxFrame) {
                error = QStringLiteral("IPC 消息超过大小限制");
                return false;
            }
            return true;
        }
        if (newline > MaxFrame) {
            error = QStringLiteral("IPC 消息超过大小限制");
            return false;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(buffer.left(newline), &parseError);
        buffer.remove(0, newline + 1);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            error = QStringLiteral("IPC 消息不是 JSON 对象");
            return false;
        }
        messages.append(document.object());
    }
}
