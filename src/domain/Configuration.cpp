#include "Configuration.h"

#include <QDir>
#include <QHostAddress>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <cmath>
#include <stdexcept>

namespace cst {
namespace {
QString child(const QString &path, QString name) {
    name.replace('~', "~0");
    name.replace('/', "~1");
    return path + '/' + name;
}

bool typeMatches(const QJsonValue &value, const QString &type) {
    if (type == "object") return value.isObject();
    if (type == "array") return value.isArray();
    if (type == "string") return value.isString();
    if (type == "boolean") return value.isBool();
    if (type == "integer") return value.isDouble() && std::floor(value.toDouble()) == value.toDouble();
    if (type == "number") return value.isDouble();
    if (type == "null") return value.isNull();
    throw std::logic_error("Unsupported schema type");
}

void evaluate(const QJsonObject &root, const QJsonObject &rule, const QJsonValue &value,
              const QString &path, ValidationIssues &issues) {
    const auto error = [&](const QString &message) { issues.append({path, message}); };
    if (rule.contains("$ref")) {
        const auto ref = rule.value("$ref").toString();
        if (!ref.startsWith("#/$defs/")) throw std::logic_error("Non-local schema reference");
        const auto target = root.value("$defs").toObject().value(ref.mid(8));
        if (!target.isObject()) throw std::logic_error("Unknown schema reference");
        evaluate(root, target.toObject(), value, path, issues);
    }
    if (rule.contains("type") && !typeMatches(value, rule.value("type").toString())) {
        error("类型必须为 " + rule.value("type").toString());
        return;
    }
    if (rule.contains("const") && value != rule.value("const")) error("值与规定常量不符");
    if (rule.contains("enum") && !rule.value("enum").toArray().contains(value)) error("不在允许值范围内");
    const auto matches = [&](const QJsonObject &sub) {
        ValidationIssues temporary;
        evaluate(root, sub, value, path, temporary);
        return temporary.isEmpty();
    };
    for (const auto &sub : rule.value("allOf").toArray()) evaluate(root, sub.toObject(), value, path, issues);
    for (const auto &name : {QStringLiteral("oneOf"), QStringLiteral("anyOf")}) {
        if (!rule.contains(name)) continue;
        int count = 0;
        for (const auto &sub : rule.value(name).toArray()) if (matches(sub.toObject())) ++count;
        if (count == 0 || (name == "oneOf" && count != 1)) error("配置不符合 " + name + " 分支规则");
    }
    if (rule.contains("not") && matches(rule.value("not").toObject())) error("包含互斥字段");
    if (rule.contains("if")) {
        const auto name = matches(rule.value("if").toObject()) ? "then" : "else";
        if (rule.contains(name)) evaluate(root, rule.value(name).toObject(), value, path, issues);
    }
    if (value.isObject()) {
        const auto object = value.toObject();
        const auto properties = rule.value("properties").toObject();
        for (const auto &required : rule.value("required").toArray())
            if (!object.contains(required.toString())) issues.append({child(path, required.toString()), "缺少必填字段"});
        if (rule.contains("maxProperties") && object.size() > rule.value("maxProperties").toInt()) error("字段过多");
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (rule.contains("propertyNames")) evaluate(root, rule.value("propertyNames").toObject(), it.key(), child(path, it.key()), issues);
            if (properties.contains(it.key())) evaluate(root, properties.value(it.key()).toObject(), it.value(), child(path, it.key()), issues);
            else if (rule.value("additionalProperties").isBool() && !rule.value("additionalProperties").toBool())
                issues.append({child(path, it.key()), "未知字段"});
            else if (rule.value("additionalProperties").isObject()) evaluate(root, rule.value("additionalProperties").toObject(), it.value(), child(path, it.key()), issues);
        }
    }
    if (value.isArray()) {
        const auto array = value.toArray();
        if (rule.contains("minItems") && array.size() < rule.value("minItems").toInt()) error("项目数量不足");
        if (rule.contains("maxItems") && array.size() > rule.value("maxItems").toInt()) error("项目数量过多");
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (rule.contains("items")) evaluate(root, rule.value("items").toObject(), array[i], child(path, QString::number(i)), issues);
            if (rule.value("uniqueItems").toBool()) for (qsizetype j = 0; j < i; ++j)
                if (array[i] == array[j]) { error("数组中存在重复值"); break; }
        }
    }
    if (value.isString()) {
        const auto text = value.toString();
        const auto length = text.toUcs4().size();
        if (rule.contains("minLength") && length < rule.value("minLength").toInt()) error("字符串过短");
        if (rule.contains("maxLength") && length > rule.value("maxLength").toInt()) error("字符串过长");
        if (rule.contains("pattern") && !QRegularExpression(rule.value("pattern").toString()).match(text).hasMatch()) error("字符串格式不符");
        if (rule.value("format") == "uri") {
            const QUrl url(text, QUrl::StrictMode);
            if (!url.isValid() || url.isRelative()) error("必须是有效绝对 URI");
        }
        if (text.contains(QChar::Null)) error("不允许 NUL 字符");
    }
    if (value.isDouble()) {
        if (rule.contains("minimum") && value.toDouble() < rule.value("minimum").toDouble()) error("低于最小值");
        if (rule.contains("maximum") && value.toDouble() > rule.value("maximum").toDouble()) error("超过最大值");
    }
}
}

