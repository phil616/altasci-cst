#include "RuntimeClient.h"
#include "LogService.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QTimer>
#include <condition_variable>
#include <deque>
#include <thread>

namespace cst {
int serveRuntime(ITaskSupervisor &supervisor, const ProjectPaths &paths, const QString &name, const QString &token,
                 std::function<bool()> ownerAlive) {
    LogService logs(paths.storageDirectory);
    struct State {
        std::mutex mutex;
        std::condition_variable wake;
        QJsonObject pending;
        QString operation, finished, failure, runtimeFailure;
        bool busy = false, exit = false, running = false, empty = true;
        QMap<QString, TaskStatus> tasks;
        std::deque<QJsonObject> output;
        qsizetype outputSize = 0;
        quint64 sequence = 0, dropped = 0;
        Cancellation cancel;
    } state;
    const auto outputSize = [](const QJsonObject &o) { return o.value("text").toString().size() * 2 + o.value("bytes").toString().size() * 2 + 1024; };
    supervisor.output = [&](const QString &task, ProcessOutput out) {
        QJsonObject item{{"task", task}, {"channel", out.channel}, {"text", out.text}, {"bytes", QString::fromLatin1(out.bytes.toBase64())},
            {"ts", out.timestamp.toUTC().toString(Qt::ISODateWithMs)}, {"projectId", out.projectId}, {"runId", out.runId}, {"attemptId", out.attemptId}, {"decodeError", out.decodeError}};
        if (out.channel != "terminal" && out.channel != "record" && !out.projectId.isEmpty())
            logs.write(out.projectId, task, out.runId, "process." + out.channel, out.text, "info", out.channel, out.decodeError, out.timestamp, out.attemptId);
        std::lock_guard lock(state.mutex); item["seq"] = qint64(++state.sequence);
        state.outputSize += outputSize(item); state.output.push_back(std::move(item));
        while (state.outputSize > 8 * 1024 * 1024 && !state.output.empty()) { state.outputSize -= outputSize(state.output.front()); state.output.pop_front(); ++state.dropped; }
    };
    QObject::connect(&logs, &LogService::lineWritten, &logs, [&](const QString &task, const QString &record) {
        ProcessOutput out{"record", record, false, QDateTime::currentDateTimeUtc()}; supervisor.output(task, std::move(out));
    });
    QObject::connect(&logs, &LogService::failed, &logs, [&](const QString &error) {
        ProcessOutput out{"gap", error, false, QDateTime::currentDateTimeUtc()}; supervisor.output({}, std::move(out));
    });
    supervisor.taskChanged = [&](const TaskStatus &task) { std::lock_guard lock(state.mutex); state.tasks[task.id] = task; };
    QLocalServer server; server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(name)) return 10;
    std::thread worker([&] {
        std::unique_ptr<QLockFile> projectLock;
        for (;;) {
            QJsonObject command; bool exiting = false, running = false;
            { std::unique_lock lock(state.mutex); state.wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return state.exit || !state.pending.isEmpty(); });
              exiting = state.exit; running = state.running; command = std::exchange(state.pending, {}); }
            if (exiting) { try { supervisor.stop(); } catch (...) {} break; }
            QString failure;
            try {
                const auto action = command.value("command").toString();
                if (action == "preflight") {
                    const auto project = command.value("project").toObject();
                    projectLock = std::make_unique<QLockFile>(paths.storageDirectory + "/locks/" + project.value("id").toString() + ".lock");
                    QDir().mkpath(paths.storageDirectory + "/locks"); projectLock->setStaleLockTime(0);
                    if (!projectLock->tryLock(0)) throw std::runtime_error("项目正由其他运行或同步操作使用");
                    supervisor.preflight(project, state.cancel);
                } else if (action == "start") {
                    supervisor.start(command.value("project").toObject(), command.value("runId").toString(), state.cancel);
                    const bool alive = !supervisor.empty();
                    if (!alive) projectLock.reset();
                    std::lock_guard lock(state.mutex); state.running = alive;
                } else if (action == "stop") {
                    supervisor.stop(); if (supervisor.empty()) projectLock.reset();
                    std::lock_guard lock(state.mutex); state.running = false; state.runtimeFailure.clear();
                } else if (running) supervisor.tick(state.cancel);
            } catch (const std::exception &e) { failure = QString::fromUtf8(e.what()); }
            const bool empty = supervisor.empty();
            { std::lock_guard lock(state.mutex); state.empty = empty;
              if (!command.isEmpty()) { state.busy = false; state.finished = command.value("requestId").toString(); state.failure = failure; }
              else if (!failure.isEmpty()) { state.runtimeFailure = failure; state.running = false; }
            }
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), &QCoreApplication::quit, Qt::QueuedConnection);
    });
    const auto handle = [&](const QJsonObject &request) -> QJsonObject {
        if (request.value("version").toInt() != 1 || request.value("token").toString() != token) return {{"error", "运行协议身份验证失败"}};
        const auto command = request.value("command").toString();
        if (command == "input") { supervisor.writeInput(request.value("task").toString(), QByteArray::fromBase64(request.value("bytes").toString().toLatin1())); return {}; }
        if (command == "resize") { supervisor.resizeTerminal(request.value("task").toString(), request.value("columns").toInt(), request.value("rows").toInt()); return {}; }
        std::lock_guard lock(state.mutex);
        if (command == "cancel") { state.cancel.requested.store(true); return {}; }
        if (command == "shutdown") { state.exit = true; state.cancel.requested.store(true); state.wake.notify_all(); return {}; }
        if (command == "snapshot") {
            QJsonArray tasks, output;
            for (const auto &task : state.tasks) tasks.append(QJsonObject{{"id", task.id}, {"name", task.name}, {"state", task.state}, {"restartCount", qint64(task.restartCount)}});
            qsizetype size = 0; const auto after = request.value("afterSeq").toInteger();
            if (!state.output.empty() && state.output.front().value("seq").toInteger() > after + 1) {
                const auto lostUntil = state.output.front().value("seq").toInteger() - 1;
                output.append(QJsonObject{{"channel", "gap"}, {"seq", lostUntil}, {"text", "输出已覆盖，缺失序列 " + QString::number(after + 1) + "–" + QString::number(lostUntil)}});
            }
            for (const auto &item : state.output) {
                if (item.value("seq").toInteger() <= after) continue;
                if (size >= 256 * 1024) break;
                size += outputSize(item); output.append(item);
            }
            return {{"tasks", tasks}, {"output", output}, {"empty", state.empty}, {"finished", state.finished}, {"failure", state.failure}, {"runtimeFailure", state.runtimeFailure}};
        }
        if (command != "start" && command != "preflight" && command != "stop") return {{"error", "未知运行命令"}};
        const auto id = request.value("requestId").toString();
        if (id.isEmpty()) return {{"error", "缺少 requestId"}};
        if (state.operation == id || state.finished == id) return {{"operation", id}};
        if (state.busy) return {{"error", "运行宿主正在处理操作"}};
        if (command != "stop" && state.running) return {{"error", "项目已经运行"}};
        state.cancel.requested.store(false); state.operation = id; state.pending = request; state.busy = true; state.wake.notify_all();
        if (command == "preflight") { state.tasks.clear(); state.runtimeFailure.clear(); }
        return {{"operation", id}};
    };
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            socket->setReadBufferSize(32 * 1024 * 1024);
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            auto buffer = std::make_shared<QByteArray>();
            const auto receive = [&, socket, buffer] {
                *buffer += socket->readAll();
                if (buffer->size() > 16 * 1024 * 1024) { socket->abort(); return; }
                if (!buffer->endsWith('\n')) return;
                QJsonObject reply;
                try { reply = handle(QJsonDocument::fromJson(*buffer).object()); }
                catch (const std::exception &e) { reply = {{"error", QString::fromUtf8(e.what())}}; }
                socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n'); socket->disconnectFromServer();
            };
            QObject::connect(socket, &QLocalSocket::readyRead, socket, receive); receive();
        }
    });
    QTimer monitor;
    QObject::connect(&monitor, &QTimer::timeout, &server, [&] {
        if (!ownerAlive()) { std::lock_guard lock(state.mutex); state.exit = true; state.cancel.requested.store(true); state.wake.notify_all(); }
    }); monitor.start(100);
    const int code = QCoreApplication::exec();
    { std::lock_guard lock(state.mutex); state.exit = true; state.cancel.requested.store(true); state.wake.notify_all(); }
    worker.join(); logs.flush(); supervisor.output = {}; supervisor.taskChanged = {}; return code;
}
}
