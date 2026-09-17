#pragma once
#include "Platform.h"

namespace cst {
class PortReclaimService {
public:
    PortReclaimService(IPortManager &ports, IClock &clock);
    void reclaim(const QList<PortRequirement> &required, int timeoutMs, int maxAncestorEscalation,
                 const QList<std::shared_ptr<IManagedProcess>> &oldJobs, const Cancellation &cancel);
    bool ownedBy(const QList<PortRequirement> &required, const QString &taskId, const IManagedProcess &job) const;
    static QList<PortRequirement> requirements(const QJsonArray &array);
private:
    IPortManager &ports_;
    IClock &clock_;
};
}