QString ProjectPaths::dataDirectory(const QString &id) const { return storageDirectory + "\\data\\" + id; }
QString ProjectPaths::logDirectory(const QString &id) const { return storageDirectory + "\\logs\\" + id; }

QString normalizeWindowsPathInput(QString path) {
    path = path.trimmed();
    if (path.size() >= 2) {
        const auto quote = path.front();
        if ((quote == '"' || quote == '\'') && path.back() == quote)
            path = path.mid(1, path.size() - 2).trimmed();
    }
    if (path.isEmpty() || path.contains(QChar::Null)) return path;
    const bool unc = path.startsWith("\\\\") || path.startsWith("//");
    path.replace('\\', '/');
    const bool driveRoot = path.size() >= 3 && path[1] == ':' && path[2] == '/';
    if (unc) {
        auto remainder = path.mid(2);
        remainder = QDir::cleanPath('/' + remainder);
        if (!remainder.startsWith('/')) remainder.prepend('/');
        path = "//" + remainder.mid(1);
    } else {
        path = QDir::cleanPath(path);
        if (driveRoot && path.size() == 2 && path[1] == ':') path += '/';
    }
    path.replace('/', '\\');
    return path;
}

QJsonObject normalizeProjectPaths(QJsonObject document) {
    auto projectValue = document.value("project");
    if (!projectValue.isObject()) return document;
    auto project = projectValue.toObject();
    auto normalizeArray = [](const QJsonArray &array) {
        QJsonArray result;
        for (const auto &value : array) result.append(value.isString() ? QJsonValue(normalizeWindowsPathInput(value.toString())) : value);
        return result;
    };
    if (project.contains("source") && project.value("source").isObject()) {
        auto source = project.value("source").toObject();
        for (const auto &key : {"workingDirectory", "gitExecutable"})
            if (source.value(key).isString()) source[key] = normalizeWindowsPathInput(source.value(key).toString());
        project["source"] = source;
    }
    if (project.contains("toolDirectories")) project["toolDirectories"] = normalizeArray(project.value("toolDirectories").toArray());
    auto tasks = project.value("tasks").toArray();
    for (qsizetype i = 0; i < tasks.size(); ++i) {
        if (!tasks[i].isObject()) continue;
        auto task = tasks[i].toObject();
        QString taskDirectory;
        if (task.value("workingDirectory").isString()) taskDirectory = normalizeWindowsPathInput(task.value("workingDirectory").toString());
        if (task.contains("environment") && task.value("environment").isObject()) {
            auto environment = task.value("environment").toObject();
            if (environment.contains("envFiles")) environment["envFiles"] = normalizeArray(environment.value("envFiles").toArray());
            task["environment"] = environment;
        }
        auto normalizeCommands = [taskDirectory](const QJsonArray &commands) {
            QJsonArray normalized;
            for (const auto &value : commands) {
                if (!value.isObject()) { normalized.append(value); continue; }
                auto command = value.toObject();
                if (command.value("mode").toString() == "exec" && command.value("program").isString())
                    command["program"] = normalizeWindowsPathInput(command.value("program").toString());
                const auto commandDirectory = command.value("workingDirectory").toString().trimmed();
                if (!commandDirectory.isEmpty()) command["workingDirectory"] = normalizeWindowsPathInput(commandDirectory);
                else if (!taskDirectory.isEmpty()) command["workingDirectory"] = taskDirectory;
                normalized.append(command);
            }
            return normalized;
        };
        if (task.contains("prepareCommands")) task["prepareCommands"] = normalizeCommands(task.value("prepareCommands").toArray());
        if (task.contains("serviceCommand") && task.value("serviceCommand").isObject()) {
            auto command = task.value("serviceCommand").toObject();
            if (command.value("mode").toString() == "exec" && command.value("program").isString())
                command["program"] = normalizeWindowsPathInput(command.value("program").toString());
            const auto commandDirectory = command.value("workingDirectory").toString().trimmed();
            if (!commandDirectory.isEmpty()) command["workingDirectory"] = normalizeWindowsPathInput(commandDirectory);
            else if (!taskDirectory.isEmpty()) command["workingDirectory"] = taskDirectory;
            task["serviceCommand"] = command;
        }
        task.remove("workingDirectory");
        tasks[i] = task;
    }
    project["tasks"] = tasks;
    document["project"] = project;
    return document;
}

