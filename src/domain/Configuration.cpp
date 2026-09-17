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
        if (task.value("workingDirectory").isString()) task["workingDirectory"] = normalizeWindowsPathInput(task.value("workingDirectory").toString());
        if (task.contains("environment") && task.value("environment").isObject()) {
            auto environment = task.value("environment").toObject();
            if (environment.contains("envFiles")) environment["envFiles"] = normalizeArray(environment.value("envFiles").toArray());
            task["environment"] = environment;
        }
        auto normalizeCommands = [](const QJsonArray &commands) {
            QJsonArray normalized;
            for (const auto &value : commands) {
                if (!value.isObject()) { normalized.append(value); continue; }
                auto command = value.toObject();
                if (command.value("mode").toString() == "exec" && command.value("program").isString())
                    command["program"] = normalizeWindowsPathInput(command.value("program").toString());
                normalized.append(command);
            }
            return normalized;
        };
        if (task.contains("prepareCommands")) task["prepareCommands"] = normalizeCommands(task.value("prepareCommands").toArray());
        if (task.contains("serviceCommand") && task.value("serviceCommand").isObject()) {
            auto command = task.value("serviceCommand").toObject();
            if (command.value("mode").toString() == "exec" && command.value("program").isString())
                command["program"] = normalizeWindowsPathInput(command.value("program").toString());
            task["serviceCommand"] = command;
        }
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
    const auto directory = source.value("workingDirectory").toString();
    const QMap<QString, QString> variables{{"PROJECT_DIR", directory}, {"DATA_DIR", paths_.dataDirectory(projectId)}, {"LOG_DIR", paths_.logDirectory(projectId)}};
    const auto fail = [&](const QString &path, const QString &message) { issues.append({"/project/" + path, message}); };
    const auto expanded = [&](const QString &text, const QString &path) {
        try { return expandPlaceholders(text, variables); }
        catch (const std::exception &e) { fail(path, QString::fromUtf8(e.what())); return QString{}; }
    };
    const auto absolute = [&](const QString &text, const QString &path, bool allowExpansion) {
        const auto result = allowExpansion ? expanded(text, path) : text;
        if (!isWindowsAbsolutePath(result) || (!allowExpansion && (result.contains("{{") || result.contains("}}"))))
            fail(path, "必须是 Windows 绝对路径");
        return result;
    };
    absolute(directory, "source/workingDirectory", false);
    absolute(source.value("gitExecutable").toString(), "source/gitExecutable", false);
    for (const auto &reserved : {paths_.installDirectory, paths_.storageDirectory, paths_.windowsDirectory})
        if (isWithinWindowsPath(directory, reserved)) fail("source/workingDirectory", "源码目录不得位于安装、数据或系统目录内");
    if (!isAllowedUrl(source.value("repositoryUrl").toString(), true)) fail("source/repositoryUrl", "仓库必须为无凭据、查询参数和片段的 HTTPS URL");
    const auto credential = "CST/git/" + projectId + '/' + QUrl(source.value("repositoryUrl").toString()).host();
    if (source.value("credentialTarget").toString() != credential) fail("source/credentialTarget", "凭据名称必须为 " + credential);
    const auto branch = source.value("branch").toString();
    if (branch.startsWith('-') || branch.startsWith('/') || branch.endsWith('/') || branch.endsWith('.') ||
        branch.contains("..") || branch.contains("@{") || branch.contains("//") || branch == "@" ||
        branch.contains(QRegularExpression("[\\x00-\\x20~^:?*\\[\\\\\\x7f]"))) fail("source/branch", "Git 分支名称无效");
    for (const auto &part : branch.split('/')) if (part.startsWith('.') || part.endsWith(".lock")) fail("source/branch", "Git 分支名称无效");
    const auto tools = project.value("toolDirectories").toArray();
    for (qsizetype i = 0; i < tools.size(); ++i) absolute(tools[i].toString(), "toolDirectories/" + QString::number(i), true);
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
        const auto id = task.value("id").toString();
        unique(taskIds, id, path + "id");
        unique(taskOrders, task.value("order").toInt(), path + "order");
        absolute(task.value("workingDirectory").toString(), path + "workingDirectory", true);
        const auto env = task.value("environment").toObject();
        for (const auto &file : env.value("envFiles").toArray()) {
            const auto envPath = absolute(file.toString(), path + "environment/envFiles", true);
            if (!isWithinWindowsPath(envPath, paths_.dataDirectory(projectId))) fail(path + "environment/envFiles", "环境文件必须位于项目数据目录");
        }
        const auto envVariables = env.value("variables").toObject();
        for (auto it = envVariables.begin(); it != envVariables.end(); ++it) expanded(it.value().toString(), path + "environment/variables/" + it.key());
        auto commands = task.value("prepareCommands").toArray();
        const auto prepareCount = commands.size();
        commands.append(task.value("serviceCommand"));
        for (qsizetype j = 0; j < commands.size(); ++j) {
            const auto command = commands[j].toObject();
            const bool service = j == prepareCount;
            const auto cp = path + (service ? QStringLiteral("serviceCommand/") : "prepareCommands/" + QString::number(j) + '/');
            unique(commandIds, command.value("id").toString(), cp + "id");
            if ((service && command.value("timeoutMs").toInt() != 0) || (!service && command.value("timeoutMs").toInt() == 0)) fail(cp + "timeoutMs", "服务超时必须为 0，准备命令超时必须大于 0");
            if (command.value("mode") == "exec") {
                const auto program = expanded(command.value("program").toString(), cp + "program");
                if (program.endsWith(".cmd", Qt::CaseInsensitive) || program.endsWith(".bat", Qt::CaseInsensitive)) fail(cp + "program", "批处理必须使用 shell 模式");
                if ((program.contains('/') || program.contains('\\') || program.contains(':')) && !isWindowsAbsolutePath(program)) fail(cp + "program", "带目录的程序路径必须是绝对路径");
                for (const auto &arg : command.value("arguments").toArray()) expanded(arg.toString(), cp + "arguments");
            } else expanded(command.value("script").toString(), cp + "script");
        }
        for (const auto &probeValue : task.value("readiness").toObject().value("probes").toArray()) {
            const auto probe = probeValue.toObject();
            if (probe.value("type") == "http") {
                if (!isAllowedUrl(expanded(probe.value("url").toString(), path + "readiness/probes/url"))) fail(path + "readiness/probes", "HTTP 探针 URL 无效");
            } else {
                bool found = false;
                for (const auto &portValue : ports) {
                    const auto port = portValue.toObject();
                    if (port.value("protocol") == "tcp" && port.value("port") == probe.value("port") &&
                        port.value("ownerTaskId") == id && addressesConflict(port.value("address").toString(), probe.value("address").toString())) found = true;
                }
                if (!found) fail(path + "readiness/probes", "TCP 探针必须匹配归属当前任务的 requiredPorts");
            }
        }
    }
    QSet<QString> portKeys;
    for (qsizetype i = 0; i < ports.size(); ++i) {
        const auto port = ports[i].toObject();
        const auto path = "requiredPorts/" + QString::number(i);
        const QHostAddress address(port.value("address").toString());
        if (address.isNull()) fail(path + "/address", "端口地址必须为 IPv4 或 IPv6 地址");
        if (!taskIds.contains(port.value("ownerTaskId").toString())) fail(path + "/ownerTaskId", "任务不存在");
        unique(portKeys, port.value("protocol").toString() + '/' + address.toString() + '/' + QString::number(port.value("port").toInt()), path);
    }
    const auto actions = project.value("userActions").toArray();
    for (qsizetype i = 0; i < actions.size(); ++i) {
        const auto action = actions[i].toObject();
        const auto path = "userActions/" + QString::number(i) + '/';
        unique(actionIds, action.value("id").toString(), path + "id");
        unique(actionOrders, action.value("order").toInt(), path + "order");
        if (!isAllowedUrl(expanded(action.value("url").toString(), path + "url"))) fail(path + "url", "仅允许绝对 HTTP/HTTPS URL");
    }
    return issues;
}
}
