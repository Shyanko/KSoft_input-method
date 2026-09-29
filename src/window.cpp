#include "window.h"
#include "editor.h"

#include <QAction>
#include <QCloseEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolBar>

Window::Window(QString dictionaryPath, QString endpoint, QWidget *parent)
    : QMainWindow(parent), dictionaryPath_(std::move(dictionaryPath)), endpoint_(std::move(endpoint)),
      tabs_(new QTabWidget(this)), status_(new QLabel(this)), cursorStatus_(new QLabel(this))
{
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1060, 730);
    setMinimumSize(640, 420);
    setCentralWidget(tabs_);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    QSettings settings;
    count_ = qBound(1, settings.value(QStringLiteral("completion/count"), 5).toInt(), 7);
    completionEnabled_ = settings.value(QStringLiteral("completion/enabled"), true).toBool();

    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    fileMenu->addAction(QStringLiteral("新建标签页"), this, [this] { addTab(); })->setShortcut(QKeySequence::AddTab);
    fileMenu->addAction(QStringLiteral("新建窗口"), this, &Window::newWindow)->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
    fileMenu->addAction(QStringLiteral("打开…"), this, [this] {
        const auto paths = QFileDialog::getOpenFileNames(this, QStringLiteral("打开文本文件"), {},
            QStringLiteral("文本文件 (*.txt *.md *.cpp *.h *.json *.yaml);;所有文件 (*)"));
        for (const auto &path : paths)
            openFile(path);
    })->setShortcut(QKeySequence::Open);
    fileMenu->addAction(QStringLiteral("保存"), this, [this] { save(currentEditor()); })->setShortcut(QKeySequence::Save);
    fileMenu->addAction(QStringLiteral("另存为…"), this, [this] { save(currentEditor(), true); })->setShortcut(QKeySequence::SaveAs);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("将标签页移到新窗口"), this, &Window::detachTab);
    fileMenu->addAction(QStringLiteral("关闭标签页"), this, [this] { closeTab(tabs_->currentIndex()); })->setShortcut(QKeySequence::Close);
    fileMenu->addAction(QStringLiteral("关闭窗口"), this, &QWidget::close)->setShortcut(QKeySequence(QStringLiteral("Alt+F4")));

    auto *editMenu = menuBar()->addMenu(QStringLiteral("编辑(&E)"));
    editMenu->addAction(QStringLiteral("撤销"), this, [this] { currentEditor()->undo(); })->setShortcut(QKeySequence::Undo);
    editMenu->addAction(QStringLiteral("重做"), this, [this] { currentEditor()->redo(); })->setShortcut(QKeySequence::Redo);
    editMenu->addSeparator();
    editMenu->addAction(QStringLiteral("剪切"), this, [this] { currentEditor()->cut(); })->setShortcut(QKeySequence::Cut);
    editMenu->addAction(QStringLiteral("复制"), this, [this] { currentEditor()->copy(); })->setShortcut(QKeySequence::Copy);
    editMenu->addAction(QStringLiteral("粘贴"), this, [this] { currentEditor()->paste(); })->setShortcut(QKeySequence::Paste);
    editMenu->addAction(QStringLiteral("全选"), this, [this] { currentEditor()->selectAll(); })->setShortcut(QKeySequence::SelectAll);

    auto *toolbar = addToolBar(QStringLiteral("输入设置"));
    toolbar->setMovable(false);
    chineseAction_ = toolbar->addAction(QStringLiteral("中文拼音"));
    chineseAction_->setCheckable(true);
    chineseAction_->setChecked(true);
    chineseAction_->setToolTip(QStringLiteral("F2 切换中文拼音（中文标点）/ 英文与系统输入法"));
    connect(chineseAction_, &QAction::toggled, this, [this](bool enabled) {
        if (auto *editor = currentEditor()) {
            editor->setChinese(enabled);
            editor->setFocus();
        }
    });
    toolbar->addSeparator();
    auto *completionAction = toolbar->addAction(QStringLiteral("智能补全"));
    completionAction->setCheckable(true);
    completionAction->setChecked(completionEnabled_);
    connect(completionAction, &QAction::toggled, this, [this](bool enabled) {
        completionEnabled_ = enabled;
        QSettings().setValue(QStringLiteral("completion/enabled"), enabled);
        for (int index = 0; index < tabs_->count(); ++index)
            qobject_cast<Editor *>(tabs_->widget(index))->setCompletionEnabled(enabled);
        currentEditor()->setFocus();
    });
    toolbar->addWidget(new QLabel(QStringLiteral("   候选数量 "), toolbar));
    auto *count = new QSpinBox(toolbar);
    count->setRange(1, 7);
    count->setValue(count_);
    count->setToolTip(QStringLiteral("同时比较 1～2 token 路径，按联合概率降序排列"));
    toolbar->addWidget(count);
    connect(count, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        count_ = value;
        QSettings().setValue(QStringLiteral("completion/count"), value);
        for (int index = 0; index < tabs_->count(); ++index)
            qobject_cast<Editor *>(tabs_->widget(index))->setCompletionCount(value);
    });
    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(QStringLiteral("  F2 中/英    Tab 首项    Ctrl+1～7 补全"), toolbar));

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    helpMenu->addAction(QStringLiteral("使用说明"), this, [this] {
        QMessageBox::information(this, QStringLiteral("Ksipl · 简化中文输入法"),
            QStringLiteral("中文模式：输入全拼，空格或数字 1～7 选词；上下键选中，PgUp/PgDn 或 -/= 翻页。\n"
                           "Enter 提交原始拼音，Esc 取消拼音，F2 切换英文 / 系统输入法。\n"
                           "中文模式使用中文标点；输入标点会先提交当前候选。数字、字母和空格保持原样。\n\n"
                           "无拼音预编辑时，停顿 350ms 显示灰色补全。\n"
                           "Tab 接受第一项；Ctrl+1～7 接受对应项；Esc 隐藏。\n"
                           "补全比较 1～2 token 路径，概率为沿路径各步概率的乘积。\n\n"
                           "智能补全开启时，光标前最多 4096 个字符会发送到配置的模型服务。\n"
                           "模型服务：%1\n\n文件以 UTF-8 保存。").arg(endpoint_));
    });
    statusBar()->addWidget(status_, 1);
    statusBar()->addPermanentWidget(cursorStatus_);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &Window::closeTab);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
        if (auto *editor = currentEditor()) {
            const QSignalBlocker blocker(chineseAction_);
            chineseAction_->setChecked(editor->chinese());
            refreshTitle(editor);
            updateCursorStatus();
            editor->setFocus();
        }
    });
    setStyleSheet(QStringLiteral("QMainWindow { background: #f2f5f9; } QToolBar { spacing: 8px; padding: 9px; border-bottom: 1px solid #e0e5ed; } QTabBar::tab { padding: 10px 20px; } QStatusBar { color: #566275; }"));
    addTab();
    status_->setText(QStringLiteral("正在启动后台服务…"));
}

