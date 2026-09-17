#pragma once
#include "application/ProjectRuntimeService.h"
#include <QWidget>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QTableWidget>

namespace cst {
class UserPage final : public QWidget {
    Q_OBJECT
public:
    explicit UserPage(IUrlLauncher &urls, QWidget *parent = nullptr);
    void setProject(const QJsonObject &document);
    void setState(ProjectState state);
    void setActive(bool active);
    void setTask(const TaskStatus &task);
    void appendLog(const QString &line);
signals:
    void mainAction();
    void error(QString message);
protected:
    void resizeEvent(QResizeEvent *event) override;
private:
    void arrangeActions();
    IUrlLauncher &urls_;
    QLabel *title_;
    QLabel *description_;
    QLabel *statusHelp_;
    QLabel *actionsHelp_;
    QPushButton *main_;
    QGridLayout *actions_;
    QList<QPushButton *> buttons_;
    QTableWidget *tasks_;
    QPlainTextEdit *logs_;
    ProjectState state_ = ProjectState::Stopped;
    bool hasProject_ = false;
};
}
