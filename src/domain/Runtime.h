#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <optional>

namespace cst {
enum class ProjectState { Stopped, Preflight, ReclaimingPorts, Starting, Running, Stopping, Failed, Syncing };
enum class RuntimeEvent { Start, ChecksPassed, CheckFailed, PortsFree, ReclaimFailed, TasksReady, TaskFailed,
                          Stop, Close, UnrecoverableCrash, JobsEmpty, Sync, SyncSucceeded, SyncFailed };
QString stateName(ProjectState state);
std::optional<ProjectState> transition(ProjectState state, RuntimeEvent event, bool preserveFailure = false);

struct RestartPolicy {
    int maxRestarts = 5;
    qint64 windowMs = 600000;
    int backoffSeconds = 1;
    int maxBackoffSeconds = 30;
};
class RestartBudget {
public:
    explicit RestartBudget(RestartPolicy policy = {});
    std::optional<qint64> schedule(qint64 monotonicMs);
    void reset();
    qsizetype attempts() const;
private:
    RestartPolicy policy_;
    QList<qint64> restarts_;
    qsizetype consecutive_ = 0;
};

using Environment = QMap<QString, QString>;
Environment parseEnv(const QByteArray &bytes);
Environment mergeEnvironment(bool inheritSystem, const Environment &system,
                             const QList<Environment> &files, const Environment &variables,
                             const Environment &injected);
QString environmentBlock(const Environment &environment);
QString redactSecrets(QString text, const QStringList &knownSecrets = {});
}
