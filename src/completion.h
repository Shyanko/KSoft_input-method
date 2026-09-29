#pragma once

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include <functional>

struct Token {
    int id = -1;
    QString text;
    double probability = 0;
    bool terminal = false;
};

struct CompletionPath {
    QString text;
    QVector<int> ids;
    double probability = 0;
};

bool parseTokens(const QByteArray &payload, QVector<Token> &tokens, QString &error);
QVector<CompletionPath> rankPaths(QVector<CompletionPath> paths, int count);

class CompletionJob : public QObject {
    Q_OBJECT
public:
    CompletionJob(QNetworkAccessManager *network, QUrl endpoint, QString prefill, int count,
                  QObject *parent = nullptr);
    ~CompletionJob() override;
    void start();
    void cancel();

signals:
    void finished(const QJsonArray &paths, const QString &error);

private:
    void request(const QString &prefill, std::function<void(const QVector<Token> &)> callback);
    void expand();
    void finish(const QString &error = {});
    QNetworkAccessManager *network_;
    QUrl endpoint_;
    QString prefill_;
    int count_;
    QVector<Token> roots_;
    QVector<CompletionPath> paths_;
    int nextRoot_ = 0;
    bool done_ = false;
    QPointer<QNetworkReply> reply_;
    QTimer deadline_;
};
