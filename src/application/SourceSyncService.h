#pragma once
#include "Platform.h"
#include <QMutex>

namespace cst {
struct SyncRequest {
    QString projectId;
    QString repositoryUrl;
    QString branch;
    QString target;
    QString gitExecutable;
    QString credentialTarget;
    QString operationId;
};
class SourceSyncService {
public:
    SourceSyncService(IProcessRunner &runner, IFileTransaction &files, IClock &clock,
                      QString storageDirectory, QString askpassPath, QMutex &operationMutex);
    QString synchronize(const SyncRequest &request, const Cancellation &cancel,
                        const std::function<bool()> &canSync);
    void testAuthentication(const SyncRequest &request, const Cancellation &cancel);
    void recover(const SyncRequest &request, const Cancellation &cancel);
    void retryCleanup();
private:
    QString git(const SyncRequest &request, const QStringList &arguments, const QString &directory,
                const Cancellation &cancel);
    QString validateCheckout(const SyncRequest &request, const QString &directory, const Cancellation &cancel);
    void queueCleanup(const QString &path);
    void cleanup(const QString &path);
    void recoverLocked(const SyncRequest &request, const Cancellation &cancel);
    QString journalPath(const QString &projectId) const;
    IProcessRunner &runner_;
    IFileTransaction &files_;
    IClock &clock_;
    QString storage_;
    QString askpass_;
    QMutex &operationMutex_;
};
}
