#include "ui/widgets/TerminalView.h"
#include <QTest>
#include <QSignalSpy>
#include <QClipboard>
#include <QPushButton>
#include <QVBoxLayout>
using namespace cst;
class TerminalTests final : public QObject {
    Q_OBJECT
private slots:
    void styleAndQueryPreserveWrap() {
        TerminalScreen screen; screen.resize(4, 3);
        screen.feed("abcd\x1b[31m\x1b[6nE");
        QCOMPARE(screen.plainText(), "abcd\nE");
    }
    void insertOutsideScrollRegion() {
        TerminalScreen screen; screen.resize(10, 4);
        screen.feed("one\r\ntwo\r\nthree\r\nfour\x1b[1;2r\x1b[4;1H\x1b[L\x1b[M");
        QCOMPARE(screen.plainText(), "one\ntwo\nthree\nfour");
    }
    void tabReachesTerminal() {
        QWidget window; QVBoxLayout layout(&window);
        TerminalView view; QPushButton button("Other action");
        layout.addWidget(&view); layout.addWidget(&button);
        window.show(); view.setFocus(); QTest::qWait(20);
        QSignalSpy input(&view, &TerminalView::input);
        QTest::keyClick(&view, Qt::Key_Tab);
        QCOMPARE(input.size(), 1); QCOMPARE(input.last()[0].toByteArray(), QByteArray("\t"));
        QTest::keyClick(&view, Qt::Key_Backtab);
        QCOMPARE(input.size(), 2); QCOMPARE(input.last()[0].toByteArray(), QByteArray("\x1b[Z"));
    }
    void pasteLineEndingsAndUtf8Boundary() {
        TerminalView view; QSignalSpy input(&view, &TerminalView::input);
        QApplication::clipboard()->setText("first\r\nsecond\nthird\rfourth"); view.paste();
        QCOMPARE(input.last()[0].toByteArray(), QByteArray("first\rsecond\rthird\rfourth"));
        QApplication::clipboard()->setText(QString(59999, 'a') + QString::fromUtf8("你好"));
        view.feed("\x1b[?2004h"); view.paste();
        QCOMPARE(input.last()[0].toByteArray(), QByteArray("\x1b[200~") + QByteArray(59999, 'a') + "\x1b[201~");
    }
    void progressAndErase() {
        TerminalScreen screen; screen.resize(20, 3); screen.feed("progress 99%\r\x1b[2Kdone\r\nnext");
        QCOMPARE(screen.plainText(), "done\nnext");
    }
    void incrementalUtf8AndWideCharacters() {
        TerminalScreen screen; screen.resize(20, 3); const auto bytes = QString("你好").toUtf8();
        for (const auto b : bytes) screen.feed(QByteArray(1, b));
        QCOMPARE(screen.plainText(), QString("你好")); QCOMPARE(screen.cursorColumn(), 4);
    }
    void colorsCursorAndAlternateScreen() {
        TerminalScreen screen; screen.resize(20, 3); screen.feed("\x1b[31mred\x1b[0m\x1b[2;1Hrow");
        QCOMPARE(screen.line(0)[0].foreground, QColor("#EF4444"));
        screen.feed("\x1b[?1049hother"); QVERIFY(screen.plainText().contains("other"));
        screen.feed("\x1b[?1049l"); QVERIFY(screen.plainText().contains("red")); QVERIFY(!screen.plainText().contains("other"));
    }
    void boundedHistoryAndControlResponses() {
        TerminalScreen screen; screen.resize(10, 2); QByteArray response; screen.reply = [&](const QByteArray &bytes) { response += bytes; };
        for (int i = 0; i < 6000; ++i) screen.feed("line\r\n");
        QCOMPARE(screen.historySize(), 5000); screen.feed("\x1b[6n"); QCOMPARE(response, QByteArray("\x1b[2;1R"));
        screen.feed("\x1b]0;ignored title\x1b\\hello"); QVERIFY(screen.plainText().endsWith("hello"));
    }
    void inputAndResize() {
        TerminalView view; QSignalSpy input(&view, &TerminalView::input), resized(&view, &TerminalView::terminalResized);
        view.resize(640, 480); view.show(); QTest::qWait(20);
        QVERIFY(!resized.isEmpty()); QTest::keyClick(&view, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(input.last()[0].toByteArray(), QByteArray(1, '\x03'));
        view.setInputEnabled(false); const auto before = input.size(); QTest::keyClick(&view, Qt::Key_A); QCOMPARE(input.size(), before);
    }
};
QTEST_MAIN(TerminalTests)
#include "TerminalTests.moc"