bool isWindowsAbsolutePath(const QString &path) {
    if (path.contains(QChar::Null) || path.contains(QRegularExpression("[<>\"|?*\\x00-\\x1f]"))) return false;
    QString p = path;
    p.replace('/', '\\');
    if (p.startsWith("\\\\?\\") || p.startsWith("\\\\.\\")) return false;
    static const QRegularExpression drive("^[A-Za-z]:\\\\");
    static const QRegularExpression unc("^\\\\\\\\[^\\\\:]+\\\\[^\\\\:]+(?:\\\\|$)");
    if (!drive.match(p).hasMatch() && !unc.match(p).hasMatch()) return false;
    if (p.mid(drive.match(p).hasMatch() ? 2 : 0).contains(':')) return false;
    for (const auto &component : p.mid(drive.match(p).hasMatch() ? 3 : 2).split('\\', Qt::SkipEmptyParts)) {
        if (component == "." || component == "..") continue;
        if (component.endsWith('.') || component.endsWith(' ')) return false;
        static const QRegularExpression reserved("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)", QRegularExpression::CaseInsensitiveOption);
        if (reserved.match(component).hasMatch()) return false;
    }
    return true;
}

QString normalizedWindowsPath(QString path) {
    path.replace('\\', '/');
    return QDir::cleanPath(path).toCaseFolded();
}

bool isWithinWindowsPath(const QString &path, const QString &directory) {
    if (directory.isEmpty()) return false;
    auto base = normalizedWindowsPath(directory);
    while (base.endsWith('/')) base.chop(1);
    const auto candidate = normalizedWindowsPath(path);
    return candidate == base || candidate.startsWith(base + '/');
}

bool isAllowedUrl(const QString &text, bool repository) {
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.isRelative() || url.host().isEmpty() || text.contains(QChar::Null)) return false;
    if (repository) return url.scheme() == "https" && !url.authority().contains('@') && !url.hasQuery() && !url.hasFragment();
    return url.scheme() == "http" || url.scheme() == "https";
}

