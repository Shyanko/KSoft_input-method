#include "completion.h"
#include "dictionary.h"
#include "editor.h"
#include "protocol.h"
#include "service.h"
#include "window.h"

#include <QFile>
#include <QElapsedTimer>
#include <QAction>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <random>

class ModelStub : public QTcpServer {
public:
    explicit ModelStub(QObject *parent = nullptr) : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    *buffer += socket->readAll();
                    const auto divider = buffer->indexOf("\r\n\r\n");
                    if (divider < 0)
                        return;
                    int length = 0;
                    for (const auto &line : buffer->left(divider).split('\n')) {
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(15).trimmed().toInt();
                    }
                    if (buffer->size() < divider + 4 + length)
                        return;
                    const auto request = QJsonDocument::fromJson(buffer->mid(divider + 4, length)).object();
                    buffer->clear();
                    ++requests;
                    received.append(request);
                    const auto payload = handler(request);
                    const QPointer<QTcpSocket> guarded(socket);
                    QTimer::singleShot(delay, this, [guarded, payload] {
                        if (!guarded || guarded->state() != QAbstractSocket::ConnectedState)
                            return;
                        guarded->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                            + QByteArray::number(payload.size()) + "\r\n\r\n" + payload);
                        guarded->disconnectFromHost();
                    });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        listen(QHostAddress::LocalHost);
    }
    QUrl endpoint() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/v1/next-token").arg(serverPort()));
    }
    std::function<QByteArray(const QJsonObject &)> handler;
    QVector<QJsonObject> received;
    int requests = 0;
    int delay = 0;
};

static QByteArray tokenPayload(const QVector<Token> &tokens, int count)
{
    QJsonArray result;
    auto sorted = tokens;
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto &left, const auto &right) {
        return left.probability > right.probability;
    });
    for (int index = 0; index < qMin(count, static_cast<int>(sorted.size())); ++index) {
        const auto &token = sorted.at(index);
        result.append(QJsonObject{{"token_id", token.id}, {"token", token.text}, {"probability", token.probability}});
    }
    return QJsonDocument(result).toJson(QJsonDocument::Compact);
}

class Tests : public QObject {
    Q_OBJECT
private slots:
    void protocolFrames();
    void dictionaryFormats();
    void tokenResponses();
    void searchMatchesExhaustive();
    void completionExcludesNewlines_data();
    void completionExcludesNewlines();
    void cancelledSearch();
    void networkFailure();
    void editorAndIpc();
    void editorChinesePunctuation();
    void fullDictionaryAndProcesses();
    void windowDocuments();
};

void Tests::protocolFrames()
{
    const QJsonObject first{{"text", QStringLiteral("你好\n世界")}};
    const auto frame = Wire::encode(first);
    QByteArray buffer = frame.left(7);
    QVector<QJsonObject> messages;
    QString error;
    QVERIFY(Wire::extract(buffer, messages, error));
    QVERIFY(messages.isEmpty());
    buffer += frame.mid(7) + Wire::encode({{"id", "2"}});
    QVERIFY(Wire::extract(buffer, messages, error));
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages.first(), first);
    QVERIFY(buffer.isEmpty());
    buffer = "[]\n";
    QVERIFY(!Wire::extract(buffer, messages, error));
    buffer = QByteArray(Wire::MaxFrame + 1, 'x');
    QVERIFY(!Wire::extract(buffer, messages, error));
}

void Tests::dictionaryFormats()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile characters(directory.filePath("8105.dict.yaml"));
    QVERIFY(characters.open(QIODevice::WriteOnly));
    characters.write(QStringLiteral("---\nname: test\n...\n你\tni\t100\n好\thao\t90\n吕\tlv\t10\n你\tni\t1\n").toUtf8());
    characters.close();
    QFile words(directory.filePath("base.dict.yaml"));
    QVERIFY(words.open(QIODevice::WriteOnly));
    words.write(QStringLiteral("---\n...\n你好\tni hao\t50\n拟好\tni hao\t20\n你号\tni hao\t10\n#忽略\tni hao\t999\n").toUtf8());
    words.close();
    QFile tencent(directory.filePath("tencent.dict.yaml"));
    QVERIFY(tencent.open(QIODevice::WriteOnly));
    tencent.write(QStringLiteral("---\ncolumns:\n  - text\n  - weight\n...\n你好好\t80\n未知\t90\n").toUtf8());
    tencent.close();
    Dictionary dictionary;
    QString error;
    QVERIFY2(dictionary.load(directory.path(), error), qPrintable(error));
    QCOMPARE(dictionary.lookup("ni").first().text, QStringLiteral("你"));
    QCOMPARE(dictionary.lookup("ni'hao").first().text, QStringLiteral("你好"));
    QCOMPARE(dictionary.lookup("nihao", 2).size(), 2);
    QCOMPARE(dictionary.lookup("nihaohao").first().text, QStringLiteral("你好好"));
    QCOMPARE(dictionary.lookup(QStringLiteral("lü")).first().text, QStringLiteral("吕"));
    QVERIFY(dictionary.lookup("").isEmpty());
    QVERIFY(dictionary.lookup("missing").isEmpty());
    QCOMPARE(dictionary.lookup("ni").first().weight, 100.0);
}

