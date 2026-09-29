#pragma once

#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>

class ServiceClient : public QObject {
    Q_OBJECT
public:
    ServiceClient(QString dictionaryPath, QString endpoint, QObject *parent = nullptr);
    ~ServiceClient() override;
    QString lookup(const QString &input);
    QString complete(const QString &prefill, int count);
    void cancel();
    bool ready() const { return ready_; }

signals:
    void response(const QJsonObject &message);
    void statusChanged(bool ready, const QString &message);

private:
    void reconnect();
    void startService();
    void receive();
    QString send(QJsonObject message);
    QString dictionaryPath_;
    QString endpoint_;
    QString serverName_;
    QLocalSocket socket_;
    QTimer retry_;
    QByteArray buffer_;
    quint64 sequence_ = 0;
    bool ready_ = false;
};
