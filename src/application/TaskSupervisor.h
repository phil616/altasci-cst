#pragma once
#include "Platform.h"
#include "domain/Configuration.h"
#include <vector>

namespace cst {
struct TaskStatus {
    QString id;
    QString name;
    QString state;
    qsizetype restartCount = 0;
};
class TaskSupervisor {
public:
    TaskSupervisor(IProcessRunner &runner, IClock &clock, ProjectPaths paths);
    void preflight(const QJsonObject &project, const Cancellation &cancel);
    void start(const QJsonObject &project, const QString &operationId, const Cancellation &cancel);
    void tick(const Cancellation &cancel);
    void stop();
    bool empty() const;
    QList<TaskStatus> statuses() const;
    std::function<void(const TaskStatus &)> taskChanged;
    std::function<void(const QString &, ProcessOutput)> output;
private:
    struct Task {
        QJsonObject configuration;
        std::shared_ptr<IManagedProcess> process;
        RestartBudget budget;
        QString state = "Stopped";
        std::optional<qint64> startedAt;
        std::optional<qint64> restartAt;
        bool restarting = false;
    };
    ProcessSpec command(const QJsonObject &task, const QJsonObject &command) const;
    void prepare(Task &task, const QJsonObject &command, const Cancellation &cancel);
    void launch(Task &task, const Cancellation &cancel);
    void notify(Task &task, const QString &state);
    void log(const QString &taskId, const QString &message);
    QJsonObject expand(const QJsonObject &object) const;
    IProcessRunner &runner_;
    IClock &clock_;
    ProjectPaths paths_;
    QJsonObject project_;
    QString operationId_;
    std::vector<Task> tasks_;
    std::shared_ptr<IManagedProcess> preparing_;
    bool stopping_ = false;
};
}
