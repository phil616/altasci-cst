#include "RuntimeClient.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcessEnvironment>
#include <QThread>
#include <QUuid>

namespace cst {
RuntimeClient::RuntimeClient(const QString &executable) {
    server_ = "cst-runtime-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    token_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto environment = QProcessEnvironment::systemEnvironment(); environment.insert("CST_HOST_TOKEN", token_);
    host_.setProcessEnvironment(environment); host_.setProgram(executable);
    host_.setArguments({server_, QString::number(QCoreApplication::applicationPid())});
    host_.setStandardOutputFile(QProcess::nullDevice()); host_.setStandardErrorFile(QProcess::nullDevice());
    host_.start();
    if (!host_.waitForStarted(5000)) throw std::runtime_error("无法启动 cst-runtime.exe");
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 5000) {
        try { request({{"command", "snapshot"}}); return; }
        catch (...) { if (host_.waitForFinished(25)) break; }
    }
    host_.kill(); host_.waitForFinished(5000);
    throw std::runtime_error("运行宿主握手失败");
}
RuntimeClient::~RuntimeClient() {
    try { stop(); request({{"command", "shutdown"}}); } catch (...) {}
    if (!host_.waitForFinished(8000)) { host_.kill(); host_.waitForFinished(5000); }
}
QJsonObject RuntimeClient::request(QJsonObject message) const {
    message["version"] = 1; message["token"] = token_;
    QLocalSocket socket; socket.connectToServer(server_);
    if (!socket.waitForConnected(1000)) throw std::runtime_error("运行宿主连接已断开");
    const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    socket.write(bytes);
    QElapsedTimer timer; timer.start();
    while (socket.bytesToWrite() && timer.elapsed() < 3000) socket.waitForBytesWritten(100);
    QByteArray response;
    while (timer.elapsed() < 5000) {
        response += socket.readAll();
        if (response.size() > 32 * 1024 * 1024) throw std::runtime_error("宿主响应超过大小限制");
        if (response.endsWith('\n')) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(response, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) throw std::runtime_error("宿主响应无效");
            const auto result = document.object();
            if (!result.value("error").toString().isEmpty()) throw std::runtime_error(result.value("error").toString().toUtf8().constData());
            return result;
        }
        if (socket.state() == QLocalSocket::UnconnectedState) break;
        socket.waitForReadyRead(100);
    }
    throw std::runtime_error("运行宿主响应超时");
}
void RuntimeClient::receive(const QJsonObject &message) {
    QList<TaskStatus> statuses;
    for (const auto &v : message.value("tasks").toArray()) {
        const auto task = v.toObject(); statuses.append({task.value("id").toString(), task.value("name").toString(), task.value("state").toString(), qsizetype(task.value("restartCount").toInteger())});
    }
    QList<TaskStatus> previous;
    { std::lock_guard lock(snapshotMutex_); previous = statuses_; statuses_ = statuses; empty_ = message.value("empty").toBool(); }
    if (taskChanged) for (const auto &task : statuses) {
        bool changed = true;
        for (const auto &old : previous) if (old.id == task.id && old.state == task.state && old.restartCount == task.restartCount) changed = false;
        if (changed) taskChanged(task);
    }
    for (const auto &value : message.value("output").toArray()) {
        const auto item = value.toObject();
        ProcessOutput out; out.channel = item.value("channel").toString(); out.text = item.value("text").toString();
        out.bytes = QByteArray::fromBase64(item.value("bytes").toString().toLatin1());
        out.timestamp = QDateTime::fromString(item.value("ts").toString(), Qt::ISODateWithMs);
        out.projectId = item.value("projectId").toString(); out.runId = item.value("runId").toString(); out.attemptId = item.value("attemptId").toString();
        out.sequence = quint64(item.value("seq").toInteger());
        if (out.sequence && out.sequence <= afterSequence_) continue;
        afterSequence_ = std::max(afterSequence_, out.sequence); out.decodeError = item.value("decodeError").toBool();
        if (output) output(item.value("task").toString(), std::move(out));
    }
}
void RuntimeClient::operation(QJsonObject message, const Cancellation *cancel) {
    message["requestId"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto accepted = request(message); const auto id = accepted.value("operation").toString();
    bool sentCancel = false;
    for (;;) {
        if (cancel && cancel->requested.load() && !sentCancel) { request({{"command", "cancel"}}); sentCancel = true; }
        const auto state = request({{"command", "snapshot"}, {"afterSeq", qint64(afterSequence_)}}); receive(state);
        if (state.value("finished").toString() == id) {
            if (sentCancel) throw Cancelled();
            if (!state.value("failure").toString().isEmpty()) throw std::runtime_error(state.value("failure").toString().toUtf8().constData());
            return;
        }
        QThread::msleep(25);
    }
}
void RuntimeClient::preflight(const QJsonObject &project, const Cancellation &cancel) { operation({{"command", "preflight"}, {"project", project}}, &cancel); }
void RuntimeClient::start(const QJsonObject &project, const QString &id, const Cancellation &cancel) { operation({{"command", "start"}, {"project", project}, {"runId", id}}, &cancel); }
void RuntimeClient::tick(const Cancellation &cancel) {
    cancel.check(); const auto snapshot = request({{"command", "snapshot"}, {"afterSeq", qint64(afterSequence_)}}); receive(snapshot);
    if (!snapshot.value("runtimeFailure").toString().isEmpty()) throw std::runtime_error(snapshot.value("runtimeFailure").toString().toUtf8().constData());
}
void RuntimeClient::stop() { operation({{"command", "stop"}}, nullptr); }
bool RuntimeClient::empty() const { std::lock_guard lock(snapshotMutex_); return empty_; }
QList<TaskStatus> RuntimeClient::statuses() const { std::lock_guard lock(snapshotMutex_); return statuses_; }
void RuntimeClient::writeInput(const QString &task, const QByteArray &bytes) { request({{"command", "input"}, {"task", task}, {"bytes", QString::fromLatin1(bytes.toBase64())}}); }
void RuntimeClient::resizeTerminal(const QString &task, int columns, int rows) { request({{"command", "resize"}, {"task", task}, {"columns", columns}, {"rows", rows}}); }
}
