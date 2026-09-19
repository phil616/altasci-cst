#pragma once

#include "domain/Runtime.h"
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>

namespace cst {
struct Cancellation {
    std::atomic_bool requested{false};
    void check() const;
};
class Cancelled final : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("操作已取消") {}
};

struct ProcessSpec {
    QString program;
    QStringList arguments;
    QString workingDirectory;
    Environment environment;
    // shellCommandLine, when non-empty, is passed verbatim after the quoted cmd.exe path.
    QString shellCommandLine;
    QString projectId = {};
    QString taskId;
    QString operationId;
    QString attemptId = {};
    bool terminal = false;
    bool inputEnabled = false;
    QString encoding = "utf-8";
};
struct ProcessResult {
    qint64 exitCode = 0;
    bool crashed = false;
};
struct ProcessOutput {
    QString channel;
    QString text;
    bool decodeError = false;
    QDateTime timestamp;
    QString projectId = {};
    QString runId = {};
    QString attemptId = {};
    QByteArray bytes = {};
    quint64 sequence = 0;
};
class IManagedProcess {
public:
    virtual ~IManagedProcess() = default;
    virtual quint32 rootPid() const = 0;
    virtual bool rootRunning() const = 0;
    virtual bool empty() const = 0;
    virtual QList<quint32> processIds() const = 0;
    virtual std::optional<ProcessResult> result() const = 0;
    virtual bool treeEmpty() const { return processIds().isEmpty(); }
    virtual void writeInput(const QByteArray &) { throw std::runtime_error("此任务不支持输入"); }
    virtual void resizeTerminal(int, int) {}
    virtual void stop(int graceMs) = 0;
    virtual void forceStop() = 0;
};
class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;
    virtual std::shared_ptr<IManagedProcess> start(const ProcessSpec &spec,
                                                 std::function<void(ProcessOutput)> output) = 0;
    virtual QString resolveExecutable(const QString &program, const QStringList &toolDirectories) const = 0;
    virtual Environment inheritedEnvironment() const = 0;
    virtual QString resolveInEnvironment(const QString &program, const Environment &environment, const QString &) const {
        return resolveExecutable(program, environment.value("PATH").split(';', Qt::SkipEmptyParts));
    }
};
struct PortRequirement {
    QString protocol;
    QString address;
    quint16 port = 0;
    QString ownerTaskId;
};
struct PortOwner {
    QString protocol;
    QString address;
    quint16 port = 0;
    quint32 pid = 0;
    quint32 parentPid = 0;
    QString imagePath;
};
class IPortManager {
public:
    virtual ~IPortManager() = default;
    virtual QList<PortOwner> owners(const PortRequirement &port) const = 0;
    virtual void terminateTree(quint32 pid, const PortRequirement &port, const Cancellation &cancel) = 0;
    virtual bool stopService(quint32 pid, const Cancellation &cancel) = 0;
    virtual quint32 parentPid(quint32 pid) const = 0;
};
class IPrivilegeService {
public:
    virtual ~IPrivilegeService() = default;
    virtual void requireElevationAndDebugPrivilege() = 0;
};
struct Credential { QString username; QString password; };
class ICredentialStore {
public:
    virtual ~ICredentialStore() = default;
    virtual void write(const QString &target, const Credential &credential) = 0;
    virtual std::optional<Credential> read(const QString &target) const = 0;
    virtual void remove(const QString &target) = 0;
};
class IFileTransaction {
public:
    virtual ~IFileTransaction() = default;
    virtual void renameDirectory(const QString &from, const QString &to) = 0;
    virtual bool removeDirectory(const QString &path) = 0;
};
class IUrlLauncher {
public:
    virtual ~IUrlLauncher() = default;
    virtual void open(const QString &url) = 0;
};
class IClock {
public:
    virtual ~IClock() = default;
    virtual qint64 monotonicMs() const = 0;
    virtual void sleep(int milliseconds, const Cancellation &cancel) const = 0;
};
}