QString expandPlaceholders(const QString &text, const QMap<QString, QString> &values) {
    static const QRegularExpression placeholder("\\{\\{([^{}]*)\\}\\}");
    QString output;
    qsizetype cursor = 0;
    auto matches = placeholder.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const auto key = match.captured(1);
        if (!QStringList{"PROJECT_DIR", "DATA_DIR", "LOG_DIR"}.contains(key) || !values.contains(key))
            throw std::invalid_argument("未知路径占位符");
        const auto replacement = values.value(key);
        if (replacement.contains("{{") || replacement.contains("}}")) throw std::invalid_argument("不允许递归占位符");
        output += text.mid(cursor, match.capturedStart() - cursor) + replacement;
        cursor = match.capturedEnd();
    }
    output += text.mid(cursor);
    if (output.contains("{{") || output.contains("}}")) throw std::invalid_argument("路径占位符不完整");
    return output;
}

QString quoteWindowsArgument(const QString &argument) {
    QString result = "\"";
    qsizetype slashes = 0;
    for (const auto c : argument) {
        if (c == '\\') { ++slashes; continue; }
        if (c == '"') result += QString(slashes * 2 + 1, '\\');
        else result += QString(slashes, '\\');
        result += c;
        slashes = 0;
    }
    return result + QString(slashes * 2, '\\') + '"';
}

QString windowsCommandLine(const QString &program, const QStringList &arguments) {
    QStringList parts{quoteWindowsArgument(program)};
    for (const auto &arg : arguments) parts.append(quoteWindowsArgument(arg));
    return parts.join(' ');
}

bool addressesConflict(const QString &left, const QString &right) {
    const QHostAddress a(left), b(right);
    if (a.isNull() || b.isNull()) return false;
    return a == b || a == QHostAddress::AnyIPv4 || a == QHostAddress::AnyIPv6 ||
           b == QHostAddress::AnyIPv4 || b == QHostAddress::AnyIPv6;
}

ConfigurationValidator::ConfigurationValidator(QJsonObject schema, ProjectPaths paths)
    : schema_(std::move(schema)), paths_(std::move(paths)) {}

