#include "PortReclaimService.h"
#include <QJsonArray>
#include <QSet>

namespace cst {
PortReclaimService::PortReclaimService(IPortManager &ports, IClock &clock) : ports_(ports), clock_(clock) {}
QList<PortRequirement> PortReclaimService::requirements(const QJsonArray &array) {
    QList<PortRequirement> result;
    for (const auto &value : array) {
        const auto p = value.toObject();
        result.append({p.value("protocol").toString(), p.value("address").toString(), quint16(p.value("port").toInt()), p.value("ownerTaskId").toString()});
    }
    return result;
}
void PortReclaimService::reclaim(const QList<PortRequirement> &required, int timeoutMs, int maxAncestorEscalation,
                               const QList<std::shared_ptr<IManagedProcess>> &oldJobs, const Cancellation &cancel) {
    const auto deadline = clock_.monotonicMs() + timeoutMs;
    std::optional<qint64> freeSince;
    QMap<quint32, QSet<quint32>> childrenSeen;
    QMap<quint32, int> escalationDepth;
    while (clock_.monotonicMs() <= deadline) {
        cancel.check();
        bool free = true;
        for (const auto &port : required) {
            const auto owners = ports_.owners(port);
            if (!owners.isEmpty()) free = false;
            for (const auto &owner : owners) {
                cancel.check();
                bool managed = false;
                for (const auto &job : oldJobs) if (job->processIds().contains(owner.pid)) { job->forceStop(); managed = true; break; }
                if (managed) continue;
                quint32 target = owner.pid;
                quint32 parent = owner.parentPid;
                childrenSeen[parent].insert(owner.pid);
                if (parent != 0 && childrenSeen[parent].size() >= 3 && escalationDepth.value(parent) < maxAncestorEscalation) {
                    target = parent;
                    const auto grandparent = ports_.parentPid(parent);
                    escalationDepth[grandparent] = escalationDepth.value(parent) + 1;
                    childrenSeen[grandparent].insert(parent);
                }
                const bool serviceStopped = ports_.stopService(target, cancel);
                if (serviceStopped) clock_.sleep(2000, cancel);
                bool stillOwned = false;
                for (const auto &remaining : ports_.owners(port)) if (remaining.pid == owner.pid) stillOwned = true;
                if (stillOwned) ports_.terminateTree(target, port, cancel);
                if (clock_.monotonicMs() > deadline) break;
            }
        }
        if (free) {
            if (!freeSince) freeSince = clock_.monotonicMs();
            if (clock_.monotonicMs() - *freeSince >= 500) return;
        } else freeSince.reset();
        clock_.sleep(250, cancel);
    }
    throw std::runtime_error("端口释放超时；请检查端口占用诊断");
}
bool PortReclaimService::ownedBy(const QList<PortRequirement> &required, const QString &taskId, const IManagedProcess &job) const {
    const auto ids = job.processIds();
    for (const auto &port : required) {
        if (port.ownerTaskId != taskId) continue;
        const auto owners = ports_.owners(port);
        if (owners.isEmpty()) return false;
        for (const auto &owner : owners) if (!ids.contains(owner.pid)) return false;
    }
    return true;
}
}
