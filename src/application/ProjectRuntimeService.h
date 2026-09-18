#pragma once
#include "OperationQueue.h"
#include "PortReclaimService.h"
#include "ProjectConfigService.h"
#include "SourceSyncService.h"
#include "TaskSupervisor.h"
#include <QTimer>
#include <QLockFile>

namespace cst {
class ProjectRuntimeService final : public QObject {
    Q_OBJECT
public:
    ProjectRuntimeService(ProjectConfigService &configuration, ProjectCatalogService &catalog,
                          TaskSupervisor &supervisor, PortReclaimService &ports, SourceSyncService &sync,
                          QObject *parent = nullptr);
    ~ProjectRuntimeService() override;
    ProjectState state() const;
    QJsonObject currentProject() const;
    bool editable() const;
    bool editing() const;
    bool busy() const;
    QString operationId() const;
    QJsonArray taskSnapshot() const;
    void initialize(const QString &storageDirectory);
    void setEditing(bool editing);
    void start();
    void stop();
    void synchronize();
    void close();
    void switchProject(const QString &id);
    void saveConfig(const QJsonObject &document);
    void importProject(const QJsonObject &document);
    void removeProject(const QString &id);
    void setDefault(const QString &id);
    void exportProject(const QString &destination);
    void maintenance(std::function<void(const Cancellation &)> work);
signals:
    void availabilityChanged();
    void stateChanged(cst::ProjectState state);
    void projectChanged(QJsonObject document);
    void operationFinished(QString result, bool success);
    void taskChanged(cst::TaskStatus task);
    void processOutput(QString taskId, cst::ProcessOutput output);
    void closeReady();
    void headChanged(QString head);
private:
    void event(RuntimeEvent event, bool failure = false);
    void edit(std::function<void()> work, bool reload);
    void finish(std::exception_ptr error, const QString &success);
    void cleanupAfterFailure(std::exception_ptr error);
    SyncRequest syncRequest() const;
    QString newOperation();
    ProjectConfigService &configuration_;
    ProjectCatalogService &catalog_;
    TaskSupervisor &supervisor_;
    PortReclaimService &ports_;
    SourceSyncService &sync_;
    OperationQueue queue_;
    QTimer monitor_;
    QJsonObject current_;
    ProjectState state_ = ProjectState::Stopped;
    std::shared_ptr<Cancellation> cancel_;
    QString operationId_;
    bool editing_ = false;
    bool closing_ = false;
    bool jobsEmpty_ = true;
    bool stopQueued_ = false;
    QMap<QString, TaskStatus> taskStatuses_;
    QString storageDirectory_;
    std::unique_ptr<QLockFile> projectLock_;
};
}
Q_DECLARE_METATYPE(cst::ProjectState)
Q_DECLARE_METATYPE(cst::TaskStatus)
Q_DECLARE_METATYPE(cst::ProcessOutput)
