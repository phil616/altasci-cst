#pragma once
#include <QObject>
#include <QQueue>
#include <functional>
#include <thread>

namespace cst {
// The QObject thread is the sole writer of application state. Each mutation has
// a synchronous begin event, a single background body, and a serialized finish event.
class OperationQueue final : public QObject {
    Q_OBJECT
public:
    explicit OperationQueue(QObject *parent = nullptr);
    ~OperationQueue() override;
    void enqueue(std::function<void()> begin, std::function<void()> work,
                 std::function<void(std::exception_ptr)> finish);
    void post(std::function<void()> event);
    bool busy() const;
    void join();
signals:
    void busyChanged(bool busy);
private:
    struct Operation {
        std::function<void()> begin;
        std::function<void()> work;
        std::function<void(std::exception_ptr)> finish;
    };
    void next();
    QQueue<Operation> pending_;
    std::thread worker_;
    bool active_ = false;
    bool closing_ = false;
};
}
