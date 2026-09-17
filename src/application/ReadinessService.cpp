#include "ReadinessService.h"
#include <QEventLoop>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpSocket>
#include <QTimer>
#include <algorithm>

namespace cst {
ReadinessService::ReadinessService(IClock &clock) : clock_(clock) {}
bool ReadinessService::probe(const QJsonObject &spec, const Cancellation &cancel, qint64 deadline) {
    cancel.check();
    const auto budget = std::max<qint64>(0, deadline - clock_.monotonicMs());
    if (budget == 0) return false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QTimer cancellation;
    cancellation.setInterval(20);
    QObject::connect(&cancellation, &QTimer::timeout, &loop, [&] { if (cancel.requested.load()) loop.quit(); });
    cancellation.start();
    if (spec.value("type") == "tcp") {
        QTcpSocket socket;
        QObject::connect(&socket, &QTcpSocket::connected, &loop, &QEventLoop::quit);
        QObject::connect(&socket, &QTcpSocket::errorOccurred, &loop, &QEventLoop::quit);
        timeout.start(int(std::min<qint64>(budget, spec.value("connectTimeoutMs").toInt())));
        socket.connectToHost(spec.value("address").toString(), quint16(spec.value("port").toInt()));
        loop.exec(); cancel.check();
        return socket.state() == QAbstractSocket::ConnectedState;
    }
    QNetworkAccessManager network;
    QNetworkRequest request(QUrl(spec.value("url").toString()));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto *reply = network.get(request);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(int(std::min<qint64>(budget, spec.value("requestTimeoutMs").toInt())));
    loop.exec();
    const bool finished = reply->isFinished();
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!finished) reply->abort();
    cancel.check();
    return finished && status != 0 && spec.value("expectedStatusCodes").toArray().contains(status);
}
void ReadinessService::wait(const QJsonObject &readiness, IManagedProcess &process, const Cancellation &cancel,
                            const std::function<void()> &heartbeat) {
    const auto deadline = clock_.monotonicMs() + readiness.value("timeoutMs").toInt();
    const auto probes = readiness.value("probes").toArray();
    QList<int> consecutive(probes.size(), 0);
    while (clock_.monotonicMs() < deadline) {
        cancel.check();
        if (heartbeat) heartbeat();
        if (!process.rootRunning() || process.empty()) throw std::runtime_error("服务在就绪前退出");
        bool allReady = true;
        for (qsizetype i = 0; i < probes.size(); ++i) {
            if (probe(probes[i].toObject(), cancel, deadline)) ++consecutive[i]; else consecutive[i] = 0;
            allReady = allReady && consecutive[i] >= readiness.value("successThreshold").toInt();
            if (!process.rootRunning() || process.empty()) throw std::runtime_error("服务在就绪前退出");
        }
        if (allReady && clock_.monotonicMs() <= deadline) return;
        clock_.sleep(int(std::max<qint64>(0, std::min<qint64>(readiness.value("pollIntervalMs").toInt(), deadline - clock_.monotonicMs()))), cancel);
    }
    throw std::runtime_error("服务就绪探针超时");
}
}
