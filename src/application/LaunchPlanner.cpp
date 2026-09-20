#include "LaunchPlanner.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
#include <QUuid>

namespace cst {
namespace {
QStringList strings(const QJsonArray &values) {
    QStringList result;
    for (const auto &value : values) result.append(value.toString());
    return result;
}
Environment values(const QJsonObject &object) {
    Environment result;
    for (auto it = object.begin(); it != object.end(); ++it) result[it.key().toUpper()] = it.value().toString();
    return result;
}
QString pathAt(QString path, QString root) {
    path.replace('\\', '/'); root.replace('\\', '/');
    if (path.isEmpty() || isWindowsAbsolutePath(path) || QDir::isAbsolutePath(path)) return path;
    return QDir::cleanPath(root + '/' + path);
}
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toUtf8().constData()); }
// This is deliberately NOT the native argv quoting algorithm. Legacy shell syntax
// has expansion rules that cannot represent all native argv values transparently.
QString batchWord(const QString &word) {
    if (word.contains(QChar::Null) || word.contains('"') || word.contains('%') || word.contains('\n') || word.contains('\r'))
        fail("批处理参数包含不支持的引号、百分号或换行；请使用原生程序/venv/npm 模式");
    return '"' + word + '"';
}
}
LaunchPlanner::LaunchPlanner(IProcessRunner &runner, ProjectPaths paths) : runner_(runner), paths_(std::move(paths)) {}
QJsonObject LaunchPlanner::expand(const QJsonObject &object, const QJsonObject &project) const {
    const auto id = project.value("id").toString();
    const QMap<QString, QString> replacements{{"PROJECT_DIR", project.value("source").toObject().value("workingDirectory").toString()},
        {"DATA_DIR", paths_.dataDirectory(id)}, {"LOG_DIR", paths_.logDirectory(id)}};
    const auto visit = [&](auto &&self, const QJsonValue &value) -> QJsonValue {
        if (value.isString()) return expandPlaceholders(value.toString(), replacements);
        if (value.isArray()) { QJsonArray result; for (const auto &v : value.toArray()) result.append(self(self, v)); return result; }
        if (value.isObject()) { auto result = value.toObject(); for (auto it = result.begin(); it != result.end(); ++it) it.value() = self(self, it.value()); return result; }
        return value;
    };
    return visit(visit, object).toObject();
}
Environment LaunchPlanner::environment(const QJsonObject &project, const QJsonObject &task, const QJsonObject &command) const {
    const auto inherited = runner_.inheritedEnvironment();
    Environment result;
    const auto profiles = project.value("environments").toObject();
    QList<QJsonObject> layers;
    QSet<QString> visited;
    auto ref = task.value("environmentRef").toString();
    while (!ref.isEmpty()) {
        if (visited.contains(ref) || !profiles.value(ref).isObject()) fail("环境引用不存在或循环：" + ref);
        visited.insert(ref);
        const auto profile = profiles.value(ref).toObject(); layers.prepend(profile); ref = profile.value("extends").toString();
    }
    layers.prepend(project.value("environment").toObject());
    layers.append(task.value("environment").toObject()); layers.append(command.value("environment").toObject());
    bool inherit = true;
    for (const auto &layer : layers) if (layer.contains("inheritSystem")) inherit = layer.value("inheritSystem").toBool();
    if (inherit) result = mergeEnvironment(true, inherited, {}, {}, {});
    else for (const auto &key : {"SYSTEMROOT", "WINDIR", "COMSPEC", "TEMP", "TMP"})
        if (inherited.contains(key)) result[key] = inherited.value(key);
    const auto root = project.value("source").toObject().value("workingDirectory").toString();
    for (const auto &layer : layers) {
        for (const auto &fileName : layer.value("envFiles").toArray()) {
            QFile file(pathAt(fileName.toString(), root));
            if (!file.open(QIODevice::ReadOnly)) fail("无法读取环境文件：" + file.fileName());
            result = mergeEnvironment(true, result, {parseEnv(file.readAll())}, {}, {});
        }
        result = mergeEnvironment(true, result, {}, values(layer.value("variables").toObject()), {});
    }
    QStringList prefixes;
    for (const auto &tool : project.value("toolDirectories").toArray()) prefixes.append(normalizeWindowsPathInput(pathAt(tool.toString(), root)));
    if (!result.value("PATH").isEmpty()) prefixes.append(result.value("PATH"));
    if (!prefixes.isEmpty()) result["PATH"] = prefixes.join(';');
    return result;
}
ProcessSpec LaunchPlanner::resolve(const QJsonObject &rawProject, const QJsonObject &rawTask,
                                  const QJsonObject &rawCommand, const QString &runId) const {
    const auto project = expand(rawProject, rawProject), task = expand(rawTask, rawProject), command = expand(rawCommand, rawProject);
    ProcessSpec plan;
    plan.projectId = project.value("id").toString(); plan.taskId = task.value("id").toString(); plan.operationId = runId;
    plan.attemptId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto root = project.value("source").toObject().value("workingDirectory").toString();
    const auto directory = command.value("workingDirectory").toString();
    plan.workingDirectory = pathAt(directory.isEmpty() ? root : directory, root);
    plan.environment = environment(project, task, command);
    const auto io = command.value("io").toObject();
    plan.terminal = io.value("mode").toString() == "terminal";
    plan.inputEnabled = plan.terminal || io.value("stdin").toString() == "pipe";
    plan.encoding = io.value("encoding").toString("utf-8");
    const auto resolve = [&](const QString &program) { return runner_.resolveInEnvironment(program, plan.environment, plan.workingDirectory); };
    const auto mode = command.value("mode").toString("exec");
    auto arguments = strings(command.value("arguments").toArray());
    const auto activation = command.value("activationScript").toString();
    if (mode == "shell" || !activation.isEmpty()) {
        auto processor = plan.environment.value("COMSPEC");
        if (processor.isEmpty()) processor = paths_.windowsDirectory + "/System32/cmd.exe";
        plan.program = resolve(processor);
        auto script = command.value("script").toString();
        if (mode != "shell") {
            // Lookup occurs in the activated shell, never before activation.
            script = batchWord(command.value("program").toString());
            for (const auto &arg : arguments) script += ' ' + batchWord(arg);
        }
        if (!activation.isEmpty()) {
            const auto path = pathAt(activation, plan.workingDirectory);
            if (!QFileInfo(path).isFile()) fail("激活脚本不存在：" + path + "\n.venv 项目可使用 python-venv 模式；其他环境请检查激活脚本相对于命令工作目录的位置。");
            if (!path.endsWith(".bat", Qt::CaseInsensitive) && !path.endsWith(".cmd", Qt::CaseInsensitive))
                fail("cmd 激活脚本必须是 .bat 或 .cmd 文件；Python .venv 请选择 python-venv 模式。");
            script = "call " + batchWord(normalizeWindowsPathInput(path)) + " && " + script;
        }
        plan.shellCommandLine = "/D /V:OFF /S /C \"" + script + '"';
    } else if (mode == "python-venv") {
        const auto venv = pathAt(command.value("venv").toString(), plan.workingDirectory);
        if (venv.isEmpty()) fail("请指定 Python 虚拟环境目录");
        const auto variables = plan.environment;
        if (variables.contains("PYTHONHOME")) plan.environment.remove("PYTHONHOME");
        plan.environment["VIRTUAL_ENV"] = venv;
        plan.environment["PATH"] = venv + "/Scripts;" + plan.environment.value("PATH");
        try { plan.program = resolve(venv + "/Scripts/python.exe"); }
        catch (const std::exception &e) {
            fail("Python 虚拟环境不可用：" + venv + "\n请先在准备命令中执行 python -m venv 或 uv venv 创建 Windows 环境；从其他机器复制的环境可能需要重建。\n" + QString::fromUtf8(e.what()));
        }
        plan.arguments = arguments;
        if (!plan.environment.contains("PYTHONUNBUFFERED")) plan.environment["PYTHONUNBUFFERED"] = "1";
        if (!plan.terminal && plan.encoding == "utf-8" && !plan.environment.contains("PYTHONIOENCODING"))
            plan.environment["PYTHONIOENCODING"] = "utf-8";
    } else if (mode == "uv") {
        plan.program = resolve(command.value("program").toString().isEmpty() ? "uv.exe" : command.value("program").toString());
        plan.arguments = QStringList{"run"} + strings(command.value("toolArguments").toArray()) + QStringList{"--"} + arguments;
    } else if (mode == "npm") {
        plan.program = resolve(command.value("program").toString().isEmpty() ? "node.exe" : command.value("program").toString());
        // npm lifecycle scripts invoke `node` again; they must use this Node even
        // when it was selected by an absolute path outside the inherited PATH.
        auto nodePath = plan.program; nodePath.replace('\\', '/');
        const auto nodeDirectory = nodePath.left(nodePath.lastIndexOf('/'));
        if (nodePath.contains('/')) plan.environment["PATH"] = nodeDirectory + ';' + plan.environment.value("PATH");
        auto cli = command.value("npmCli").toString();
        if (cli.isEmpty()) cli = nodeDirectory + "/node_modules/npm/bin/npm-cli.js";
        else cli = pathAt(cli, plan.workingDirectory);
        if (!QFileInfo(cli).isFile()) fail("找不到此 Node 安装的 npm-cli.js，请配置 npmCli：" + cli);
        const auto action = command.value("npmAction").toString("run");
        plan.arguments = QStringList{cli, action};
        if (action == "run") plan.arguments.append(command.value("script").toString());
        plan.arguments += strings(command.value("toolArguments").toArray());
        if (!arguments.isEmpty()) plan.arguments += QStringList{"--"} + arguments;
    } else {
        plan.program = resolve(command.value("program").toString()); plan.arguments = arguments;
    }
    plan.environment["CST_PROJECT_ID"] = plan.projectId;
    plan.environment["CST_PROJECT_DIR"] = project.value("source").toObject().value("workingDirectory").toString();
    plan.environment["CST_DATA_DIR"] = paths_.dataDirectory(plan.projectId);
    plan.environment["CST_LOG_DIR"] = paths_.logDirectory(plan.projectId);
    plan.environment["CST_RUN_ID"] = runId; plan.environment["CST_ATTEMPT_ID"] = plan.attemptId;
    return plan;
}
}