void Tests::tokenResponses()
{
    QVector<Token> tokens;
    QString error;
    QVERIFY(parseTokens(R"({"tokens":[{"token_id":1,"token":" A","logprob":-0.6931471805599453}]})", tokens, error));
    QCOMPARE(tokens.first().text, QStringLiteral(" A"));
    QVERIFY(qAbs(tokens.first().probability - 0.5) < 1e-12);
    QVERIFY(!parseTokens("not JSON", tokens, error));
    QVERIFY(!parseTokens(R"([{"token_id":1,"token":"x","probability":1.2}])", tokens, error));
    QVERIFY(!parseTokens(R"({"error":"failed"})", tokens, error));
    QVERIFY(parseTokens("[]", tokens, error));
    QVERIFY(tokens.isEmpty());
}

void Tests::searchMatchesExhaustive()
{
    ModelStub server;
    QNetworkAccessManager network;
    std::mt19937 generator(2026);
    std::uniform_real_distribution<double> distribution(0.01, 1.0);
    for (int trial = 0; trial < 8; ++trial) {
        QVector<Token> roots;
        QHash<QString, QVector<Token>> children;
        double rootSum = 0;
        for (int index = 0; index < 9; ++index) {
            const double weight = distribution(generator);
            rootSum += weight;
            roots.append({index, QString(QChar('A' + index)), weight, false});
        }
        QVector<CompletionPath> exhaustive;
        for (auto &root : roots) {
            root.probability /= rootSum;
            exhaustive.append({root.text, {root.id}, root.probability});
            QVector<Token> next;
            double sum = 0;
            for (int index = 0; index < 9; ++index) {
                const double weight = distribution(generator);
                sum += weight;
                next.append({index + 20, QString(QChar('a' + index)), weight, false});
            }
            for (auto &token : next) {
                token.probability /= sum;
                exhaustive.append({root.text + token.text, {root.id, token.id}, root.probability * token.probability});
            }
            children.insert(root.text, next);
        }
        if (trial == 0) {
            roots = {{1, "A", 0.9, false}, {2, "C", 0.07, false}, {3, "D", 0.03, false}};
            children = {{"A", {{4, "B", 0.8, false}, {5, "E", 0.2, false}}},
                        {"C", {{6, "F", 1, false}}}, {"D", {{7, "G", 1, false}}}};
            exhaustive.clear();
            for (const auto &root : roots) {
                exhaustive.append({root.text, {root.id}, root.probability});
                for (const auto &token : children.value(root.text))
                    exhaustive.append({root.text + token.text, {root.id, token.id}, root.probability * token.probability});
            }
        }
        server.handler = [roots, children](const QJsonObject &request) {
            const QString prefix = request.value("prefill").toString();
            return tokenPayload(prefix == "context" ? roots : children.value(prefix.mid(7)), request.value("k").toInt());
        };
        for (int count = 1; count <= 7; ++count) {
            CompletionJob job(&network, server.endpoint(), "context", count);
            QSignalSpy completed(&job, &CompletionJob::finished);
            job.start();
            QVERIFY(completed.wait(3000));
            QCOMPARE(completed.first().at(1).toString(), QString());
            const auto actual = completed.first().at(0).toJsonArray();
            const auto expected = rankPaths(exhaustive, count);
            QCOMPARE(actual.size(), expected.size());
            for (int index = 0; index < actual.size(); ++index) {
                QCOMPARE(actual.at(index).toObject().value("text").toString(), expected.at(index).text);
                QVERIFY(qAbs(actual.at(index).toObject().value("probability").toDouble() - expected.at(index).probability) < 1e-12);
            }
        }
    }
}

