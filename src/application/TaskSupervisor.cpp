#include "TaskSupervisor.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <algorithm>

namespace cst {
namespace {
QStringList strings(const QJsonArray &array) { QStringList values; for (const auto &value : array) values.append(value.toString()); return values; }
Environment environmentValues(const QJsonObject &object) {
    Environment result; for (auto it = object.begin(); it != object.end(); ++it) result.insert(it.key(), it.value().toString()); return result;
}
[[noreturn]] void taskError(const QString &message) { throw std::runtime_error(message.toUtf8().constData()); }
}
TaskSupervisor::TaskSupervisor(IProcessRunner &runner, IClock &clock, ProjectPaths paths)
    : runner_(runner), clock_(clock), paths_(std::move(paths)) {}
QJsonObject TaskSupervisor::expand(const QJsonObject &object) const {
    const auto id = project_.value("id").toString();
    const QMap<QString, QString> values{{"PROJECT_DIR", project_.value("source").toObject().value("workingDirectory").toString()},
                                       {"DATA_DIR", paths_.dataDirectory(id)}, {"LOG_DIR", paths_.logDirectory(id)}};
    const auto replace = [&](auto &&self, const QJsonValue &value) -> QJsonValue {
        if (value.isString()) return expandPlaceholders(value.toString(), values);
        if (value.isArray()) { QJsonArray result; for (const auto &item : value.toArray()) result.append(self(self, item)); return result; }
        if (value.isObject()) { auto result = value.toObject(); for (auto it = result.begin(); it != result.end(); ++it) it.value() = self(self, it.value()); return result; }
        return value;
    };
    return replace(replace, object).toObject();
}
ProcessSpec TaskSupervisor::command(const QJsonObject &task, const QJsonObject &rawCommand) const {
    const auto spec = expand(rawCommand);
    const auto expandedTask = expand(task);
    const auto env = expandedTask.value("environment").toObject();
    QList<Environment> files;
    for (const auto &path : env.value("envFiles").toArray()) {
        QFile file(path.toString());
        if (!file.open(QIODevice::ReadOnly)) taskError("无法读取环境文件：" + path.toString() + "：" + file.errorString());
        files.append(parseEnv(file.readAll()));
    }
    const auto id = project_.value("id").toString();
    const auto projectDirectory = project_.value("source").toObject().value("workingDirectory").toString();
    ProcessSpec result;
    result.projectId = id; result.taskId = task.value("id").toString(); result.operationId = operationId_;
    result.workingDirectory = spec.value("workingDirectory").toString();
    if (result.workingDirectory.trimmed().isEmpty()) result.workingDirectory = expandedTask.value("workingDirectory").toString();
    result.environment = mergeEnvironment(env.value("inheritSystem").toBool(), runner_.inheritedEnvironment(), files,
        environmentValues(env.value("variables").toObject()), {{"CST_PROJECT_ID", id}, {"CST_PROJECT_DIR", projectDirectory},
        {"CST_DATA_DIR", paths_.dataDirectory(id)}, {"CST_LOG_DIR", paths_.logDirectory(id)}});
    if (spec.value("mode") == "shell") {
        result.program = runner_.inheritedEnvironment().value("SYSTEMROOT") + "\\System32\\cmd.exe";
        result.program = runner_.resolveExecutable(result.program, {});
        result.shellCommandLine = "/D /S /C \"" + spec.value("script").toString() + '"';
    } else {
        const auto tools = expand(QJsonObject{{"tools", project_.value("toolDirectories")}}).value("tools").toArray();
        result.program = runner_.resolveExecutable(spec.value("program").toString(), strings(tools));
        result.arguments = strings(spec.value("arguments").toArray());
    }
    return result;
}
void TaskSupervisor::notify(Task &task, const QString &state) {
    task.state = state;
    if (taskChanged) taskChanged({task.configuration.value("id").toString(), task.configuration.value("name").toString(), state, task.budget.attempts()});
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
    for (const auto &value : project.value("tasks").toArray()) {
        cancel.check(); const auto task = value.toObject(); const auto expandedTask = expand(task);
        const auto checkDirectory = [&](const QJsonObject &raw, const QString &label) {
            const auto spec = expand(raw);
            auto commandDirectory = spec.value("workingDirectory").toString();
            if (commandDirectory.trimmed().isEmpty()) commandDirectory = expandedTask.value("workingDirectory").toString();
            if (!QFileInfo(commandDirectory).isDir()) taskError("命令工作目录不存在：" + task.value("name").toString() + " / " + label + "：" + commandDirectory);
        };
        for (const auto &spec : task.value("prepareCommands").toArray()) { cancel.check(); checkDirectory(spec.toObject(), spec.toObject().value("name").toString()); command(task, spec.toObject()); }
        const auto service = task.value("serviceCommand").toObject();
        checkDirectory(service, service.value("name").toString());
        command(task, service);
    }
}
void TaskSupervisor::prepare(Task &task, const QJsonObject &spec, const Cancellation &cancel) {
    notify(task, "Preparing");
    const auto processSpec = command(task.configuration, spec);
    preparing_ = runner_.start(processSpec, [this, id = processSpec.taskId](ProcessOutput line) { if (output) output(id, std::move(line)); });
    const auto deadline = clock_.monotonicMs() + spec.value("timeoutMs").toInt();
    while (preparing_->rootRunning()) {
        cancel.check(); tick(cancel);
        if (clock_.monotonicMs() >= deadline) { preparing_->forceStop(); taskError("准备命令超时：" + spec.value("name").toString()); }
        clock_.sleep(25, cancel);
    }
    const auto result = preparing_->result();
    if (!preparing_->empty()) preparing_->forceStop();
    preparing_.reset();
    if (!result || result->crashed || !spec.value("successExitCodes").toArray().contains(double(result->exitCode))) taskError("准备命令失败：" + spec.value("name").toString());
}
void TaskSupervisor::launch(Task &task, const Cancellation &cancel) {
    cancel.check();
    notify(task, "Starting");
    const auto spec = command(task.configuration, task.configuration.value("serviceCommand").toObject());
    task.process = runner_.start(spec, [this, id = spec.taskId](ProcessOutput line) { if (output) output(id, std::move(line)); });
    notify(task, "Running");
}
void TaskSupervisor::start(const QJsonObject &project, const QString &operationId, const Cancellation &cancel) {
    if (!empty()) taskError("有残留托管进程，不能启动");
    tasks_.clear(); project_ = project; operationId_ = operationId; stopping_ = false;
    const auto configurations = project.value("tasks").toArray();
    for (const auto &value : configurations) {
        const auto task = value.toObject(); const auto policy = task.value("restartPolicy").toObject();
        tasks_.push_back({task, {}, RestartBudget({policy.value("maxRestarts").toInt(), qint64(policy.value("windowSeconds").toInt()) * 1000,
            policy.value("backoffSeconds").toInt(), policy.value("maxBackoffSeconds").toInt()}), "Stopped", {}, false});
    }
    std::sort(tasks_.begin(), tasks_.end(), [](const Task &a, const Task &b) { return a.configuration.value("order").toInt() < b.configuration.value("order").toInt(); });
    for (auto &task : tasks_) {
        for (const auto &prepareSpec : task.configuration.value("prepareCommands").toArray()) { cancel.check(); prepare(task, prepareSpec.toObject(), cancel); }
        launch(task, cancel);
    }
    for (;;) {
        tick(cancel);
        const bool allRunning = std::all_of(tasks_.begin(), tasks_.end(), [](const Task &task) { return task.state == "Running"; });
        if (allRunning) break;
        clock_.sleep(25, cancel);
    }
}
void TaskSupervisor::tick(const Cancellation &cancel) {
    if (stopping_) return;
    cancel.check();
    for (auto &task : tasks_) {
        if (task.restarting || (task.state != "Running" && task.state != "Restarting")) continue;
        if (!task.restartAt && (!task.process->rootRunning() || task.process->empty())) {
            task.process->forceStop();
            const auto delay = task.budget.schedule(clock_.monotonicMs());
            if (!delay) { notify(task, "Failed"); taskError("服务重启次数超过滑动窗口限制：" + task.configuration.value("name").toString()); }
            task.restartAt = clock_.monotonicMs() + *delay; notify(task, "Restarting");
        }
        if (task.restartAt && clock_.monotonicMs() >= *task.restartAt) {
            task.restarting = true;
            try { launch(task, cancel); task.restartAt.reset(); task.restarting = false; }
            catch (...) { task.restarting = false; notify(task, "Failed"); throw; }
        }
    }
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
