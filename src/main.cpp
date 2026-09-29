#include "service.h"
#include "window.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <cstdio>
#include <memory>

int main(int argc, char *argv[])
{
    bool serviceMode = false;
    for (int index = 1; index < argc; ++index)
        serviceMode |= QByteArray(argv[index]) == "--service";
    std::unique_ptr<QCoreApplication> application;
    if (serviceMode)
        application = std::make_unique<QCoreApplication>(argc, argv);
    else
        application = std::make_unique<QApplication>(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Ksipl"));
    QCoreApplication::setApplicationName(QStringLiteral("Ksipl"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Qt 中文拼音编辑器与两步模型补全"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("service"), QStringLiteral("运行共享后台服务")});
    parser.addOption({QStringLiteral("dicts"), QStringLiteral("Rime YAML 词库目录"), QStringLiteral("directory")});
    parser.addOption({QStringLiteral("endpoint"), QStringLiteral("模型 next-token 接口"), QStringLiteral("url"),
                      QStringLiteral("http://125.220.157.15:1237/v1/next-token")});
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("打开的 UTF-8 文件"), QStringLiteral("[files…]"));
    parser.process(*application);
    QString dictionaryPath = parser.value(QStringLiteral("dicts"));
    if (dictionaryPath.isEmpty()) {
        const QString binary = QCoreApplication::applicationDirPath();
        for (const auto &candidate : {binary + QStringLiteral("/dicts"),
                                     binary + QStringLiteral("/../dicts"),
                                     QDir::current().filePath(QStringLiteral("dicts"))}) {
            if (QDir(candidate).exists()) {
                dictionaryPath = candidate;
                break;
            }
        }
    }
    dictionaryPath = QFileInfo(dictionaryPath.isEmpty() ? QStringLiteral("dicts") : dictionaryPath).absoluteFilePath();
    const QString endpoint = parser.value(QStringLiteral("endpoint"));
    const QUrl url(endpoint);
    if (!QDir(dictionaryPath).exists() || !url.isValid() || url.host().isEmpty()
        || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))) {
        const QString error = QStringLiteral("请通过 --dicts 指定现有词库目录，并通过 --endpoint 指定 HTTP(S) 接口。");
        if (!serviceMode)
            QMessageBox::critical(nullptr, QStringLiteral("启动失败"), error);
        else
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
        return 1;
    }
    if (serviceMode) {
        Service service(dictionaryPath, endpoint);
        QString error;
        if (!service.start(error))
            return 1;
        return application->exec();
    }
    auto *window = new Window(dictionaryPath, endpoint);
    for (const auto &path : parser.positionalArguments())
        window->openFile(path);
    window->show();
    return application->exec();
}