Editor *Window::currentEditor() const
{
    return qobject_cast<Editor *>(tabs_->currentWidget());
}

Editor *Window::addTab(const QString &text, const QString &path)
{
    auto *editor = new Editor(dictionaryPath_, endpoint_, tabs_);
    editor->setProperty("path", path);
    editor->setProperty("newline", QStringLiteral("LF"));
    editor->setCompletionCount(count_);
    editor->setCompletionEnabled(completionEnabled_);
    editor->setPlainText(text);
    editor->document()->setModified(false);
    const int index = tabs_->addTab(editor, QStringLiteral("未命名"));
    connect(editor->document(), &QTextDocument::modificationChanged, this, [this, editor] { refreshTitle(editor); });
    connect(editor, &Editor::statusMessage, this, [this, editor](const QString &message) {
        if (editor == currentEditor())
            status_->setText(message);
    });
    connect(editor, &Editor::chineseChanged, this, [this, editor](bool enabled) {
        if (editor == currentEditor()) {
            const QSignalBlocker blocker(chineseAction_);
            chineseAction_->setChecked(enabled);
        }
    });
    connect(editor, &Editor::cursorPositionChanged, this, &Window::updateCursorStatus);
    connect(editor, &Editor::textChanged, this, &Window::updateCursorStatus);
    tabs_->setCurrentIndex(index);
    refreshTitle(editor);
    editor->setFocus();
    return editor;
}

