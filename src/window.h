#pragma once

#include <QMainWindow>
#include <QTabWidget>

class Editor;
class QLabel;
class QAction;

class Window : public QMainWindow {
    Q_OBJECT
public:
    Window(QString dictionaryPath, QString endpoint, QWidget *parent = nullptr);
    void openFile(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    Editor *addTab(const QString &text = {}, const QString &path = {});
    Editor *currentEditor() const;
    bool save(Editor *editor, bool saveAs = false);
    bool confirmClose(Editor *editor);
    void closeTab(int index);
    void refreshTitle(Editor *editor);
    void updateCursorStatus();
    void newWindow();
    void detachTab();
    QString dictionaryPath_;
    QString endpoint_;
    QTabWidget *tabs_;
    QLabel *status_;
    QLabel *cursorStatus_;
    QAction *chineseAction_;
    int count_ = 5;
    bool completionEnabled_ = true;
};