ValidationIssues ConfigurationValidator::validate(const QJsonObject &document) const {
    ValidationIssues issues;
    evaluate(schema_, schema_, document, {}, issues);
    if (!issues.isEmpty()) return issues;
    const auto project = document.value("project").toObject();
    const auto source = project.value("source").toObject();
    const auto projectId = project.value("id").toString();
    const auto directory = source.value("workingDirectory").toString().trimmed();
    const QMap<QString, QString> variables{{"PROJECT_DIR", directory}, {"DATA_DIR", paths_.dataDirectory(projectId)}, {"LOG_DIR", paths_.logDirectory(projectId)}};
    const auto fail = [&](const QString &path, const QString &message) { issues.append({"/project/" + path, message}); };
    const auto nonEmpty = [](const QString &text) { return !text.trimmed().isEmpty(); };
    const auto validId = [](const QString &id) {
        static const QRegularExpression pattern("^[a-z][a-z0-9-]{1,62}[a-z0-9]$");
        return pattern.match(id).hasMatch();
    };
    const auto expanded = [&](const QString &text, const QString &path) {
        try { return expandPlaceholders(text, variables); }
        catch (const std::exception &e) { fail(path, QString::fromUtf8(e.what())); return QString{}; }
    };
    const auto absolute = [&](const QString &text, const QString &path, bool allowExpansion) {
        if (text.trimmed().isEmpty()) return QString{};
        const auto result = allowExpansion ? expanded(text, path) : text;
        if (!isWindowsAbsolutePath(result) || (!allowExpansion && (result.contains("{{") || result.contains("}}"))))
            fail(path, "必须是 Windows 绝对路径");
        return result;
    };

    const auto repository = source.value("repositoryUrl").toString().trimmed();
    if (nonEmpty(repository) && !isAllowedUrl(repository, true))
        fail("source/repositoryUrl", "仓库必须为无凭据、查询参数和片段的 HTTPS URL");
    if (nonEmpty(directory)) {
        absolute(directory, "source/workingDirectory", false);
        for (const auto &reserved : {paths_.installDirectory, paths_.storageDirectory, paths_.windowsDirectory})
            if (isWithinWindowsPath(directory, reserved)) fail("source/workingDirectory", "源码目录不得位于安装、数据或系统目录内");
    }
    const auto gitExecutable = source.value("gitExecutable").toString().trimmed();
    if (nonEmpty(gitExecutable)) absolute(gitExecutable, "source/gitExecutable", false);
    const auto credentialTarget = source.value("credentialTarget").toString().trimmed();
    if (nonEmpty(credentialTarget)) {
        const auto expected = "CST/git/" + projectId + '/' + (nonEmpty(repository) ? QUrl(repository).host() : QString{});
        if (credentialTarget != expected) fail("source/credentialTarget", "凭据名称必须为 " + expected);
    }
    const auto branch = source.value("branch").toString().trimmed();
    if (nonEmpty(branch)) {
        const auto branchInvalid = branch.startsWith('-') || branch.startsWith('/') || branch.endsWith('/') || branch.endsWith('.') ||
            branch.contains("..") || branch.contains("@{") || branch.contains("//") || branch == "@" ||
            branch.contains(QRegularExpression("[\\x00-\\x20~^:?*\\[\\\\\\x7f]"));
        if (branchInvalid) fail("source/branch", "Git 分支名称无效");
        for (const auto &part : branch.split('/')) if (part.startsWith('.') || part.endsWith(".lock")) fail("source/branch", "Git 分支名称无效");
    }

    const auto tools = project.value("toolDirectories").toArray();
    for (qsizetype i = 0; i < tools.size(); ++i) {
        const auto tool = tools[i].toString().trimmed();
        if (nonEmpty(tool)) absolute(tool, "toolDirectories/" + QString::number(i), true);
    }

    QSet<QString> taskIds, commandIds, actionIds;
    QSet<int> taskOrders, actionOrders;
    const auto unique = [&](auto &set, const auto &value, const QString &path) {
        if (set.contains(value)) fail(path, "值必须唯一");
        set.insert(value);
    };
    const auto tasks = project.value("tasks").toArray();
    const auto ports = project.value("requiredPorts").toArray();
    for (qsizetype i = 0; i < tasks.size(); ++i) {
        const auto task = tasks[i].toObject();
        const auto path = "tasks/" + QString::number(i) + '/';
        const auto id = task.value("id").toString().trimmed();
        if (nonEmpty(id)) {
            if (!validId(id)) fail(path + "id", "任务标识格式无效");
            unique(taskIds, id, path + "id");
        }
        if (task.contains("order")) unique(taskOrders, task.value("order").toInt(), path + "order");
        const auto env = task.value("environment").toObject();
        for (const auto &file : env.value("envFiles").toArray()) {
            if (!nonEmpty(file.toString())) continue;
            const auto envPath = absolute(file.toString(), path + "environment/envFiles", true);
            if (nonEmpty(envPath) && !isWithinWindowsPath(envPath, paths_.dataDirectory(projectId)))
                fail(path + "environment/envFiles", "环境文件必须位于项目数据目录");
        }
        const auto envVariables = env.value("variables").toObject();
        for (auto it = envVariables.begin(); it != envVariables.end(); ++it)
            if (nonEmpty(it.value().toString())) expanded(it.value().toString(), path + "environment/variables/" + it.key());
        auto commands = task.value("prepareCommands").toArray();
        const auto prepareCount = commands.size();
        commands.append(task.value("serviceCommand"));
        for (qsizetype j = 0; j < commands.size(); ++j) {
            const auto command = commands[j].toObject();
            if (command.isEmpty()) continue;
            const bool service = j == prepareCount;
            const auto cp = path + (service ? QStringLiteral("serviceCommand/") : "prepareCommands/" + QString::number(j) + '/');
            const auto commandId = command.value("id").toString().trimmed();
            if (nonEmpty(commandId)) {
                if (!validId(commandId)) fail(cp + "id", "命令标识格式无效");
                unique(commandIds, commandId, cp + "id");
            }
            const auto commandDirectory = command.value("workingDirectory").toString().trimmed();
            if (nonEmpty(commandDirectory)) absolute(commandDirectory, cp + "workingDirectory", true);
            const auto mode = command.value("mode").toString();
            if (command.contains("timeoutMs")) {
                const auto timeout = command.value("timeoutMs").toInt();
                if (service && timeout != 0) fail(cp + "timeoutMs", "服务超时必须为 0");
                if (!service && timeout <= 0) fail(cp + "timeoutMs", "准备命令超时必须大于 0");
            }
            if (mode == "exec") {
                if (command.contains("script") && nonEmpty(command.value("script").toString())) fail(cp + "script", "exec 模式不能配置脚本");
                const auto program = command.value("program").toString().trimmed();
                if (nonEmpty(program)) {
                    const auto expandedProgram = expanded(program, cp + "program");
                    if (expandedProgram.endsWith(".cmd", Qt::CaseInsensitive) || expandedProgram.endsWith(".bat", Qt::CaseInsensitive))
                        fail(cp + "program", "批处理必须使用 shell 模式");
                    if ((expandedProgram.contains('/') || expandedProgram.contains('\\') || expandedProgram.contains(':')) && !isWindowsAbsolutePath(expandedProgram))
                        fail(cp + "program", "带目录的程序路径必须是绝对路径");
                }
                for (const auto &arg : command.value("arguments").toArray())
                    if (nonEmpty(arg.toString())) expanded(arg.toString(), cp + "arguments");
            } else if (mode == "shell") {
                if (command.contains("program") && nonEmpty(command.value("program").toString())) fail(cp + "program", "shell 模式不能配置程序");
                if (!command.value("arguments").toArray().isEmpty()) fail(cp + "arguments", "shell 模式不能配置参数");
                const auto script = command.value("script").toString();
                if (nonEmpty(script)) expanded(script, cp + "script");
            }
        }
        const auto probes = task.value("readiness").toObject().value("probes").toArray();
        for (const auto &probeValue : probes) {
            const auto probe = probeValue.toObject();
            const auto type = probe.value("type").toString();
            if (type == "http") {
                const auto url = probe.value("url").toString().trimmed();
                if (nonEmpty(url)) {
                    const auto expandedUrl = expanded(url, path + "readiness/probes/url");
                    if (nonEmpty(expandedUrl) && !isAllowedUrl(expandedUrl)) fail(path + "readiness/probes", "HTTP 探针 URL 无效");
                }
            } else if (type == "tcp") {
                const auto address = probe.value("address").toString().trimmed();
                const auto port = probe.value("port").toInt();
                if (nonEmpty(address) && port > 0) {
                    bool found = false;
                    for (const auto &portValue : ports) {
                        const auto required = portValue.toObject();
                        if (required.value("protocol") == "tcp" && required.value("port") == port &&
                            required.value("ownerTaskId") == id && addressesConflict(required.value("address").toString(), address)) found = true;
                    }
                    if (!found) fail(path + "readiness/probes", "TCP 探针必须匹配归属当前任务的 requiredPorts");
                }
            }
        }
    }

    QSet<QString> portKeys;
    for (qsizetype i = 0; i < ports.size(); ++i) {
        const auto port = ports[i].toObject();
        const auto path = "requiredPorts/" + QString::number(i);
        const auto addressText = port.value("address").toString().trimmed();
        const auto owner = port.value("ownerTaskId").toString().trimmed();
        const auto protocol = port.value("protocol").toString();
        if (nonEmpty(owner) && !validId(owner)) fail(path + "/ownerTaskId", "所属任务标识格式无效");
        const auto portNumber = port.value("port").toInt();
        if (nonEmpty(addressText) && QHostAddress(addressText).isNull()) fail(path + "/address", "端口地址必须为 IPv4 或 IPv6 地址");
        if (nonEmpty(owner) && !taskIds.contains(owner)) fail(path + "/ownerTaskId", "任务不存在");
        if (nonEmpty(protocol) && nonEmpty(addressText) && portNumber > 0)
            unique(portKeys, protocol + '/' + QHostAddress(addressText).toString() + '/' + QString::number(portNumber), path);
    }

    const auto actions = project.value("userActions").toArray();
    for (qsizetype i = 0; i < actions.size(); ++i) {
        const auto action = actions[i].toObject();
        const auto path = "userActions/" + QString::number(i) + '/';
        const auto id = action.value("id").toString().trimmed();
        if (nonEmpty(id)) {
            if (!validId(id)) fail(path + "id", "入口标识格式无效");
            unique(actionIds, id, path + "id");
        }
        if (action.contains("order")) unique(actionOrders, action.value("order").toInt(), path + "order");
        const auto url = action.value("url").toString().trimmed();
        if (nonEmpty(url)) {
            const auto expandedUrl = expanded(url, path + "url");
            if (nonEmpty(expandedUrl) && !isAllowedUrl(expandedUrl)) fail(path + "url", "仅允许绝对 HTTP/HTTPS URL");
        }
    }
    return issues;
}

