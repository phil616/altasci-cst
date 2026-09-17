#pragma once
#include "Platform.h"

namespace cst {
class ReadinessService {
public:
    explicit ReadinessService(IClock &clock);
    void wait(const QJsonObject &readiness, IManagedProcess &process, const Cancellation &cancel,
              const std::function<void()> &heartbeat = {});
private:
    bool probe(const QJsonObject &probe, const Cancellation &cancel, qint64 deadline);
    IClock &clock_;
};
}