void Tests::completionExcludesNewlines_data()
{
    QTest::addColumn<QString>("blockedToken");
    QTest::newRow("lf") << QStringLiteral("\n");
    QTest::newRow("cr") << QStringLiteral("\r");
    QTest::newRow("crlf") << QStringLiteral("\r\n");
    QTest::newRow("repeated") << QStringLiteral("\n\n");
    QTest::newRow("mixed-lf") << QStringLiteral("text\nmore");
    QTest::newRow("mixed-cr") << QStringLiteral("text\rmore");
}

void Tests::completionExcludesNewlines()
{
    QFETCH(QString, blockedToken);
    ModelStub server;
    const QString context = QStringLiteral("existing\ncontext");
    server.handler = [blockedToken, context](const QJsonObject &request) {
        const QString prefix = request.value("prefill").toString();
        const QVector<Token> tokens = prefix == context
            ? QVector<Token>{{1, blockedToken, 0.6, false}, {2, "A", 0.3, false}, {3, " ", 0.1, false}}
            : prefix == context + "A"
                ? QVector<Token>{{4, blockedToken, 0.6, false}, {5, "B", 0.3, false}, {6, "C", 0.1, false}}
                : QVector<Token>{};
        return tokenPayload(tokens, request.value("k").toInt());
    };
    QNetworkAccessManager network;
    CompletionJob job(&network, server.endpoint(), context, 3);
    QSignalSpy completed(&job, &CompletionJob::finished);
    job.start();
    QVERIFY(completed.wait(3000));
    QCOMPARE(completed.first().at(1).toString(), QString());
    const auto paths = completed.first().at(0).toJsonArray();
    QCOMPARE(paths.size(), 3);
    QCOMPARE(paths.at(0).toObject().value("text").toString(), QStringLiteral("A"));
    QCOMPARE(paths.at(1).toObject().value("text").toString(), QStringLiteral(" "));
    QCOMPARE(paths.at(2).toObject().value("text").toString(), QStringLiteral("AB"));
    QVERIFY(qAbs(paths.at(2).toObject().value("probability").toDouble() - 0.09) < 1e-12);
    QCOMPARE(server.received.size(), 3);
    QCOMPARE(server.received.at(0).value("prefill").toString(), context);
    QCOMPARE(server.received.at(1).value("prefill").toString(), context + "A");
    QCOMPARE(server.received.at(2).value("prefill").toString(), context + " ");
    CompletionJob onlyBlocked(&network, server.endpoint(), context, 1);
    QSignalSpy emptyCompleted(&onlyBlocked, &CompletionJob::finished);
    onlyBlocked.start();
    QVERIFY(emptyCompleted.wait(3000));
    QVERIFY(emptyCompleted.first().at(0).toJsonArray().isEmpty());
    QCOMPARE(emptyCompleted.first().at(1).toString(), QString());
    QCOMPARE(server.requests, 4);
}

void Tests::cancelledSearch()
{
    ModelStub server;
    server.delay = 150;
    server.handler = [](const QJsonObject &) { return tokenPayload({{1, "A", 0.9, false}}, 1); };
    QNetworkAccessManager network;
    CompletionJob job(&network, server.endpoint(), "", 1);
    QSignalSpy completed(&job, &CompletionJob::finished);
    job.start();
    QTRY_COMPARE(server.requests, 1);
    job.cancel();
    QTest::qWait(200);
    QCOMPARE(completed.count(), 0);
}

void Tests::networkFailure()
{
    ModelStub server;
    server.handler = [](const QJsonObject &) { return QByteArray("invalid"); };
    QNetworkAccessManager network;
    CompletionJob job(&network, server.endpoint(), "", 5);
    QSignalSpy completed(&job, &CompletionJob::finished);
    job.start();
    QVERIFY(completed.wait(3000));
    QVERIFY(!completed.first().at(1).toString().isEmpty());
    QVERIFY(completed.first().at(0).toJsonArray().isEmpty());
}

