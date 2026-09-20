#include "TaskSupervisor.h"
#include "LaunchPlanner.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <algorithm>
#include <QSet>
#include <QTcpSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QRegularExpression>

namespace cst {
namespace {
[[noreturn]] void taskError(const QString &message) { throw std::runtime_error(message.toUtf8().constData()); }
}
TaskSupervisor::TaskSupervisor(IProcessRunner &runner, IClock &clock, ProjectPaths paths)
    : runner_(runner), clock_(clock), paths_(std::move(paths)) {}
QJsonObject TaskSupervisor::expand(const QJsonObject &object) const { return LaunchPlanner(runner_, paths_).expand(object, project_); }
ProcessSpec TaskSupervisor::command(const QJsonObject &task, const QJsonObject &spec) const {
    try { return LaunchPlanner(runner_, paths_).resolve(project_, task, spec, operationId_); }
    catch (const std::exception &e) {
        taskError("解析命令失败：任务 " + task.value("name").toString(task.value("id").toString()) +
                  " / " + spec.value("name").toString(spec.value("id").toString()) +
                  "（" + spec.value("mode").toString("exec") + "）\n" + QString::fromUtf8(e.what()));
    }
}
void TaskSupervisor::notify(Task &task, const QString &state) {
    task.state = state;
    if (taskChanged) taskChanged({task.configuration.value("id").toString(), task.configuration.value("name").toString(), state, task.budget.attempts()});
}
void TaskSupervisor::log(const QString &taskId, const QString &message) {
    ProcessOutput item{"cst", message, false, QDateTime::currentDateTimeUtc()};
    item.projectId = project_.value("id").toString(); item.runId = operationId_;
    if (output) output(taskId, std::move(item));
}
void TaskSupervisor::preflight(const QJsonObject &project, const Cancellation &cancel) {
    if (!empty()) taskError("有残留托管进程，不能启动");
    project_ = project;
    const auto id = project.value("id").toString();
    if (!QDir().mkpath(paths_.dataDirectory(id)) || !QDir().mkpath(paths_.logDirectory(id))) taskError("无法创建项目数据或日志目录");
    const auto directory = project.value("source").toObject().value("workingDirectory").toString();
    if (!directory.trimmed().isEmpty()) {
        const auto canonical = QFileInfo(directory).canonicalFilePath();
        for (const auto &reserved : {paths_.installDirectory, paths_.storageDirectory, paths_.windowsDirectory})
            if (isWithinWindowsPath(canonical, reserved)) taskError("源码目录的实际路径位于受保护目录内");
    }
    // Files produced by earlier steps are checked by the backend at spawn time.
    cancel.check();
}
void TaskSupervisor::prepare(Task &task, const QJsonObject &spec, const Cancellation &cancel) {
    notify(task, "Preparing");
    const auto processSpec = command(task.configuration, spec);
    log(processSpec.taskId, "执行准备命令：" + spec.value("name").toString() + " | " + processSpec.program + ' ' + processSpec.arguments.join(' ') + " | 工作目录：" + processSpec.workingDirectory);
    const auto diagnostic = std::make_shared<AttemptOutput>();
    preparing_ = spawn(processSpec, diagnostic);
    registerSession(processSpec.taskId, preparing_);
    const auto deadline = clock_.monotonicMs() + spec.value("timeoutMs").toInt();
    while (preparing_->rootRunning()) {
        cancel.check(); tick(cancel);
        if (clock_.monotonicMs() >= deadline) { preparing_->forceStop(); taskError("准备命令超时：" + spec.value("name").toString()); }
        clock_.sleep(25, cancel);
    }
    const auto result = preparing_->result();
    preparing_->forceStop();
    preparing_.reset();
    if (!result || result->crashed || !spec.value("successExitCodes").toArray().contains(double(result->exitCode))) {
        notify(task, "Failed");
        taskError("命令失败：" + task.configuration.value("name").toString() + " / " + spec.value("name").toString(spec.value("id").toString()) +
                  "\n程序：" + processSpec.program + "\n工作目录：" + processSpec.workingDirectory + "\n" + failureDetails(result, diagnostic));
    }
}
void TaskSupervisor::launch(Task &task, const Cancellation &cancel) {
    cancel.check();
    notify(task, "Starting");
    const auto spec = command(task.configuration, task.configuration.value("serviceCommand").toObject());
    const auto taskId = task.configuration.value("id").toString();
    const auto commandText = spec.shellCommandLine.isEmpty() ? (spec.program + ' ' + spec.arguments.join(' ')) : (spec.program + ' ' + spec.shellCommandLine);
    log(taskId, "启动服务命令：" + commandText + " | 工作目录：" + spec.workingDirectory);
    task.startedAt = clock_.monotonicMs();
    task.output = std::make_shared<AttemptOutput>();
    task.process = spawn(spec, task.output);
    registerSession(spec.taskId, task.process);
    log(taskId, "服务进程已创建，PID=" + QString::number(task.process->rootPid()));
    notify(task, "Running");
    waitReady(task, cancel);
}
void TaskSupervisor::start(const QJsonObject &project, const QString &operationId, const Cancellation &cancel) {
    if (!empty()) taskError("有残留托管进程，不能启动");
    tasks_.clear(); project_ = project; operationId_ = operationId; stopping_ = false;
    const auto configurations = project.value("tasks").toArray();
    for (const auto &value : configurations) {
        const auto task = value.toObject(); const auto policy = task.value("restartPolicy").toObject();
        Task initialized;
        initialized.configuration = task;
        initialized.budget = RestartBudget({policy.value("maxRestarts").toInt(5), qint64(policy.value("windowSeconds").toInt(600)) * 1000,
            policy.value("backoffSeconds").toInt(1), policy.value("maxBackoffSeconds").toInt(30)});
        tasks_.push_back(std::move(initialized));
    }
    std::sort(tasks_.begin(), tasks_.end(), [](const Task &a, const Task &b) { return a.configuration.value("order").toInt() < b.configuration.value("order").toInt(); });
    std::vector<Task> sorted;
    QSet<QString> added;
    while (sorted.size() < tasks_.size()) {
        bool progress = false;
        for (const auto &task : tasks_) {
            const auto id = task.configuration.value("id").toString();
            if (added.contains(id)) continue;
            bool eligible = true;
            for (const auto &dep : task.configuration.value("dependsOn").toArray())
                if (!added.contains(dep.toObject().value("task").toString())) eligible = false;
            if (eligible) { sorted.push_back(task); added.insert(id); progress = true; }
        }
        if (!progress) taskError("任务依赖循环或目标不存在");
    }
    tasks_ = std::move(sorted);
    for (auto &task : tasks_) {
        try {
            for (const auto &prepareSpec : task.configuration.value("prepareCommands").toArray()) { cancel.check(); prepare(task, prepareSpec.toObject(), cancel); }
            if (task.configuration.value("kind").toString() == "task") {
                prepare(task, task.configuration.value("serviceCommand").toObject(), cancel);
                notify(task, "Completed");
            } else launch(task, cancel);
        } catch (const Cancelled &) { throw; }
        catch (const std::exception &) { notify(task, "Failed"); throw; }
    }
    tick(cancel);
}
void TaskSupervisor::tick(const Cancellation &cancel) {
    if (stopping_) return;
    cancel.check();
    for (auto &task : tasks_) {
        if (task.restartAt) {
            if (clock_.monotonicMs() < *task.restartAt) continue;
            task.restarting = true;
            try { launch(task, cancel); task.restartAt.reset(); task.restarting = false; }
            catch (...) { task.restarting = false; notify(task, "Failed"); throw; }
            continue;
        }
        if (task.restarting || (task.state != "Running" && task.state != "Ready" && task.state != "Restarting") || !task.process) continue;
        const bool tree = task.configuration.value("lifetime").toString() == "tree";
        const auto rootResult = task.process->result();
        const auto successCodes = task.configuration.value("serviceCommand").toObject().value("successExitCodes").toArray();
        const bool success = rootResult && !rootResult->crashed && successCodes.contains(double(rootResult->exitCode));
        if ((task.process->rootRunning() || (tree && success)) && !task.process->treeEmpty()) continue;
        const auto result = task.process->result();
        const auto uptime = task.startedAt ? clock_.monotonicMs() - *task.startedAt : 0;
        task.process->forceStop();
        log(task.configuration.value("id").toString(), "服务进程退出，运行 " + QString::number(uptime) + " ms\n" + failureDetails(result, task.output));
        const auto restart = task.configuration.value("restartPolicy").toObject();
        const auto mode = restart.value("mode").toString("always");
        if (mode == "never" || (mode == "on_failure" && success)) {
            notify(task, success ? "Exited" : "Failed");
            taskError("长期服务已退出：" + task.configuration.value("name").toString() + "\n" + failureDetails(result, task.output));
        }
        const auto resetSeconds = restart.value("resetAfterSeconds").toInt();
        if (resetSeconds > 0 && uptime >= qint64(resetSeconds) * 1000) task.budget.reset();
        const auto delay = task.budget.schedule(clock_.monotonicMs());
        if (!delay) {
            const auto message = "服务重启次数超过滑动窗口限制，任务失败：" + task.configuration.value("name").toString() + "\n" + failureDetails(result, task.output);
            log(task.configuration.value("id").toString(), message);
            notify(task, "Failed");
            throw std::runtime_error(message.toUtf8().constData());
        }
        task.restartAt = clock_.monotonicMs() + *delay;
        notify(task, "Restarting");
        log(task.configuration.value("id").toString(), "将在 " + QString::number(*delay / 1000) + " 秒后重启，累计重启次数 " + QString::number(task.budget.attempts()) + "/" + QString::number(task.configuration.value("restartPolicy").toObject().value("maxRestarts").toInt()));
    }
}

std::shared_ptr<IManagedProcess> TaskSupervisor::spawn(const ProcessSpec &spec, const std::shared_ptr<AttemptOutput> &diagnostic) {
    try { return runner_.start(spec, consumer(spec, diagnostic)); }
    catch (const std::exception &e) {
        taskError("创建进程失败：任务 " + spec.taskId + "\n程序：" + spec.program + "\n工作目录：" + spec.workingDirectory +
                  "\nConsole：" + (spec.terminal ? QString("terminal") : QString("pipes")) + "\n" + QString::fromUtf8(e.what()));
    }
}
QString TaskSupervisor::failureDetails(const std::optional<ProcessResult> &result, const std::shared_ptr<AttemptOutput> &diagnostic) const {
    QString detail = "退出状态未知";
    if (result) {
        const auto code = static_cast<quint32>(result->exitCode);
        detail = QString("%1 %2（0x%3）").arg(result->crashed ? "异常退出代码" : "退出代码").arg(result->exitCode)
                     .arg(code, 8, 16, QLatin1Char('0'));
        if (code == 0xc0000135u) detail += "\n缺少运行时 DLL；请检查该工具的安装和依赖路径。";
        if (code == 0xc0000142u) detail += "\n进程初始化失败；请检查 Console 初始化及该工具的运行环境。";
    }
    if (diagnostic) {
        QString tail;
        { std::lock_guard lock(diagnostic->mutex); tail = diagnostic->tail; }
        // Remove common VT presentation sequences from the diagnostic excerpt.
        tail.remove(QRegularExpression(QStringLiteral("\x1b\\[[0-?]*[ -/]*[@-~]")));
        tail.remove(QRegularExpression(QStringLiteral("\x1b\\][^\x07\x1b]*(?:\x07|\x1b\\\\)")));
        if (!tail.trimmed().isEmpty()) detail += "\n最后输出：\n" + tail.trimmed();
    }
    return detail;
}
std::function<void(ProcessOutput)> TaskSupervisor::consumer(const ProcessSpec &spec, const std::shared_ptr<AttemptOutput> &diagnostic) {
    return [this, spec, diagnostic](ProcessOutput item) {
        if (diagnostic && (item.channel == "stdout" || item.channel == "stderr" || item.channel == "terminal")) {
            std::lock_guard lock(diagnostic->mutex);
            diagnostic->tail += item.text.right(8192);
            if (diagnostic->tail.size() > 8192) diagnostic->tail = diagnostic->tail.right(8192);
        }
        item.projectId = spec.projectId; item.runId = spec.operationId; item.attemptId = spec.attemptId;
        if (output) output(spec.taskId, std::move(item));
    };
}
void TaskSupervisor::registerSession(const QString &id, const std::shared_ptr<IManagedProcess> &session) {
    std::lock_guard lock(sessionsMutex_); sessions_[id] = session;
}
void TaskSupervisor::writeInput(const QString &id, const QByteArray &bytes) {
    std::shared_ptr<IManagedProcess> session;
    { std::lock_guard lock(sessionsMutex_); session = sessions_.value(id); }
    if (!session || session->treeEmpty()) taskError("任务已退出，不能输入");
    session->writeInput(bytes);
}
void TaskSupervisor::resizeTerminal(const QString &id, int columns, int rows) {
    std::shared_ptr<IManagedProcess> session;
    { std::lock_guard lock(sessionsMutex_); session = sessions_.value(id); }
    if (session && !session->treeEmpty()) session->resizeTerminal(columns, rows);
}
void TaskSupervisor::waitReady(Task &task, const Cancellation &cancel) {
    const auto probe = task.configuration.value("readiness").toObject();
    const auto type = probe.value("type").toString("none");
    if (type == "none") return;
    const auto deadline = clock_.monotonicMs() + probe.value("timeoutMs").toInt(30000);
    while (clock_.monotonicMs() < deadline) {
        cancel.check();
        if (task.process->treeEmpty()) {
            const auto result = task.process->result(); task.process->forceStop();
            taskError("就绪前进程已退出：" + task.configuration.value("name").toString() + "\n" + failureDetails(result, task.output));
        }
        bool ready = false;
        if (type == "tcp") {
            QTcpSocket socket; socket.connectToHost(probe.value("host").toString(), quint16(probe.value("port").toInt()));
            ready = socket.waitForConnected(100);
        } else if (type == "http") {
            QNetworkAccessManager manager;
            auto *reply = manager.get(QNetworkRequest(QUrl(probe.value("url").toString())));
            QEventLoop loop; QTimer timer; timer.setSingleShot(true);
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            timer.start(200); loop.exec();
            const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            ready = reply->isFinished() && reply->error() == QNetworkReply::NoError && code >= 200 && code < 400;
            if (!reply->isFinished()) reply->abort();
        }
        if (ready) { notify(task, "Ready"); return; }
        clock_.sleep(100, cancel);
    }
    taskError("服务就绪检查超时：" + task.configuration.value("name").toString());
}

void TaskSupervisor::stop() {
    stopping_ = true;
    QStringList errors;
    if (preparing_) {
        try { preparing_->stop(0); } catch (const std::exception &e) { errors.append(QString::fromUtf8(e.what())); try { preparing_->forceStop(); } catch (const std::exception &f) { errors.append(QString::fromUtf8(f.what())); } }
    }
    for (auto it = tasks_.rbegin(); it != tasks_.rend(); ++it) {
        auto &task = *it; task.restartAt.reset();
        if (!task.process) continue;
        notify(task, "Stopping");
        try { task.process->stop(task.configuration.value("shutdownGraceMs").toInt()); }
        catch (const std::exception &e) {
            errors.append(QString::fromUtf8(e.what()));
            try { task.process->forceStop(); } catch (const std::exception &f) { errors.append(QString::fromUtf8(f.what())); }
        }
        notify(task, task.process->empty() ? "Stopped" : "Failed");
    }
    if (!empty()) errors.append("停止后仍有托管进程");
    if (!errors.isEmpty()) taskError(errors.join('\n'));
}
bool TaskSupervisor::empty() const {
    if (preparing_ && !preparing_->empty()) return false;
    for (const auto &task : tasks_) if (task.process && !task.process->empty()) return false;
    return true;
}
QList<TaskStatus> TaskSupervisor::statuses() const {
    QList<TaskStatus> result;
    for (const auto &task : tasks_) result.append({task.configuration.value("id").toString(), task.configuration.value("name").toString(), task.state, task.budget.attempts()});
    return result;
}
}
