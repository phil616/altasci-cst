#pragma once
#include "domain/Configuration.h"
#include <QAbstractNativeEventFilter>
#include <QObject>
#include <memory>

namespace cst {
class WindowsSession final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit WindowsSession(QObject *parent = nullptr);
    ~WindowsSession() override;
    bool primary() const;
    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *result) override;
    static ProjectPaths paths();
    static void requireSupportedWindows();
signals:
    void activateRequested();
    void endSessionRequested();
private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};
}