void Tests::editorAndIpc()
{
    QTemporaryDir directory;
    QFile dictionaryFile(directory.filePath("base.dict.yaml"));
    QVERIFY(dictionaryFile.open(QIODevice::WriteOnly));
    dictionaryFile.write(QStringLiteral("---\n...\n你好\tni hao\t100\n你\tni\t200\n").toUtf8());
    dictionaryFile.close();
    ModelStub server;
    server.handler = [](const QJsonObject &request) {
        const QString prefix = request.value("prefill").toString();
        const auto tokens = prefix.endsWith("A") ? QVector<Token>{{3, "B", 0.8, false}, {4, "E", 0.2, false}}
                                                 : QVector<Token>{{1, "A", 0.9, false}, {2, "C", 0.1, false}};
        return tokenPayload(tokens, request.value("k").toInt());
    };
    Service service(directory.path(), server.endpoint().toString());
    QString error;
    QVERIFY2(service.start(error), qPrintable(error));
    Editor editor(directory.path(), server.endpoint().toString());
    editor.resize(700, 400);
    editor.show();
    editor.activateWindow();
    editor.setFocus();
    auto *client = editor.findChild<ServiceClient *>();
    QTRY_VERIFY(client->ready());
    editor.setCompletionEnabled(false);
    QTest::keyClicks(&editor, "nihao");
    QCOMPARE(editor.preedit(), QStringLiteral("nihao"));
    QCOMPARE(editor.toPlainText(), QString());
    auto *list = editor.findChild<QListWidget *>();
    QTRY_VERIFY(list->count() > 0 && list->item(0)->text().contains(QStringLiteral("你好")));
    QTest::keyClick(&editor, Qt::Key_Space);
    QCOMPARE(editor.toPlainText(), QStringLiteral("你好"));
    QCOMPARE(editor.preedit(), QString());
    QTest::keyClicks(&editor, "abc");
    QTest::keyClick(&editor, Qt::Key_Escape);
    QCOMPARE(editor.toPlainText(), QStringLiteral("你好"));
    QTest::keyClick(&editor, Qt::Key_F2);
    QVERIFY(!editor.chinese());
    editor.setCompletionCount(2);
    editor.setCompletionEnabled(true);
    QTRY_VERIFY(list->count() == 2 && list->item(1)->text().contains("AB"));
    QTest::keyClick(&editor, Qt::Key_2, Qt::ControlModifier);
    QCOMPARE(editor.toPlainText(), QStringLiteral("你好AB"));
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor.toPlainText(), QStringLiteral("你好"));
    QTRY_VERIFY(list->count() == 2 && editor.findChild<CandidatePopup *>()->isVisible());
    QTest::keyClick(&editor, Qt::Key_Tab);
    QCOMPARE(editor.toPlainText(), QStringLiteral("你好A"));
    editor.setCompletionEnabled(false);
    editor.clear();
    const int previousRequests = server.requests;
    editor.setCompletionEnabled(true);
    QTest::keyClicks(&editor, "fast");
    QTest::qWait(100);
    QCOMPARE(server.requests, previousRequests);
    QTRY_VERIFY(editor.findChild<CandidatePopup *>()->isVisible());
    QCOMPARE(server.requests, previousRequests + 2);
    server.delay = 150;
    editor.setPlainText("old");
    editor.moveCursor(QTextCursor::End);
    QTRY_VERIFY(server.requests > previousRequests + 2);
    editor.setPlainText("new");
    editor.moveCursor(QTextCursor::End);
    editor.setCompletionEnabled(false);
    QTest::qWait(250);
    QVERIFY(!editor.findChild<CandidatePopup *>()->isVisible());
    QCOMPARE(editor.toPlainText(), QStringLiteral("new"));
    ServiceClient second(directory.path(), server.endpoint().toString());
    QTRY_VERIFY(second.ready());
    QSignalSpy response(&second, &ServiceClient::response);
    const auto id = second.lookup("nihao");
    QVERIFY(response.wait(2000));
    const auto message = response.first().first().toJsonObject();
    QCOMPARE(message.value("id").toString(), id);
    QCOMPARE(message.value("items").toArray().first().toString(), QStringLiteral("你好"));
    editor.document()->setModified(false);
}

