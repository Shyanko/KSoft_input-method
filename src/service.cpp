#include "service.h"
#include "completion.h"
#include "protocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>

Service::Service(QString dictionaryPath, QString endpoint, QObject *parent)
    : QObject(parent), dictionaryPath_(std::move(dictionaryPath)), endpoint_(std::move(endpoint)),
      dictionary_(std::make_shared<Dictionary>())
{
    idleTimer_.setSingleShot(true);
    idleTimer_.setInterval(30000);
    connect(&idleTimer_, &QTimer::timeout, qApp, &QCoreApplication::quit);
    connect(&server_, &QLocalServer::newConnection, this, &Service::acceptConnections);
}

Service::~Service()
{
    idleTimer_.stop();
    for (auto *socket : jobs_.keys())
        cancel(socket);
    for (auto *socket : buffers_.keys()) {
        disconnect(socket, nullptr, this, nullptr);
        socket->abort();
    }
    server_.close();
    workerThread_.quit();
    workerThread_.wait();
}

bool Service::start(QString &error)
{
    const auto name = Wire::serverName(dictionaryPath_, endpoint_);
    const auto lockDirectory = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    lock_ = std::make_unique<QLockFile>(QDir(lockDirectory).filePath(name + QStringLiteral(".lock")));
    if (!lock_->tryLock(0)) {
        error = QStringLiteral("后台服务已启动，或无法获取服务锁");
        return false;
    }
    QLocalServer::removeServer(name);
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_.listen(name)) {
        error = server_.errorString();
        return false;
    }
    worker_ = new QObject;
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    workerThread_.start();
    QMetaObject::invokeMethod(worker_, [this, dictionary = dictionary_, path = dictionaryPath_] {
        QString error;
        const bool loaded = dictionary->load(path, error);
        QMetaObject::invokeMethod(this, [this, loaded, error] {
            ready_ = loaded;
            loadError_ = error;
            for (auto *socket : buffers_.keys())
                sendStatus(socket);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
    idleTimer_.start();
    return true;
}

void Service::acceptConnections()
{
    while (auto *socket = server_.nextPendingConnection()) {
        idleTimer_.stop();
        buffers_.insert(socket, {});
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { receive(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            cancel(socket);
            buffers_.remove(socket);
            lookupIds_.remove(socket);
            socket->deleteLater();
            if (buffers_.isEmpty())
                idleTimer_.start();
        });
        sendStatus(socket);
    }
}

void Service::sendStatus(QLocalSocket *socket)
{
    socket->write(Wire::encode({{QStringLiteral("type"), QStringLiteral("status")},
                               {QStringLiteral("ready"), ready_},
                               {QStringLiteral("entries"), ready_ ? static_cast<double>(dictionary_->size()) : 0},
                               {QStringLiteral("error"), loadError_}}));
}

void Service::receive(QLocalSocket *socket)
{
    auto &buffer = buffers_[socket];
    buffer += socket->readAll();
    QVector<QJsonObject> messages;
    QString error;
    if (!Wire::extract(buffer, messages, error)) {
        socket->disconnectFromServer();
        return;
    }
    for (const auto &message : messages)
        handle(socket, message);
}

void Service::cancel(QLocalSocket *socket)
{
    if (const auto job = jobs_.take(socket)) {
        job->cancel();
        job->deleteLater();
    }
}

void Service::handle(QLocalSocket *socket, const QJsonObject &message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    const QString id = message.value(QStringLiteral("id")).toString();
    if (type == QStringLiteral("cancel")) {
        cancel(socket);
        lookupIds_.remove(socket);
        return;
    }
    if (type == QStringLiteral("lookup")) {
        lookupIds_[socket] = id;
        const QString input = message.value(QStringLiteral("input")).toString().left(128);
        if (!ready_) {
            socket->write(Wire::encode({{QStringLiteral("type"), type}, {QStringLiteral("id"), id},
                                       {QStringLiteral("error"), loadError_.isEmpty() ? QStringLiteral("词库正在加载") : loadError_}}));
            return;
        }
        const QPointer<QLocalSocket> guarded(socket);
        QMetaObject::invokeMethod(worker_, [this, guarded, id, input, dictionary = dictionary_] {
            const auto entries = dictionary->lookup(input);
            QJsonArray candidates;
            for (const auto &entry : entries)
                candidates.append(entry.text);
            QMetaObject::invokeMethod(this, [this, guarded, id, candidates] {
                if (guarded && lookupIds_.value(guarded) == id) {
                    guarded->write(Wire::encode({{QStringLiteral("type"), QStringLiteral("lookup")},
                                                {QStringLiteral("id"), id}, {QStringLiteral("items"), candidates}}));
                }
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
        return;
    }
    if (type == QStringLiteral("complete")) {
        cancel(socket);
        const int count = qBound(1, message.value(QStringLiteral("k")).toInt(5), 7);
        const QString prefill = message.value(QStringLiteral("prefill")).toString();
        if (prefill.size() > 65536) {
            socket->write(Wire::encode({{QStringLiteral("type"), type}, {QStringLiteral("id"), id},
                                       {QStringLiteral("error"), QStringLiteral("补全上下文超过限制")}}));
            return;
        }
        auto *job = new CompletionJob(&network_, QUrl(endpoint_), prefill, count, this);
        jobs_.insert(socket, job);
        const QPointer<QLocalSocket> guarded(socket);
        connect(job, &CompletionJob::finished, this, [this, guarded, job, id](const QJsonArray &paths, const QString &error) {
            if (guarded && jobs_.value(guarded) == job) {
                jobs_.remove(guarded);
                guarded->write(Wire::encode({{QStringLiteral("type"), QStringLiteral("complete")},
                                            {QStringLiteral("id"), id}, {QStringLiteral("items"), paths},
                                            {QStringLiteral("error"), error}}));
            }
            job->deleteLater();
        });
        job->start();
    }
}
