#pragma once
#include "application/Platform.h"
#include <QElapsedTimer>

namespace cst {
class SystemClock final : public IClock {
public:
    SystemClock();
    qint64 monotonicMs() const override;
    void sleep(int milliseconds, const Cancellation &cancel) const override;
private:
    QElapsedTimer timer_;
};
}