void Tests::editorChinesePunctuation()
{
    QTemporaryDir directory;
    QFile dictionaryFile(directory.filePath("base.dict.yaml"));
    QVERIFY(dictionaryFile.open(QIODevice::WriteOnly));
    dictionaryFile.write(QStringLiteral("---\n...\n你好\tni hao\t100\n拟好\tni hao\t50\n").toUtf8());
    dictionaryFile.close();
    ModelStub server;
    server.handler = [](const QJsonObject &) { return QByteArray("[]"); };
    Service service(directory.path(), server.endpoint().toString());
    QString error;
    QVERIFY2(service.start(error), qPrintable(error));
    Editor editor(directory.path(), server.endpoint().toString());
    editor.setCompletionEnabled(false);
    editor.resize(700, 400);
    editor.show();
    editor.activateWindow();
    editor.setFocus();
    QTRY_VERIFY(editor.findChild<ServiceClient *>()->ready());

    const QString punctuation = QStringLiteral(",.!?;:()[]<>\\^_\"\"''");
    QTest::keyClicks(&editor, punctuation);
    QCOMPARE(editor.toPlainText(), QStringLiteral("，。！？；：（）【】《》、……——“”‘’"));
    editor.clear();
    const QString unchanged = QStringLiteral("ABC0123456789 @#$%&*+-=/~`{}");
    QTest::keyClicks(&editor, unchanged);
    QCOMPARE(editor.toPlainText(), unchanged);
    editor.clear();
    QTest::keyClicks(&editor, "abc");
    QCOMPARE(editor.preedit(), QStringLiteral("abc"));
    QTest::keyClick(&editor, Qt::Key_Return);
    QCOMPARE(editor.toPlainText(), QStringLiteral("abc"));

    editor.clear();
    QTest::keyClicks(&editor, "ni'hao");
    QCOMPARE(editor.preedit(), QStringLiteral("ni'hao"));
    auto *list = editor.findChild<QListWidget *>();
    QTRY_VERIFY(list->count() == 2 && list->item(0)->text().contains(QStringLiteral("你好")));
    QTest::keyClick(&editor, Qt::Key_Down);
    QTest::keyClicks(&editor, ",");
    QCOMPARE(editor.toPlainText(), QStringLiteral("拟好，"));
    QVERIFY(editor.preedit().isEmpty());

    editor.clear();
    QTest::keyClicks(&editor, "nihao");
    QTRY_VERIFY(list->count() == 2 && list->item(0)->text().contains(QStringLiteral("你好")));
    QKeyEvent shiftedDigit(QEvent::KeyPress, Qt::Key_1, Qt::ShiftModifier, QStringLiteral("!"));
    QApplication::sendEvent(&editor, &shiftedDigit);
    QCOMPARE(editor.toPlainText(), QStringLiteral("你好！"));
    editor.clear();
    QTest::keyClicks(&editor, "nihao");
    QTRY_VERIFY(list->count() == 2 && list->item(0)->text().contains(QStringLiteral("你好")));
    QTest::keyClick(&editor, Qt::Key_2);
    QCOMPARE(editor.toPlainText(), QStringLiteral("拟好"));

    editor.clear();
    QTest::keyClicks(&editor, "\"");
    QCOMPARE(editor.toPlainText(), QStringLiteral("“"));
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(editor.toPlainText().isEmpty());
    QTest::keyClicks(&editor, "\"\"");
    QCOMPARE(editor.toPlainText(), QStringLiteral("“”"));
    editor.clear();
    editor.setChinese(false);
    QTest::keyClicks(&editor, punctuation + unchanged + QStringLiteral("abc"));
    QCOMPARE(editor.toPlainText(), punctuation + unchanged + QStringLiteral("abc"));
}

void Tests::fullDictionaryAndProcesses()
{
    const QString dictionaryPath = QStringLiteral(KSIPL_SOURCE_DIR "/dicts");
    ModelStub server;
    server.handler = [](const QJsonObject &request) {
        return tokenPayload({{1, " done", 0.8, false}}, request.value("k").toInt());
    };
    const QString endpoint = server.endpoint().toString();
    QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/ksipl");
#ifdef Q_OS_WIN
    executable += QStringLiteral(".exe");
#endif
    const QStringList arguments{QStringLiteral("--service"), QStringLiteral("--dicts"), dictionaryPath,
                                QStringLiteral("--endpoint"), endpoint};
    QProcess process;
    process.start(executable, arguments);
    QVERIFY(process.waitForStarted());
    QLocalSocket probe;
    const auto connectProbe = [&] {
        if (probe.state() == QLocalSocket::ConnectedState)
            return true;
        probe.abort();
        probe.connectToServer(Wire::serverName(dictionaryPath, endpoint));
        return probe.waitForConnected(50);
    };
    QTRY_VERIFY_WITH_TIMEOUT(connectProbe(), 10000);
    QByteArray buffer;
    bool ready = false;
    int entryCount = 0;
    QString error;
    const auto readStatus = [&] {
        buffer += probe.readAll();
        QVector<QJsonObject> messages;
        if (!Wire::extract(buffer, messages, error))
            return;
        for (const auto &message : messages) {
            if (message.value("ready").toBool()) {
                ready = true;
                entryCount = message.value("entries").toInt();
            }
        }
    };
    connect(&probe, &QLocalSocket::readyRead, this, readStatus);
    readStatus();
    QTRY_VERIFY_WITH_TIMEOUT(ready, 30000);
    QVERIFY(entryCount > 1000000);
    qInfo() << "Loaded dictionary entries:" << entryCount;
    QProcess duplicate;
    duplicate.start(executable, arguments);
    QVERIFY(duplicate.waitForStarted());
    QVERIFY(duplicate.waitForFinished(5000));
    QCOMPARE(duplicate.exitCode(), 1);
    {
        ServiceClient first(dictionaryPath, endpoint);
        ServiceClient second(dictionaryPath, endpoint);
        QTRY_VERIFY(first.ready() && second.ready());
        QSignalSpy firstResponse(&first, &ServiceClient::response);
        QSignalSpy secondResponse(&second, &ServiceClient::response);
        first.lookup("nihao");
        second.lookup("zhongguo");
        QTRY_COMPARE(firstResponse.size(), 1);
        QTRY_COMPARE(secondResponse.size(), 1);
        const auto firstItems = firstResponse.first().first().toJsonObject().value("items").toArray();
        const auto secondItems = secondResponse.first().first().toJsonObject().value("items").toArray();
        QVERIFY(firstItems.contains(QStringLiteral("你好")));
        QVERIFY(secondItems.contains(QStringLiteral("中国")));
    }
    disconnect(&probe, nullptr, this, nullptr);
    process.kill();
    QVERIFY(process.waitForFinished(5000));
}

