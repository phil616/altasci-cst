#pragma once
#include <QAbstractScrollArea>
#include <QColor>
#include <QStringDecoder>
#include <QVector>
#include <deque>

namespace cst {
struct TerminalCell { QString text = " "; QColor foreground = QColor("#E2E8F0"); QColor background = QColor("#0F172A"); bool continuation = false; };
class TerminalScreen {
public:
    TerminalScreen();
    void resize(int columns, int rows);
    void feed(const QByteArray &bytes);
    QString plainText() const;
    int columns() const { return columns_; }
    int rows() const { return rows_; }
    int cursorColumn() const { return x_; }
    int cursorRow() const { return y_; }
    bool cursorVisible() const { return cursorVisible_; }
    bool bracketedPaste() const { return bracketedPaste_; }
    int historySize() const { return int(history_.size()); }
    const QVector<TerminalCell> &line(int index) const;
    std::function<void(QByteArray)> reply;
private:
    void character(char32_t);
    void csi(QChar final);
    void newline();
    void scroll(int direction);
    TerminalCell blank() const;
    QVector<TerminalCell> blankLine() const;
    QVector<QVector<TerminalCell>> screen_, primary_;
    std::deque<QVector<TerminalCell>> history_;
    int columns_ = 100, rows_ = 30, x_ = 0, y_ = 0, savedX_ = 0, savedY_ = 0, top_ = 0, bottom_ = 29;
    bool wrap_ = true, pendingWrap_ = false, cursorVisible_ = true, bracketedPaste_ = false, alternate_ = false, bold_ = false, inverse_ = false;
    QColor fg_ = QColor("#E2E8F0"), bg_ = QColor("#0F172A");
    enum class Parse { Text, Escape, Csi, Osc, OscEscape, Charset } parse_ = Parse::Text;
    QString sequence_;
    QStringDecoder decoder_{QStringDecoder::Utf8};
};
class TerminalView final : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit TerminalView(QWidget *parent = nullptr);
    void feed(const QByteArray &bytes);
    void reset();
    QString plainText() const { return screen_.plainText(); }
    void setInputEnabled(bool enabled) { inputEnabled_ = enabled; }
    void paste();
signals:
    void input(QByteArray bytes);
    void terminalResized(int columns, int rows);
protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void inputMethodEvent(QInputMethodEvent *) override;
private:
    void updateScroll();
    TerminalScreen screen_;
    int cellWidth_ = 9, cellHeight_ = 18;
    bool inputEnabled_ = true;
};
}
