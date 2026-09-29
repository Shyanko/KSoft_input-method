#include "editor.h"

#include <QApplication>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QScrollBar>
#include <QScreen>
#include <QVBoxLayout>

CandidatePopup::CandidatePopup(QWidget *parent)
    : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint),
      title_(new QLabel(this)), list_(new QListWidget(this))
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setTextElideMode(Qt::ElideRight);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(5);
    layout->addWidget(title_);
    layout->addWidget(list_);
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        emit chosen(list_->row(item));
    });
}

void CandidatePopup::present(const QString &title, const QStringList &items, bool completion, const QPoint &position)
{
    title_->setText(title);
    list_->clear();
    for (int index = 0; index < items.size(); ++index) {
        auto *item = new QListWidgetItem(QStringLiteral("%1   %2").arg(index + 1).arg(items.at(index)), list_);
        item->setToolTip(items.at(index));
        item->setSizeHint(QSize(360, 28));
        if (completion)
            item->setForeground(QColor(QStringLiteral("#858b96")));
    }
    list_->setStyleSheet(completion
        ? QStringLiteral("QListWidget { border: none; background: #f8f9fc; } QListWidget::item:selected { background: #edf0f5; color: #737b88; }")
        : QStringLiteral("QListWidget { border: none; background: #f8f9fc; } QListWidget::item:selected { background: #dce9ff; color: #153b78; }"));
    setStyleSheet(QStringLiteral("CandidatePopup { background: #f8f9fc; border: 1px solid #cdd5e2; border-radius: 6px; } QLabel { color: #485568; background: transparent; border: none; }"));
    list_->setFixedHeight(qMax(1, items.size()) * 28 + 4);
    setFixedWidth(410);
    adjustSize();
    list_->setCurrentRow(0);
    const QScreen *screen = QGuiApplication::screenAt(position);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen->availableGeometry();
    QPoint location = position;
    location.setX(qBound(available.left(), location.x(), qMax(available.left(), available.right() - width())));
    if (location.y() + height() > available.bottom())
        location.setY(qMax(available.top(), position.y() - height() - 26));
    move(location);
    show();
}

void CandidatePopup::select(int index)
{
    list_->setCurrentRow(qBound(0, index, qMax(0, list_->count() - 1)));
}

int CandidatePopup::selection() const
{
    return list_->currentRow();
}

Editor::Editor(const QString &dictionaryPath, const QString &endpoint, QWidget *parent)
    : QPlainTextEdit(parent), client_(new ServiceClient(dictionaryPath, endpoint, this)),
      popup_(new CandidatePopup(this))
{
    setObjectName(QStringLiteral("editor"));
    QFont editorFont(QStringLiteral("Microsoft YaHei UI"), 12);
    editorFont.setStyleHint(QFont::Monospace);
    setFont(editorFont);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    setPlaceholderText(QStringLiteral("开始书写…  F2 切换中英文 · Tab 接受第一项补全 · Ctrl+1～7 选择补全"));
    setStyleSheet(QStringLiteral("QPlainTextEdit { border: none; padding: 18px; background: #ffffff; color: #202838; selection-background-color: #d5e5ff; }"));
    setAttribute(Qt::WA_InputMethodEnabled, false);
    lookupTimer_.setSingleShot(true);
    lookupTimer_.setInterval(35);
    completionTimer_.setSingleShot(true);
    completionTimer_.setInterval(350);
    connect(&lookupTimer_, &QTimer::timeout, this, [this] {
        if (!preedit_.isEmpty() && client_->ready())
            lookupId_ = client_->lookup(preedit_);
    });
    connect(&completionTimer_, &QTimer::timeout, this, [this] {
        if (!hasFocus() || !preedit_.isEmpty() || systemPreedit_ || !completionEnabled_
            || !client_->ready() || textCursor().hasSelection())
            return;
        completionPosition_ = textCursor().position();
        completionContext_ = toPlainText().left(completionPosition_).right(4096);
        completionId_ = client_->complete(completionContext_, count_);
        emit statusMessage(QStringLiteral("正在获取补全…"));
    });
    connect(this, &QPlainTextEdit::textChanged, this, &Editor::contextChanged);
    connect(this, &QPlainTextEdit::selectionChanged, this, &Editor::contextChanged);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (!preedit_.isEmpty())
            commitPending();
        contextChanged();
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &Editor::positionPopup);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, &Editor::positionPopup);
    connect(client_, &ServiceClient::response, this, &Editor::receive);
    connect(client_, &ServiceClient::statusChanged, this, [this](bool ready, const QString &message) {
        emit statusMessage(message);
        invalidate();
        if (ready) {
            if (preedit_.isEmpty())
                contextChanged();
            else
                lookupTimer_.start();
        }
    });
    connect(popup_, &CandidatePopup::chosen, this, [this](int index) {
        if (!preedit_.isEmpty())
            acceptCandidate(index);
        else
            acceptCompletion(index);
    });
}

