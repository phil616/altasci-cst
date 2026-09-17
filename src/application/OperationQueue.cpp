#include "OperationQueue.h"
#include <QThread>

namespace cst {
OperationQueue::OperationQueue(QObject *parent) : QObject(parent) {}
OperationQueue::~OperationQueue() { join(); }
void OperationQueue::join() { closing_ = true; pending_.clear(); if (worker_.joinable()) worker_.join(); }
bool OperationQueue::busy() const { return active_ || !pending_.isEmpty(); }
void OperationQueue::enqueue(std::function<void()> begin, std::function<void()> work,
                              std::function<void(std::exception_ptr)> finish) {
    Q_ASSERT(thread() == QThread::currentThread());
    if (closing_) throw std::runtime_error("操作队列正在关闭");
    pending_.enqueue({std::move(begin), std::move(work), std::move(finish)});
    next();
}
void OperationQueue::post(std::function<void()> event) {
    QMetaObject::invokeMethod(this, [this, event = std::move(event)] { if (!closing_) event(); }, Qt::QueuedConnection);
}
void OperationQueue::next() {
    if (active_ || pending_.isEmpty() || closing_) return;
    auto operation = pending_.dequeue(); active_ = true; emit busyChanged(true);
    try { if (operation.begin) operation.begin(); }
    catch (...) {
        operation.finish(std::current_exception()); active_ = false; emit busyChanged(false);
        QMetaObject::invokeMethod(this, [this] { next(); }, Qt::QueuedConnection); return;
    }
    worker_ = std::thread([this, operation = std::move(operation)] {
        std::exception_ptr error;
        try { operation.work(); } catch (...) { error = std::current_exception(); }
        post([this, finish = operation.finish, error] {
            if (worker_.joinable()) worker_.join();
            finish(error); active_ = false; emit busyChanged(false); next();
        });
    });
}
}
