#pragma once
#include "pages/admin/AdminPage.h"
#include "pages/user/UserPage.h"
#include <QLabel>
#include <QMainWindow>
#include <QDialog>
#include "widgets/TerminalView.h"

namespace cst {
class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(ProjectRuntimeService &runtime, UserPage *user, AdminPage *admin, LogService &logs, QWidget *parent = nullptr);
    void activate();
protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void positionClosingOverlay();
    ProjectRuntimeService &runtime_;
    UserPage *user_;
    AdminPage *admin_;
    QStackedWidget *pages_;
    QLabel *projectName_;
    QLabel *state_;
    QLabel *head_;
    QLabel *operation_;
    QWidget *central_;
    QLabel *closingOverlay_;
    bool mayClose_ = false;
    struct Console { QDialog *window; TerminalView *view; QLabel *status; QString attempt; bool terminal = false; };
    QMap<QString, Console> consoles_;
    Console &console(const QString &task);
    void openConsole(const QString &task);
};
}