void Editor::setChinese(bool enabled)
{
    if (chinese_ == enabled)
        return;
    commitPending();
    chinese_ = enabled;
    setAttribute(Qt::WA_InputMethodEnabled, !enabled);
    emit chineseChanged(enabled);
    emit statusMessage(enabled ? QStringLiteral("中文拼音 · 中文标点 · 空格/数字选词 · Enter 输入原文 · Esc 取消")
                               : QStringLiteral("英文 / 系统输入法"));
}

void Editor::setCompletionCount(int count)
{
    count_ = qBound(1, count, 7);
    contextChanged();
}

void Editor::setCompletionEnabled(bool enabled)
{
    completionEnabled_ = enabled;
    contextChanged();
}

void Editor::invalidate()
{
    lookupTimer_.stop();
    completionTimer_.stop();
    lookupId_.clear();
    completionId_.clear();
    completions_ = {};
    client_->cancel();
    popup_->hide();
}

void Editor::contextChanged()
{
    invalidate();
    if (preedit_.isEmpty() && !systemPreedit_ && completionEnabled_ && hasFocus())
        completionTimer_.start();
}

void Editor::updatePreedit()
{
    invalidate();
    candidates_.clear();
    candidatePage_ = 0;
    QInputMethodEvent event(preedit_, {});
    QPlainTextEdit::inputMethodEvent(&event);
    if (!preedit_.isEmpty()) {
        popup_->present(preedit_ + QStringLiteral("   · 拼音"), {QStringLiteral("正在查词…")}, false,
                        viewport()->mapToGlobal(cursorRect().bottomLeft()));
        lookupTimer_.start();
    } else {
        contextChanged();
    }
}

void Editor::commitPending()
{
    if (preedit_.isEmpty())
        return;
    const QString text = preedit_;
    preedit_.clear();
    candidates_.clear();
    QInputMethodEvent event;
    event.setCommitString(text);
    QPlainTextEdit::inputMethodEvent(&event);
    contextChanged();
}

void Editor::showCandidates()
{
    QStringList page = candidates_.mid(candidatePage_ * 7, 7);
    if (page.isEmpty())
        page.append(QStringLiteral("无候选 · Enter 输入拼音"));
    popup_->present(QStringLiteral("%1    %2/%3  · PgUp/PgDn 翻页").arg(preedit_)
                        .arg(candidatePage_ + 1).arg(qMax(1, (candidates_.size() + 6) / 7)),
                    page, false, viewport()->mapToGlobal(cursorRect().bottomLeft()));
}

void Editor::acceptCandidate(int index)
{
    const int absoluteIndex = candidatePage_ * 7 + index;
    if (absoluteIndex < 0 || absoluteIndex >= candidates_.size())
        return;
    const QString text = candidates_.at(absoluteIndex);
    preedit_.clear();
    candidates_.clear();
    QInputMethodEvent event;
    event.setCommitString(text);
    QPlainTextEdit::inputMethodEvent(&event);
    contextChanged();
}

