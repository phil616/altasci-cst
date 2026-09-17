#include "SystemClock.h"
#include <QThread>
#include <algorithm>

namespace cst {
void Cancellation::check() const { if (requested.load()) throw Cancelled(); }
SystemClock::SystemClock() { timer_.start(); }
qint64 SystemClock::monotonicMs() const { return timer_.elapsed(); }
void SystemClock::sleep(int milliseconds, const Cancellation &cancel) const {
    const auto end = monotonicMs() + milliseconds;
    while (monotonicMs() < end) {
        cancel.check();
        QThread::msleep(static_cast<unsigned long>(std::min<qint64>(25, end - monotonicMs())));
    }
    cancel.check();
}
}
