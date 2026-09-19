#pragma once
#include "application/ProjectRuntimeService.h"
#include <QWidget>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QToolButton>

namespace cst {
class UserPage final : public QWidget {
    Q_OBJECT
public:
    explicit UserPage(IUrlLauncher &urls, QWidget *parent = nullptr);
    void setProject(const QJsonObject &document);
    void setState(ProjectState state);
    void setActive(bool active);
    void setConfigurationDirty(bool dirty);
    void setTask(const TaskStatus &task);
    void appendLog(const QString &line);
signals:
    void mainAction();
    void consoleRequested(QString task);
    void configureRequested();
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
    QPushButton *configure_;
    QWidget *entries_;
    QWidget *details_;
    QToolButton *detailsToggle_;
    QGridLayout *actions_;
    QList<QPushButton *> buttons_;
    QTableWidget *tasks_;
    QPlainTextEdit *logs_;
    ProjectState state_ = ProjectState::Stopped;
    bool hasProject_ = false;
    bool configurationDirty_ = false;
};
}