void Editor::acceptCompletion(int index)
{
    if (index < 0 || index >= completions_.size() || textCursor().position() != completionPosition_
        || textCursor().hasSelection() || !preedit_.isEmpty())
        return;
    if (toPlainText().left(completionPosition_).right(4096) != completionContext_)
        return;
    const QString text = completions_.at(index).toObject().value(QStringLiteral("text")).toString();
    invalidate();
    auto cursor = textCursor();
    cursor.beginEditBlock();
    cursor.insertText(text);
    cursor.endEditBlock();
    setTextCursor(cursor);
}

void Editor::receive(const QJsonObject &message)
{
    const QString id = message.value(QStringLiteral("id")).toString();
    const QString type = message.value(QStringLiteral("type")).toString();
    const QString error = message.value(QStringLiteral("error")).toString();
    if (type == QStringLiteral("lookup") && id == lookupId_ && !preedit_.isEmpty() && hasFocus()) {
        candidates_.clear();
        for (const auto &item : message.value(QStringLiteral("items")).toArray())
            candidates_.append(item.toString());
        candidatePage_ = 0;
        showCandidates();
        if (!error.isEmpty())
            emit statusMessage(error);
    } else if (type == QStringLiteral("complete") && id == completionId_ && preedit_.isEmpty()
               && !systemPreedit_ && hasFocus() && completionEnabled_
               && textCursor().position() == completionPosition_ && !textCursor().hasSelection()
               && toPlainText().left(completionPosition_).right(4096) == completionContext_) {
        if (!error.isEmpty()) {
            emit statusMessage(error);
            return;
        }
        completions_ = message.value(QStringLiteral("items")).toArray();
        if (completions_.isEmpty()) {
            emit statusMessage(QStringLiteral("暂无可插入的补全"));
            return;
        }
        positionPopup();
        emit statusMessage(QStringLiteral("Tab 接受第一项 · Ctrl+1～7 选择补全 · Esc 隐藏"));
    }
}

void Editor::positionPopup()
{
    if (!hasFocus())
        return;
    if (!preedit_.isEmpty()) {
        showCandidates();
    } else if (!completions_.isEmpty()) {
        QStringList labels;
        for (const auto &value : completions_) {
            const auto item = value.toObject();
            QString text = item.value(QStringLiteral("text")).toString();
            text.replace(QLatin1Char('\n'), QStringLiteral("↵"));
            text.replace(QLatin1Char('\t'), QStringLiteral("⇥"));
            labels.append(QStringLiteral("%1    %2%").arg(text)
                .arg(item.value(QStringLiteral("probability")).toDouble() * 100, 0, 'f', 2));
        }
        popup_->present(QStringLiteral("续写建议    Tab / Ctrl+数字"), labels, true,
                        viewport()->mapToGlobal(cursorRect().bottomLeft()));
    }
}

bool Editor::event(QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab && key->modifiers() == Qt::NoModifier) {
            keyPressEvent(key);
            return true;
        }
    }
    return QPlainTextEdit::event(event);
}

QString Editor::chinesePunctuation(QChar character) const
{
    switch (character.unicode()) {
    case ',': return QStringLiteral("，");
    case '.': return QStringLiteral("。");
    case '!': return QStringLiteral("！");
    case '?': return QStringLiteral("？");
    case ';': return QStringLiteral("；");
    case ':': return QStringLiteral("：");
    case '(': return QStringLiteral("（");
    case ')': return QStringLiteral("）");
    case '\\': return QStringLiteral("、");
    case '[': return QStringLiteral("【");
    case ']': return QStringLiteral("】");
    case '<': return QStringLiteral("《");
    case '>': return QStringLiteral("》");
    case '^': return QStringLiteral("……");
    case '_': return QStringLiteral("——");
    case '\'':
    case '"': {
        const QChar opening = character == QLatin1Char('"') ? QChar(0x201c) : QChar(0x2018);
        const QChar closing = character == QLatin1Char('"') ? QChar(0x201d) : QChar(0x2019);
        const QString preceding = toPlainText().left(textCursor().selectionStart());
        return QString(preceding.count(opening) > preceding.count(closing) ? closing : opening);
    }
    default:
        return {};
    }
}