ValidationIssues ConfigurationValidator::validateForRun(const QJsonObject &document) const {
    auto issues = validate(document);
    const auto project = document.value("project").toObject();
    const auto source = project.value("source").toObject();
    const auto fail = [&](const QString &path, const QString &message) { issues.append({"/project/" + path, message}); };
    const auto nonEmpty = [](const QString &text) { return !text.trimmed().isEmpty(); };
    if (!nonEmpty(project.value("name").toString())) fail("name", "项目名称不能为空");
    const auto tasks = project.value("tasks").toArray();
    if (tasks.isEmpty()) fail("tasks", "至少配置一个任务后才能启动");
    for (qsizetype i = 0; i < tasks.size(); ++i) {
        const auto task = tasks[i].toObject();
        const auto path = "tasks/" + QString::number(i) + '/';
        if (!nonEmpty(task.value("id").toString())) fail(path + "id", "任务标识不能为空");
        if (!nonEmpty(task.value("name").toString())) fail(path + "name", "任务名称不能为空");
        const auto service = task.value("serviceCommand").toObject();
        const auto servicePath = path + "serviceCommand/";
        if (service.isEmpty()) {
            fail(path + "serviceCommand", "必须配置长期服务命令");
        } else {
            if (!nonEmpty(service.value("workingDirectory").toString())) fail(servicePath + "workingDirectory", "服务命令工作目录不能为空");
            const auto mode = service.value("mode").toString();
            if (mode == "exec") {
                if (!nonEmpty(service.value("program").toString())) fail(servicePath + "program", "必须配置程序");
                if (!service.contains("timeoutMs") || service.value("timeoutMs").toInt() != 0) fail(servicePath + "timeoutMs", "服务超时必须为 0");
                if (service.value("successExitCodes").toArray().isEmpty()) fail(servicePath + "successExitCodes", "至少配置一个成功退出码");
            } else if (mode == "shell") {
                if (!nonEmpty(service.value("script").toString())) fail(servicePath + "script", "必须配置脚本");
                if (!service.contains("timeoutMs") || service.value("timeoutMs").toInt() != 0) fail(servicePath + "timeoutMs", "服务超时必须为 0");
                if (service.value("successExitCodes").toArray().isEmpty()) fail(servicePath + "successExitCodes", "至少配置一个成功退出码");
            } else {
                fail(servicePath + "mode", "必须选择 exec 或 shell");
            }
        }
        const auto prepares = task.value("prepareCommands").toArray();
        for (qsizetype j = 0; j < prepares.size(); ++j) {
            const auto command = prepares[j].toObject();
            const auto cp = path + "prepareCommands/" + QString::number(j) + '/';
            if (!nonEmpty(command.value("workingDirectory").toString())) fail(cp + "workingDirectory", "准备命令工作目录不能为空");
            const auto mode = command.value("mode").toString();
            const auto timeout = command.value("timeoutMs").toInt();
            if (mode == "exec") {
                if (!nonEmpty(command.value("program").toString())) fail(cp + "program", "必须配置程序");
                if (!command.contains("timeoutMs") || timeout <= 0) fail(cp + "timeoutMs", "准备命令超时必须大于 0");
                if (command.value("successExitCodes").toArray().isEmpty()) fail(cp + "successExitCodes", "至少配置一个成功退出码");
            } else if (mode == "shell") {
                if (!nonEmpty(command.value("script").toString())) fail(cp + "script", "必须配置脚本");
                if (!command.contains("timeoutMs") || timeout <= 0) fail(cp + "timeoutMs", "准备命令超时必须大于 0");
                if (command.value("successExitCodes").toArray().isEmpty()) fail(cp + "successExitCodes", "至少配置一个成功退出码");
            } else {
                fail(cp + "mode", "必须选择 exec 或 shell");
            }
        }
        const auto readiness = task.value("readiness").toObject();
        if (readiness.isEmpty()) {
            fail(path + "readiness", "必须配置就绪检查");
        } else {
            const auto probes = readiness.value("probes").toArray();
            if (probes.isEmpty()) fail(path + "readiness/probes", "至少配置一个就绪探针");
            for (qsizetype j = 0; j < probes.size(); ++j) {
                const auto probe = probes[j].toObject();
                const auto pp = path + "readiness/probes/" + QString::number(j) + '/';
                const auto type = probe.value("type").toString();
                if (type == "tcp") {
                    if (!nonEmpty(probe.value("address").toString())) fail(pp + "address", "探针地址不能为空");
                    if (probe.value("port").toInt() <= 0) fail(pp + "port", "探针端口必须大于 0");
                    if (probe.value("connectTimeoutMs").toInt() <= 0) fail(pp + "connectTimeoutMs", "连接超时必须大于 0");
                } else if (type == "http") {
                    if (!nonEmpty(probe.value("url").toString())) fail(pp + "url", "探针 URL 不能为空");
                    if (probe.value("expectedStatusCodes").toArray().isEmpty()) fail(pp + "expectedStatusCodes", "至少配置一个预期状态码");
                    if (probe.value("requestTimeoutMs").toInt() <= 0) fail(pp + "requestTimeoutMs", "请求超时必须大于 0");
                } else {
                    fail(pp + "type", "探针类型必须为 tcp 或 http");
                }
            }
        }
        const auto restart = task.value("restartPolicy").toObject();
        if (restart.isEmpty()) fail(path + "restartPolicy", "必须配置重启策略");
        else {
            const auto rp = path + "restartPolicy/";
            if (!restart.contains("mode") || restart.value("mode").toString() != "on_failure") fail(rp + "mode", "重启策略模式必须为 on_failure");
            if (restart.value("maxRestarts").toInt() <= 0) fail(rp + "maxRestarts", "最大重启次数必须大于 0");
            if (restart.value("windowSeconds").toInt() <= 0) fail(rp + "windowSeconds", "统计窗口必须大于 0");
            if (restart.value("backoffSeconds").toInt() <= 0) fail(rp + "backoffSeconds", "初始退避必须大于 0");
            if (restart.value("maxBackoffSeconds").toInt() <= 0) fail(rp + "maxBackoffSeconds", "最大退避必须大于 0");
        }
    }
    return issues;
}

ValidationIssues ConfigurationValidator::validateForSync(const QJsonObject &document) const {
    auto issues = validate(document);
    const auto source = document.value("project").toObject().value("source").toObject();
    const auto fail = [&](const QString &path, const QString &message) { issues.append({"/project/" + path, message}); };
    if (source.value("repositoryUrl").toString().trimmed().isEmpty()) fail("source/repositoryUrl", "同步前必须配置 HTTPS 仓库地址");
    if (source.value("branch").toString().trimmed().isEmpty()) fail("source/branch", "同步前必须配置分支");
    if (source.value("workingDirectory").toString().trimmed().isEmpty()) fail("source/workingDirectory", "同步前必须配置源码目录");
    if (source.value("gitExecutable").toString().trimmed().isEmpty()) fail("source/gitExecutable", "同步前必须配置 Git 可执行文件");
    return issues;
}

}
