#pragma once

#include "client.h"
#include <QFrame>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QTimer>

class CandidatePopup : public QFrame {
    Q_OBJECT
public:
    explicit CandidatePopup(QWidget *parent);
    void present(const QString &title, const QStringList &items, bool completion, const QPoint &position);
    void select(int index);
    int selection() const;
signals:
    void chosen(int index);
private:
    QLabel *title_;
    QListWidget *list_;
};

class Editor : public QPlainTextEdit {
    Q_OBJECT
public:
    Editor(const QString &dictionaryPath, const QString &endpoint, QWidget *parent = nullptr);
    void setChinese(bool enabled);
    bool chinese() const { return chinese_; }
    void setCompletionCount(int count);
    void setCompletionEnabled(bool enabled);
    void commitPending();
    QString preedit() const { return preedit_; }

signals:
    void statusMessage(const QString &message);
    void chineseChanged(bool enabled);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    bool event(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void invalidate();
    void contextChanged();
    void updatePreedit();
    void showCandidates();
    void acceptCandidate(int index);
    void acceptCompletion(int index);
    void receive(const QJsonObject &message);
    void positionPopup();
    QString chinesePunctuation(QChar character) const;
    ServiceClient *client_;
    CandidatePopup *popup_;
    QTimer lookupTimer_;
    QTimer completionTimer_;
    QString preedit_;
    QStringList candidates_;
    QJsonArray completions_;
    QString lookupId_;
    QString completionId_;
    QString completionContext_;
    int completionPosition_ = -1;
    int candidatePage_ = 0;
    int count_ = 5;
    bool chinese_ = true;
    bool completionEnabled_ = true;
    bool systemPreedit_ = false;
};