void Editor::keyPressEvent(QKeyEvent *event)
{
    const int key = event->key();
    const auto modifiers = event->modifiers();
    if (key == Qt::Key_F2 && modifiers == Qt::NoModifier) {
        setChinese(!chinese_);
        return;
    }
    if (modifiers == Qt::ControlModifier && key >= Qt::Key_1 && key <= Qt::Key_7
        && preedit_.isEmpty() && !completions_.isEmpty()) {
        acceptCompletion(key - Qt::Key_1);
        return;
    }
    if (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        commitPending();
        QPlainTextEdit::keyPressEvent(event);
        return;
    }
    if (key == Qt::Key_Escape) {
        preedit_.clear();
        updatePreedit();
        invalidate();
        return;
    }
    if (key == Qt::Key_Tab && !completions_.isEmpty() && modifiers == Qt::NoModifier) {
        acceptCompletion(0);
        return;
    }
    if (chinese_ && event->text().size() == 1) {
        const QChar character = event->text().at(0);
        if ((character >= QLatin1Char('a') && character <= QLatin1Char('z'))
            || (character == QLatin1Char('\'') && !preedit_.isEmpty())) {
            if (preedit_.size() < 128) {
                preedit_ += character;
                updatePreedit();
            }
            return;
        }
    }
    const QString punctuation = chinese_ && event->text().size() == 1
        ? chinesePunctuation(event->text().at(0)) : QString();
    if (!preedit_.isEmpty()) {
        if (key == Qt::Key_Backspace) {
            preedit_.chop(1);
            updatePreedit();
            return;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            commitPending();
            return;
        }
        if (key >= Qt::Key_1 && key <= Qt::Key_7 && !(modifiers & Qt::ShiftModifier)) {
            acceptCandidate(key - Qt::Key_1);
            return;
        }
        if (key == Qt::Key_Space || key == Qt::Key_Tab) {
            if (candidates_.isEmpty())
                commitPending();
            else
                acceptCandidate(qMax(0, popup_->selection()));
            return;
        }
        if (key == Qt::Key_PageDown || key == Qt::Key_PageUp
            || (!(modifiers & Qt::ShiftModifier) && (key == Qt::Key_Equal || key == Qt::Key_Minus))) {
            const int direction = key == Qt::Key_PageDown || key == Qt::Key_Equal ? 1 : -1;
            candidatePage_ = qBound(0, candidatePage_ + direction, qMax(0, (candidates_.size() - 1) / 7));
            showCandidates();
            return;
        }
        if (key == Qt::Key_Down || key == Qt::Key_Up) {
            popup_->select(popup_->selection() + (key == Qt::Key_Down ? 1 : -1));
            return;
        }
        if (!punctuation.isEmpty() && !candidates_.isEmpty())
            acceptCandidate(qMax(0, popup_->selection()));
        else
            commitPending();
    }
    if (!punctuation.isEmpty()) {
        QKeyEvent converted(event->type(), key, modifiers, punctuation, event->isAutoRepeat(), event->count());
        QPlainTextEdit::keyPressEvent(&converted);
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

void Editor::inputMethodEvent(QInputMethodEvent *event)
{
    if (chinese_) {
        event->ignore();
        return;
    }
    systemPreedit_ = !event->preeditString().isEmpty();
    QPlainTextEdit::inputMethodEvent(event);
    contextChanged();
}

void Editor::focusOutEvent(QFocusEvent *event)
{
    commitPending();
    invalidate();
    QPlainTextEdit::focusOutEvent(event);
}

void Editor::focusInEvent(QFocusEvent *event)
{
    QPlainTextEdit::focusInEvent(event);
    contextChanged();
}

void Editor::mousePressEvent(QMouseEvent *event)
{
    commitPending();
    invalidate();
    QPlainTextEdit::mousePressEvent(event);
    contextChanged();
}

void Editor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    positionPopup();
}
