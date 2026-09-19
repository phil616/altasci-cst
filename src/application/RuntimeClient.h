#pragma once
#include "TaskSupervisor.h"
#include <QProcess>
#include <QJsonArray>

namespace cst {
class RuntimeClient final : public ITaskSupervisor {
public:
    explicit RuntimeClient(const QString &executable);
    ~RuntimeClient() override;
    void preflight(const QJsonObject &, const Cancellation &) override;
    void start(const QJsonObject &, const QString &, const Cancellation &) override;
    void tick(const Cancellation &) override;
    void stop() override;
    bool empty() const override;
    QList<TaskStatus> statuses() const override;
    void writeInput(const QString &, const QByteArray &) override;
    void resizeTerminal(const QString &, int, int) override;
    bool ownsProjectLock() const override { return true; }
private:
    QJsonObject request(QJsonObject) const;
    void operation(QJsonObject, const Cancellation *);
    void receive(const QJsonObject &);
    QString server_, token_;
    QProcess host_;
    mutable std::mutex snapshotMutex_;
    QList<TaskStatus> statuses_;
    bool empty_ = true;
    quint64 afterSequence_ = 0;
};
// Host event loop, shared by the Windows executable and portable IPC tests.
int serveRuntime(ITaskSupervisor &, const ProjectPaths &, const QString &server, const QString &token,
                 std::function<bool()> ownerAlive);
}
