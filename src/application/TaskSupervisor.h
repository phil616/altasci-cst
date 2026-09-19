#pragma once
#include "Platform.h"
#include "domain/Configuration.h"
#include <vector>
#include <mutex>

namespace cst {
struct TaskStatus {
    QString id;
    QString name;
    QString state;
    qsizetype restartCount = 0;
};
class ITaskSupervisor {
public:
    virtual ~ITaskSupervisor() = default;
    virtual bool ownsProjectLock() const { return false; }
    virtual void preflight(const QJsonObject &, const Cancellation &) = 0;
    virtual void start(const QJsonObject &, const QString &, const Cancellation &) = 0;
    virtual void tick(const Cancellation &) = 0;
    virtual void stop() = 0;
    virtual bool empty() const = 0;
    virtual QList<TaskStatus> statuses() const = 0;
    virtual void writeInput(const QString &, const QByteArray &) = 0;
    virtual void resizeTerminal(const QString &, int, int) = 0;
    std::function<void(const TaskStatus &)> taskChanged;
    std::function<void(const QString &, ProcessOutput)> output;
};
class TaskSupervisor final : public ITaskSupervisor {
public:
    TaskSupervisor(IProcessRunner &runner, IClock &clock, ProjectPaths paths);
    void preflight(const QJsonObject &, const Cancellation &) override;
    void start(const QJsonObject &, const QString &, const Cancellation &) override;
    void tick(const Cancellation &) override;
    void stop() override;
    bool empty() const override;
    QList<TaskStatus> statuses() const override;
    void writeInput(const QString &, const QByteArray &) override;
    void resizeTerminal(const QString &, int, int) override;
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
    mutable std::mutex sessionsMutex_;
    QMap<QString, std::shared_ptr<IManagedProcess>> sessions_;
    std::function<void(ProcessOutput)> consumer(const ProcessSpec &);
    void waitReady(Task &, const Cancellation &);
    void registerSession(const QString &, const std::shared_ptr<IManagedProcess> &);
};
}
