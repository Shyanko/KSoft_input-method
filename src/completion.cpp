#include "completion.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <algorithm>
#include <cmath>

bool parseTokens(const QByteArray &payload, QVector<Token> &tokens, QString &error)
{
    tokens.clear();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        error = QStringLiteral("模型返回的内容不是有效 JSON");
        return false;
    }
    QJsonArray array;
    bool found = document.isArray();
    if (found) {
        array = document.array();
    } else if (document.isObject()) {
        const auto object = document.object();
        for (const auto &key : {"tokens", "candidates", "data", "next_tokens"}) {
            if (object.value(QLatin1String(key)).isArray()) {
                array = object.value(QLatin1String(key)).toArray();
                found = true;
                break;
            }
        }
        if (!found && object.contains(QStringLiteral("token_id"))) {
            array.append(object);
            found = true;
        }
    }
    if (!found) {
        error = QStringLiteral("模型响应缺少 token 数组");
        return false;
    }
    for (const auto &value : array) {
        const auto object = value.toObject();
        if (!object.value(QStringLiteral("token_id")).isDouble()
            || !object.value(QStringLiteral("token")).isString()) {
            error = QStringLiteral("模型 token 缺少 token_id 或 token");
            return false;
        }
        double probability = -1;
        if (object.value(QStringLiteral("probability")).isDouble())
            probability = object.value(QStringLiteral("probability")).toDouble();
        else if (object.value(QStringLiteral("logprob")).isDouble())
            probability = std::exp(object.value(QStringLiteral("logprob")).toDouble());
        if (!std::isfinite(probability) || probability < 0 || probability > 1) {
            error = QStringLiteral("模型返回了无效概率");
            return false;
        }
        const QString text = object.value(QStringLiteral("token")).toString();
        const bool terminal = object.value(QStringLiteral("is_eos")).toBool()
            || text == QStringLiteral("<|endoftext|>") || text == QStringLiteral("<|im_end|>");
        tokens.append({object.value(QStringLiteral("token_id")).toInt(), text, probability, terminal});
    }
    std::stable_sort(tokens.begin(), tokens.end(), [](const auto &left, const auto &right) {
        return left.probability > right.probability;
    });
    return true;
}

QVector<CompletionPath> rankPaths(QVector<CompletionPath> paths, int count)
{
    std::sort(paths.begin(), paths.end(), [](const auto &left, const auto &right) {
        if (left.probability != right.probability)
            return left.probability > right.probability;
        if (left.ids.size() != right.ids.size())
            return left.ids.size() < right.ids.size();
        return left.ids < right.ids;
    });
    if (paths.size() > count)
        paths.resize(count);
    return paths;
}

CompletionJob::CompletionJob(QNetworkAccessManager *network, QUrl endpoint, QString prefill,
                           int count, QObject *parent)
    : QObject(parent), network_(network), endpoint_(std::move(endpoint)),
      prefill_(std::move(prefill)), count_(qBound(1, count, 7))
{
    deadline_.setSingleShot(true);
    deadline_.setInterval(20000);
    connect(&deadline_, &QTimer::timeout, this, [this] { finish(QStringLiteral("模型请求超时（20 秒）")); });
}

CompletionJob::~CompletionJob()
{
    cancel();
}

void CompletionJob::cancel()
{
    done_ = true;
    deadline_.stop();
    if (reply_) {
        disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
        reply_->deleteLater();
        reply_.clear();
    }
}

void CompletionJob::start()
{
    deadline_.start();
    request(prefill_, [this](const QVector<Token> &tokens) {
        roots_ = tokens;
        for (const auto &token : roots_) {
            if (!token.terminal && !token.text.isEmpty())
                paths_.append({token.text, {token.id}, token.probability});
        }
        paths_ = rankPaths(paths_, count_);
        expand();
    });
}

void CompletionJob::request(const QString &prefill, std::function<void(const QVector<Token> &)> callback)
{
    QNetworkRequest request(endpoint_);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QJsonObject payload{{QStringLiteral("prefill"), prefill}, {QStringLiteral("k"), count_}};
    auto *reply = network_->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    reply_ = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (reply->bytesAvailable() > 1024 * 1024)
            finish(QStringLiteral("模型响应超过大小限制"));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback = std::move(callback)] {
        if (done_)
            return;
        const auto payload = reply->readAll();
        const auto networkError = reply->error();
        const auto message = reply->errorString();
        reply_.clear();
        reply->deleteLater();
        if (networkError != QNetworkReply::NoError) {
            finish(QStringLiteral("模型连接失败：") + message);
            return;
        }
        QVector<Token> tokens;
        QString error;
        if (!parseTokens(payload, tokens, error)) {
            finish(error);
            return;
        }
        tokens.erase(std::remove_if(tokens.begin(), tokens.end(), [](const Token &token) {
            return token.text.contains(QLatin1Char('\r')) || token.text.contains(QLatin1Char('\n'));
        }), tokens.end());
        callback(tokens);
    });
}

void CompletionJob::expand()
{
    while (nextRoot_ < roots_.size()) {
        const Token root = roots_.at(nextRoot_++);
        if (root.terminal || root.text.isEmpty())
            continue;
        if (paths_.size() == count_ && (root.probability < paths_.last().probability
            || (root.probability == paths_.last().probability && paths_.last().ids.size() == 1)))
            break;
        request(prefill_ + root.text, [this, root](const QVector<Token> &tokens) {
            for (const auto &token : tokens) {
                if (!token.terminal && !token.text.isEmpty())
                    paths_.append({root.text + token.text, {root.id, token.id}, root.probability * token.probability});
            }
            paths_ = rankPaths(paths_, count_);
            expand();
        });
        return;
    }
    finish();
}

void CompletionJob::finish(const QString &error)
{
    if (done_)
        return;
    cancel();
    QJsonArray result;
    if (error.isEmpty()) {
        for (const auto &path : paths_) {
            QJsonArray ids;
            for (const auto id : path.ids)
                ids.append(id);
            result.append(QJsonObject{{QStringLiteral("text"), path.text},
                                      {QStringLiteral("probability"), path.probability},
                                      {QStringLiteral("token_ids"), ids}});
        }
    }
    emit finished(result, error);
}
