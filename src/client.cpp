#include "client.h"
#include "protocol.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>

ServiceClient::ServiceClient(QString dictionaryPath, QString endpoint, QObject *parent)
    : QObject(parent), dictionaryPath_(std::move(dictionaryPath)), endpoint_(std::move(endpoint)),
      serverName_(Wire::serverName(dictionaryPath_, endpoint_))
{
    retry_.setInterval(700);
    connect(&retry_, &QTimer::timeout, this, &ServiceClient::reconnect);
    connect(&socket_, &QLocalSocket::connected, this, [this] {
        buffer_.clear();
        retry_.stop();
        emit statusChanged(false, QStringLiteral("后台已连接，正在加载词库…"));
    });
    connect(&socket_, &QLocalSocket::disconnected, this, [this] {
        ready_ = false;
        buffer_.clear();
        emit statusChanged(false, QStringLiteral("后台连接已断开，正在重连…"));
        retry_.start();
    });
    connect(&socket_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        ready_ = false;
        emit statusChanged(false, QStringLiteral("正在启动后台服务…"));
        retry_.start();
        startService();
    });
    connect(&socket_, &QLocalSocket::readyRead, this, &ServiceClient::receive);
    retry_.start();
    QTimer::singleShot(0, this, &ServiceClient::reconnect);
}

ServiceClient::~ServiceClient()
{
    retry_.stop();
    disconnect(&socket_, nullptr, this, nullptr);
    socket_.abort();
}

void ServiceClient::reconnect()
{
    if (socket_.state() != QLocalSocket::UnconnectedState)
        return;
    socket_.connectToServer(serverName_);
}

void ServiceClient::startService()
{
    static QElapsedTimer lastSpawn;
    if (!lastSpawn.isValid() || lastSpawn.elapsed() > 3000) {
        lastSpawn.start();
        QProcess process;
        process.setProgram(QCoreApplication::applicationFilePath());
        process.setArguments({QStringLiteral("--service"), QStringLiteral("--dicts"), dictionaryPath_,
                              QStringLiteral("--endpoint"), endpoint_});
#ifdef Q_OS_WIN
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
            arguments->flags |= 0x08000000;
        });
#endif
        if (!process.startDetached())
            emit statusChanged(false, QStringLiteral("无法启动后台服务，请检查程序路径"));
    }
}

QString ServiceClient::send(QJsonObject message)
{
    const QString id = QString::number(++sequence_);
    message.insert(QStringLiteral("id"), id);
    if (socket_.state() == QLocalSocket::ConnectedState)
        socket_.write(Wire::encode(message));
    return id;
}

QString ServiceClient::lookup(const QString &input)
{
    return send({{QStringLiteral("type"), QStringLiteral("lookup")}, {QStringLiteral("input"), input}});
}

QString ServiceClient::complete(const QString &prefill, int count)
{
    return send({{QStringLiteral("type"), QStringLiteral("complete")},
                 {QStringLiteral("prefill"), prefill}, {QStringLiteral("k"), count}});
}

void ServiceClient::cancel()
{
    send({{QStringLiteral("type"), QStringLiteral("cancel")}});
}

void ServiceClient::receive()
{
    buffer_ += socket_.readAll();
    QVector<QJsonObject> messages;
    QString error;
    if (!Wire::extract(buffer_, messages, error)) {
        emit statusChanged(false, error);
        socket_.abort();
        return;
    }
    for (const auto &message : messages) {
        if (message.value(QStringLiteral("type")).toString() == QStringLiteral("status")) {
            ready_ = message.value(QStringLiteral("ready")).toBool();
            const auto error = message.value(QStringLiteral("error")).toString();
            emit statusChanged(ready_, !error.isEmpty() ? error : ready_
                ? QStringLiteral("词库就绪 · %1 条").arg(message.value(QStringLiteral("entries")).toInt())
                : QStringLiteral("正在加载词库…"));
        } else {
            emit response(message);
        }
    }
}