void Tests::windowDocuments()
{
    QTemporaryDir directory;
    QFile dictionaryFile(directory.filePath("base.dict.yaml"));
    QVERIFY(dictionaryFile.open(QIODevice::WriteOnly));
    dictionaryFile.write(QStringLiteral("---\n...\n你好\tni hao\t100\n").toUtf8());
    dictionaryFile.close();
    ModelStub server;
    server.handler = [](const QJsonObject &) { return QByteArray("[]"); };
    Service service(directory.path(), server.endpoint().toString());
    QString error;
    QVERIFY(service.start(error));
    const auto filename = directory.filePath(QStringLiteral("中文文档.txt"));
    QFile file(filename);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const auto original = QByteArray("\xEF\xBB\xBF") + QStringLiteral("你好\r\n世界").toUtf8();
    file.write(original);
    file.close();
    auto *window = new Window(directory.path(), server.endpoint().toString());
    window->show();
    window->openFile(filename);
    auto *tabs = window->findChild<QTabWidget *>();
    QCOMPARE(tabs->count(), 1);
    auto *editor = qobject_cast<Editor *>(tabs->currentWidget());
    editor->setCompletionEnabled(false);
    QCOMPARE(editor->toPlainText(), QStringLiteral("你好\n世界"));
    QTRY_VERIFY(editor->findChild<ServiceClient *>()->ready());
    const auto action = [window](const QString &text) -> QAction * {
        for (auto *candidate : window->findChildren<QAction *>()) {
            if (candidate->text() == text)
                return candidate;
        }
        return nullptr;
    };
    editor->moveCursor(QTextCursor::End);
    editor->insertPlainText(QStringLiteral("！"));
    QVERIFY(editor->document()->isModified());
    QVERIFY(action(QStringLiteral("保存")));
    action(QStringLiteral("保存"))->trigger();
    QVERIFY(!editor->document()->isModified());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original + QStringLiteral("！").toUtf8());
    file.close();
    action(QStringLiteral("新建标签页"))->trigger();
    QCOMPARE(tabs->count(), 2);
    tabs->setCurrentIndex(0);
    action(QStringLiteral("将标签页移到新窗口"))->trigger();
    QCOMPARE(tabs->count(), 1);
    QVector<Window *> windows;
    for (auto *widget : QApplication::topLevelWidgets()) {
        if (auto *candidate = qobject_cast<Window *>(widget))
            windows.append(candidate);
    }
    QCOMPARE(windows.size(), 2);
    for (auto *candidate : windows) {
        if (candidate != window) {
            auto *moved = qobject_cast<Editor *>(candidate->findChild<QTabWidget *>()->currentWidget());
            QCOMPARE(moved->toPlainText(), QStringLiteral("你好\n世界！"));
            QCOMPARE(moved->property("path").toString(), filename);
            QVERIFY(!moved->document()->isModified());
        }
        candidate->close();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QTEST_MAIN(Tests)
#include "tests.moc"
