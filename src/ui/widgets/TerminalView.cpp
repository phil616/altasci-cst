#include "TerminalView.h"
#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <algorithm>

namespace cst {
namespace {
QColor indexed(int index) {
    static const char *colors[] = {"#1E293B", "#EF4444", "#22C55E", "#EAB308", "#3B82F6", "#A855F7", "#06B6D4", "#CBD5E1", "#64748B", "#F87171", "#4ADE80", "#FACC15", "#60A5FA", "#C084FC", "#22D3EE", "#F8FAFC"};
    if (index < 16) return QColor(colors[std::clamp(index, 0, 15)]);
    if (index >= 232) { const int level = 8 + std::clamp(index - 232, 0, 23) * 10; return QColor(level, level, level); }
    const int n = index - 16;
    const auto component = [](int c) { return c ? 55 + c * 40 : 0; };
    return QColor(component(n / 36), component((n / 6) % 6), component(n % 6));
}
int widthOf(char32_t c) {
    const auto category = QChar::category(c);
    if (category == QChar::Mark_NonSpacing || category == QChar::Mark_Enclosing || c == 0x200d || c == 0xfe0f) return 0;
    if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7a3) ||
        (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe10 && c <= 0xfe6f) || (c >= 0xff01 && c <= 0xff60) || (c >= 0x1f300 && c <= 0x1faff) || c >= 0x20000) return 2;
    return 1;
}
}
TerminalScreen::TerminalScreen() { screen_.fill(blankLine(), rows_); }
TerminalCell TerminalScreen::blank() const { return {" ", inverse_ ? bg_ : fg_, inverse_ ? fg_ : bg_, false}; }
QVector<TerminalCell> TerminalScreen::blankLine() const { return QVector<TerminalCell>(columns_, blank()); }
void TerminalScreen::resize(int columns, int rows) {
    columns_ = std::clamp(columns, 2, 500); rows_ = std::clamp(rows, 2, 200);
    screen_.resize(rows_); for (auto &line : screen_) { const auto old = line.size(); line.resize(columns_); for (qsizetype i = old; i < line.size(); ++i) line[i] = blank(); }
    x_ = std::min(x_, columns_ - 1); y_ = std::min(y_, rows_ - 1); top_ = 0; bottom_ = rows_ - 1; pendingWrap_ = false;
}
const QVector<TerminalCell> &TerminalScreen::line(int index) const {
    if (index < historySize()) return history_[size_t(index)];
    return screen_[std::clamp(index - historySize(), 0, rows_ - 1)];
}
void TerminalScreen::scroll(int direction) {
    if (direction > 0) {
        if (!alternate_ && top_ == 0 && bottom_ == rows_ - 1) { history_.push_back(screen_[0]); if (history_.size() > 5000) history_.pop_front(); }
        for (int row = top_; row < bottom_; ++row) screen_[row] = screen_[row + 1];
        screen_[bottom_] = blankLine();
    } else { for (int row = bottom_; row > top_; --row) screen_[row] = screen_[row - 1]; screen_[top_] = blankLine(); }
}
void TerminalScreen::newline() { pendingWrap_ = false; if (y_ == bottom_) scroll(1); else y_ = std::min(y_ + 1, rows_ - 1); }
void TerminalScreen::character(char32_t c) {
    if (parse_ == Parse::Charset) { parse_ = Parse::Text; return; }
    if (parse_ == Parse::Osc || parse_ == Parse::OscEscape) {
        if (c == 7 || (parse_ == Parse::OscEscape && c == '\\')) parse_ = Parse::Text;
        else parse_ = c == 27 ? Parse::OscEscape : Parse::Osc;
        return;
    }
    if (parse_ == Parse::Escape) {
        parse_ = Parse::Text;
        if (c == '[') { parse_ = Parse::Csi; sequence_.clear(); }
        else if (c == ']') parse_ = Parse::Osc;
        else if (c == '(' || c == ')') parse_ = Parse::Charset;
        else if (c == '7') { savedX_ = x_; savedY_ = y_; }
        else if (c == '8') { x_ = std::clamp(savedX_, 0, columns_ - 1); y_ = std::clamp(savedY_, 0, rows_ - 1); }
        else if (c == 'D') newline();
        else if (c == 'M') { if (y_ == top_) scroll(-1); else y_ = std::max(0, y_ - 1); }
        else if (c == 'c') { screen_.fill(blankLine(), rows_); x_ = y_ = 0; }
        return;
    }
    if (parse_ == Parse::Csi) {
        if (c >= 0x40 && c <= 0x7e) { csi(QChar(static_cast<ushort>(c))); parse_ = Parse::Text; }
        else if (sequence_.size() < 1024) sequence_ += QChar(static_cast<ushort>(c));
        else parse_ = Parse::Text;
        return;
    }
    if (c == 27) { parse_ = Parse::Escape; return; }
    if (c == '\r') { x_ = 0; pendingWrap_ = false; return; }
    if (c == '\n' || c == '\v' || c == '\f') { newline(); return; }
    if (c == '\b') { x_ = std::max(0, x_ - 1); pendingWrap_ = false; return; }
    if (c == '\t') { x_ = std::min(columns_ - 1, (x_ / 8 + 1) * 8); return; }
    if (c < 32 || c == 127) return;
    const auto width = widthOf(c); const auto text = QString::fromUcs4(&c, 1);
    if (!width) { if (x_ > 0) screen_[y_][x_ - 1].text += text; return; }
    if (pendingWrap_ && wrap_) { x_ = 0; newline(); }
    if (width == 2 && x_ == columns_ - 1) { x_ = 0; newline(); }
    auto cell = blank(); cell.text = text; screen_[y_][x_] = cell;
    if (width == 2 && x_ + 1 < columns_) { cell.text.clear(); cell.continuation = true; screen_[y_][x_ + 1] = cell; }
    x_ += width; if (x_ >= columns_) { x_ = columns_ - 1; pendingWrap_ = true; }
}
void TerminalScreen::csi(QChar final) {
    const bool privateMode = sequence_.startsWith('?');
    const auto params = (privateMode ? sequence_.mid(1) : sequence_).split(';');
    const auto n = [&](int i, int fallback = 1) { const int value = i < params.size() ? params[i].toInt() : 0; return value ? value : fallback; };
    const int count = std::min(n(0), 1000); const int mode = n(0, 0);
    pendingWrap_ = false;
    switch (final.unicode()) {
    case 'A': y_ = std::max(0, y_ - count); break;
    case 'B': y_ = std::min(rows_ - 1, y_ + count); break;
    case 'C': x_ = std::min(columns_ - 1, x_ + count); break;
    case 'D': x_ = std::max(0, x_ - count); break;
    case 'E': y_ = std::min(rows_ - 1, y_ + count); x_ = 0; break;
    case 'F': y_ = std::max(0, y_ - count); x_ = 0; break;
    case 'G': x_ = std::clamp(n(0) - 1, 0, columns_ - 1); break;
    case 'd': y_ = std::clamp(n(0) - 1, 0, rows_ - 1); break;
    case 'H': case 'f': y_ = std::clamp(n(0) - 1, 0, rows_ - 1); x_ = std::clamp(n(1) - 1, 0, columns_ - 1); break;
    case 'J':
        if (mode == 3) history_.clear();
        else for (int row = 0; row < rows_; ++row) for (int col = 0; col < columns_; ++col)
            if (mode == 2 || (mode == 0 && (row > y_ || (row == y_ && col >= x_))) || (mode == 1 && (row < y_ || (row == y_ && col <= x_)))) screen_[row][col] = blank();
        break;
    case 'K': for (int col = 0; col < columns_; ++col) if (mode == 2 || (mode == 0 && col >= x_) || (mode == 1 && col <= x_)) screen_[y_][col] = blank(); break;
    case 'X': for (int col = x_; col < std::min(columns_, x_ + count); ++col) screen_[y_][col] = blank(); break;
    case 'P': for (int col = x_; col < columns_; ++col) screen_[y_][col] = col + count < columns_ ? screen_[y_][col + count] : blank(); break;
    case '@': for (int col = columns_ - 1; col >= x_; --col) screen_[y_][col] = col - count >= x_ ? screen_[y_][col - count] : blank(); break;
    case 'L': case 'M': { const int oldTop = top_; top_ = y_; for (int i = 0; i < std::min(count, rows_); ++i) scroll(final == 'L' ? -1 : 1); top_ = oldTop; break; }
    case 'S': case 'T': for (int i = 0; i < std::min(count, rows_); ++i) scroll(final == 'S' ? 1 : -1); break;
    case 's': savedX_ = x_; savedY_ = y_; break;
    case 'u': x_ = std::clamp(savedX_, 0, columns_ - 1); y_ = std::clamp(savedY_, 0, rows_ - 1); break;
    case 'r': top_ = std::clamp(n(0) - 1, 0, rows_ - 2); bottom_ = std::clamp(n(1, rows_) - 1, top_ + 1, rows_ - 1); x_ = y_ = 0; break;
    case 'n': if (reply) { if (mode == 6) reply(QString("\x1b[%1;%2R").arg(y_ + 1).arg(x_ + 1).toUtf8()); else if (mode == 5) reply("\x1b[0n"); } break;
    case 'c': if (reply) reply("\x1b[?1;2c"); break;
    case 'h': case 'l':
        if (privateMode) for (const auto &p : params) {
            const bool enabled = final == 'h'; const int number = p.toInt();
            if (number == 25) cursorVisible_ = enabled;
            if (number == 7) wrap_ = enabled;
            if (number == 2004) bracketedPaste_ = enabled;
            if ((number == 1049 || number == 47 || number == 1047) && enabled != alternate_) {
                if (enabled) { primary_ = screen_; savedX_ = x_; savedY_ = y_; screen_.fill(blankLine(), rows_); x_ = y_ = 0; }
                else { screen_ = primary_; resize(columns_, rows_); x_ = std::clamp(savedX_, 0, columns_ - 1); y_ = std::clamp(savedY_, 0, rows_ - 1); }
                alternate_ = enabled;
            }
        }
        break;
    case 'm':
        for (int i = 0; i < params.size(); ++i) {
            const int code = params[i].toInt();
            if (code == 0) { fg_ = QColor("#E2E8F0"); bg_ = QColor("#0F172A"); bold_ = inverse_ = false; }
            else if (code == 1) bold_ = true;
            else if (code == 22) bold_ = false;
            else if (code == 7 || code == 27) inverse_ = code == 7;
            else if (code == 39) fg_ = QColor("#E2E8F0");
            else if (code == 49) bg_ = QColor("#0F172A");
            else if (code >= 30 && code <= 37) fg_ = indexed(code - 30 + (bold_ ? 8 : 0));
            else if (code >= 40 && code <= 47) bg_ = indexed(code - 40);
            else if (code >= 90 && code <= 97) fg_ = indexed(code - 90 + 8);
            else if (code >= 100 && code <= 107) bg_ = indexed(code - 100 + 8);
            else if ((code == 38 || code == 48) && i + 2 < params.size()) {
                QColor color;
                if (params[i + 1] == "5") { color = indexed(std::clamp(params[i + 2].toInt(), 0, 255)); i += 2; }
                else if (params[i + 1] == "2" && i + 4 < params.size()) { color = QColor(std::clamp(params[i + 2].toInt(), 0, 255), std::clamp(params[i + 3].toInt(), 0, 255), std::clamp(params[i + 4].toInt(), 0, 255)); i += 4; }
                if (color.isValid()) (code == 38 ? fg_ : bg_) = color;
            }
        }
        break;
    default: break;
    }
}
void TerminalScreen::feed(const QByteArray &bytes) { const QString text = decoder_(bytes); for (const auto c : text.toUcs4()) character(c); }
QString TerminalScreen::plainText() const {
    QStringList lines;
    for (int i = 0; i < historySize() + rows_; ++i) { QString text; for (const auto &cell : line(i)) text += cell.text; while (text.endsWith(' ')) text.chop(1); lines.append(text); }
    while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    return lines.join('\n');
}
TerminalView::TerminalView(QWidget *parent) : QAbstractScrollArea(parent) {
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); setFocusPolicy(Qt::StrongFocus); setAttribute(Qt::WA_InputMethodEnabled);
    setAccessibleName("任务交互终端"); screen_.reply = [this](QByteArray bytes) { if (inputEnabled_) emit input(bytes); };
    connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
}
void TerminalView::reset() { const auto columns = screen_.columns(), rows = screen_.rows(); screen_ = TerminalScreen(); screen_.resize(columns, rows); emit terminalResized(columns, rows); screen_.reply = [this](QByteArray bytes) { if (inputEnabled_) emit input(bytes); }; updateScroll(); viewport()->update(); }
void TerminalView::updateScroll() { const bool bottom = verticalScrollBar()->value() == verticalScrollBar()->maximum(); verticalScrollBar()->setRange(0, screen_.historySize()); if (bottom) verticalScrollBar()->setValue(verticalScrollBar()->maximum()); }
void TerminalView::feed(const QByteArray &bytes) { screen_.feed(bytes); updateScroll(); viewport()->update(); }
void TerminalView::paintEvent(QPaintEvent *) {
    QPainter painter(viewport()); painter.fillRect(viewport()->rect(), QColor("#0F172A")); painter.setFont(font());
    const int offset = verticalScrollBar()->value();
    for (int row = 0; row < screen_.rows(); ++row) {
        const auto &line = screen_.line(offset + row);
        for (int col = 0; col < std::min(screen_.columns(), int(line.size())); ++col) {
            const auto &cell = line[col]; QRect rect(col * cellWidth_, row * cellHeight_, cellWidth_, cellHeight_);
            painter.fillRect(rect, cell.background); if (cell.continuation) continue;
            if (col + 1 < line.size() && line[col + 1].continuation) rect.setWidth(cellWidth_ * 2);
            painter.setPen(cell.foreground); painter.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, cell.text);
        }
    }
    if (hasFocus() && screen_.cursorVisible() && offset == screen_.historySize()) {
        painter.setPen(QColor("#E2E8F0")); painter.drawRect(screen_.cursorColumn() * cellWidth_, screen_.cursorRow() * cellHeight_, cellWidth_ - 1, cellHeight_ - 1);
    }
}
void TerminalView::resizeEvent(QResizeEvent *event) {
    QAbstractScrollArea::resizeEvent(event); const QFontMetrics metrics(font()); cellWidth_ = std::max(1, metrics.horizontalAdvance('M')); cellHeight_ = std::max(1, metrics.height());
    screen_.resize(viewport()->width() / cellWidth_, viewport()->height() / cellHeight_); updateScroll(); emit terminalResized(screen_.columns(), screen_.rows());
}
void TerminalView::paste() {
    if (!inputEnabled_) return;
    auto bytes = QApplication::clipboard()->text().toUtf8();
    if (bytes.size() > 60000) bytes.truncate(60000);
    if (screen_.bracketedPaste()) bytes = "\x1b[200~" + bytes + "\x1b[201~";
    emit input(bytes);
}
void TerminalView::inputMethodEvent(QInputMethodEvent *event) { if (inputEnabled_ && !event->commitString().isEmpty()) emit input(event->commitString().toUtf8()); event->accept(); }
void TerminalView::keyPressEvent(QKeyEvent *event) {
    if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_C) { QApplication::clipboard()->setText(plainText()); return; }
    if (event->matches(QKeySequence::Paste)) { paste(); return; }
    if (!inputEnabled_) return;
    QByteArray bytes;
    if (event->modifiers().testFlag(Qt::ControlModifier) && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) bytes.append(char(event->key() - Qt::Key_A + 1));
    else switch (event->key()) {
    case Qt::Key_Return: case Qt::Key_Enter: bytes = "\r"; break;
    case Qt::Key_Backspace: bytes = "\x7f"; break;
    case Qt::Key_Tab: bytes = "\t"; break;
    case Qt::Key_Backtab: bytes = "\x1b[Z"; break;
    case Qt::Key_Escape: bytes = "\x1b"; break;
    case Qt::Key_Up: bytes = "\x1b[A"; break;
    case Qt::Key_Down: bytes = "\x1b[B"; break;
    case Qt::Key_Right: bytes = "\x1b[C"; break;
    case Qt::Key_Left: bytes = "\x1b[D"; break;
    case Qt::Key_Home: bytes = "\x1b[H"; break;
    case Qt::Key_End: bytes = "\x1b[F"; break;
    case Qt::Key_Delete: bytes = "\x1b[3~"; break;
    case Qt::Key_PageUp: verticalScrollBar()->setValue(verticalScrollBar()->value() - screen_.rows()); return;
    case Qt::Key_PageDown: verticalScrollBar()->setValue(verticalScrollBar()->value() + screen_.rows()); return;
    default: bytes = event->text().toUtf8(); break;
    }
    if (!bytes.isEmpty()) { verticalScrollBar()->setValue(verticalScrollBar()->maximum()); emit input(bytes); }
}
}
