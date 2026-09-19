#pragma once
#include "application/Platform.h"

namespace cst {
class WindowsPrivilegeService final : public IPrivilegeService {
public:
    void requireElevationAndDebugPrivilege() override;
};
class WindowsCredentialStore final : public ICredentialStore {
public:
    void write(const QString &target, const Credential &credential) override;
    std::optional<Credential> read(const QString &target) const override;
    void remove(const QString &target) override;
};
class WindowsFileTransaction final : public IFileTransaction {
public:
    void renameDirectory(const QString &from, const QString &to) override;
    bool removeDirectory(const QString &path) override;
};
class WindowsProcessRunner final : public IProcessRunner {
public:
    explicit WindowsProcessRunner(QString signalHelper);
    std::shared_ptr<IManagedProcess> start(const ProcessSpec &spec, std::function<void(ProcessOutput)> output) override;
    QString resolveExecutable(const QString &program, const QStringList &toolDirectories) const override;
    Environment inheritedEnvironment() const override;
    QString resolveInEnvironment(const QString &, const Environment &, const QString &) const override;
private:
    QString signalHelper_;
    Environment inherited_;
};
class WindowsPortManager final : public IPortManager {
public:
    QList<PortOwner> owners(const PortRequirement &port) const override;
    void terminateTree(quint32 pid, const PortRequirement &port, const Cancellation &cancel) override;
    bool stopService(quint32 pid, const Cancellation &cancel) override;
    quint32 parentPid(quint32 pid) const override;
};
QString currentUserSid();
}