void Window::openFile(const QString &path)
{
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    for (int index = 0; index < tabs_->count(); ++index) {
        if (tabs_->widget(index)->property("path").toString() == absolutePath) {
            tabs_->setCurrentIndex(index);
            return;
        }
    }
    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("无法打开文件"), file.errorString());
        return;
    }
    const auto data = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        QMessageBox::warning(this, QStringLiteral("无法读取文件"), file.errorString());
        return;
    }
    const bool bom = data.startsWith("\xEF\xBB\xBF");
    const auto content = bom ? data.mid(3) : data;
    const QString text = QString::fromUtf8(content);
    if (text.toUtf8() != content) {
        QMessageBox::warning(this, QStringLiteral("不支持的编码"), QStringLiteral("请将文件转换为 UTF-8 后打开，以免保存时损坏原文。"));
        return;
    }
    Editor *emptyTab = currentEditor();
    const bool replace = tabs_->count() == 1 && emptyTab->toPlainText().isEmpty()
        && !emptyTab->document()->isModified() && emptyTab->property("path").toString().isEmpty();
    auto *editor = addTab(text, absolutePath);
    editor->setProperty("bom", bom);
    editor->setProperty("newline", data.contains("\r\n") ? QStringLiteral("CRLF") : QStringLiteral("LF"));
    if (replace) {
        tabs_->removeTab(tabs_->indexOf(emptyTab));
        emptyTab->deleteLater();
    }
}

bool Window::save(Editor *editor, bool saveAs)
{
    if (!editor)
        return false;
    editor->commitPending();
    QString path = editor->property("path").toString();
    if (saveAs || path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, QStringLiteral("保存文本文件"), path,
            QStringLiteral("文本文件 (*.txt);;所有文件 (*)"));
        if (path.isEmpty())
            return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), file.errorString());
        return false;
    }
    QString text = editor->toPlainText();
    if (editor->property("newline").toString() == QStringLiteral("CRLF"))
        text.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    QByteArray data = text.toUtf8();
    if (editor->property("bom").toBool())
        data.prepend("\xEF\xBB\xBF");
    if (file.write(data) != data.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), file.errorString());
        return false;
    }
    editor->setProperty("path", QFileInfo(path).absoluteFilePath());
    editor->document()->setModified(false);
    refreshTitle(editor);
    status_->setText(QStringLiteral("已保存 · ") + path);
    return true;
}

bool Window::confirmClose(Editor *editor)
{
    editor->commitPending();
    if (!editor->document()->isModified())
        return true;
    tabs_->setCurrentWidget(editor);
    const auto answer = QMessageBox::question(this, QStringLiteral("保存更改？"),
        QStringLiteral("是否保存“%1”的更改？").arg(tabs_->tabText(tabs_->indexOf(editor))),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    return answer == QMessageBox::Discard || (answer == QMessageBox::Save && save(editor));
}

void Window::closeTab(int index)
{
    auto *editor = qobject_cast<Editor *>(tabs_->widget(index));
    if (!editor || !confirmClose(editor))
        return;
    tabs_->removeTab(index);
    editor->deleteLater();
    if (tabs_->count() == 0)
        addTab();
}

void Window::refreshTitle(Editor *editor)
{
    const QString path = editor->property("path").toString();
    const QString title = (path.isEmpty() ? QStringLiteral("未命名") : QFileInfo(path).fileName())
        + (editor->document()->isModified() ? QStringLiteral(" ●") : QString());
    tabs_->setTabText(tabs_->indexOf(editor), title);
    tabs_->setTabToolTip(tabs_->indexOf(editor), path);
    if (editor == currentEditor())
        setWindowTitle(title + QStringLiteral(" — Ksipl"));
}

void Window::updateCursorStatus()
{
    if (const auto *editor = currentEditor()) {
        const auto cursor = editor->textCursor();
        cursorStatus_->setText(QStringLiteral("行 %1，列 %2    UTF-8").arg(cursor.blockNumber() + 1).arg(cursor.positionInBlock() + 1));
    }
}

void Window::newWindow()
{
    auto *window = new Window(dictionaryPath_, endpoint_);
    window->show();
}

void Window::detachTab()
{
    auto *source = currentEditor();
    source->commitPending();
    auto *window = new Window(dictionaryPath_, endpoint_);
    auto *target = window->currentEditor();
    target->setPlainText(source->toPlainText());
    for (const auto &property : {"path", "newline", "bom"})
        target->setProperty(property, source->property(property));
    target->document()->setModified(source->document()->isModified());
    target->setChinese(source->chinese());
    auto cursor = target->textCursor();
    cursor.setPosition(source->textCursor().position());
    target->setTextCursor(cursor);
    window->refreshTitle(target);
    tabs_->removeTab(tabs_->indexOf(source));
    source->deleteLater();
    if (tabs_->count() == 0)
        addTab();
    window->show();
}

void Window::closeEvent(QCloseEvent *event)
{
    for (int index = 0; index < tabs_->count(); ++index) {
        if (!confirmClose(qobject_cast<Editor *>(tabs_->widget(index)))) {
            event->ignore();
            return;
        }
    }
    event->accept();
}
