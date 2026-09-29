#pragma once

#include "dictionary.h"
#include <QHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <memory>

class CompletionJob;

class Service : public QObject {
    Q_OBJECT
public:
    Service(QString dictionaryPath, QString endpoint, QObject *parent = nullptr);
    ~Service() override;
    bool start(QString &error);

private:
    void acceptConnections();
    void receive(QLocalSocket *socket);
    void handle(QLocalSocket *socket, const QJsonObject &message);
    void sendStatus(QLocalSocket *socket);
    void cancel(QLocalSocket *socket);
    QString dictionaryPath_;
    QString endpoint_;
    QLocalServer server_;
    std::unique_ptr<QLockFile> lock_;
    QNetworkAccessManager network_;
    QHash<QLocalSocket *, QByteArray> buffers_;
    QHash<QLocalSocket *, QPointer<CompletionJob>> jobs_;
    QHash<QLocalSocket *, QString> lookupIds_;
    QThread workerThread_;
    QObject *worker_ = nullptr;
    std::shared_ptr<Dictionary> dictionary_;
    QTimer idleTimer_;
    bool ready_ = false;
    QString loadError_;
};
