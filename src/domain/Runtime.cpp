#include "Runtime.h"

#include <QRegularExpression>
#include <QStringDecoder>
#include <algorithm>
#include <stdexcept>

namespace cst {
QString stateName(ProjectState state) {
    switch (state) {
    case ProjectState::Stopped: return "Stopped";
    case ProjectState::Preflight: return "Preflight";
    case ProjectState::ReclaimingPorts: return "ReclaimingPorts";
    case ProjectState::Starting: return "Starting";
    case ProjectState::Running: return "Running";
    case ProjectState::Stopping: return "Stopping";
    case ProjectState::Failed: return "Failed";
    case ProjectState::Syncing: return "Syncing";
    }
    throw std::logic_error("Unknown project state");
}

std::optional<ProjectState> transition(ProjectState state, RuntimeEvent event, bool preserveFailure) {
    using S = ProjectState;
    using E = RuntimeEvent;
    switch (state) {
    case S::Stopped:
    case S::Failed:
        if (event == E::Start) return S::Preflight;
        if (event == E::Sync) return S::Syncing;
        break;
    case S::Preflight:
        if (event == E::ChecksPassed) return S::ReclaimingPorts;
        if (event == E::CheckFailed) return S::Failed;
        if (event == E::Stop || event == E::Close) return S::Stopping;
        break;
    case S::ReclaimingPorts:
        if (event == E::PortsFree) return S::Starting;
        if (event == E::ReclaimFailed) return S::Failed;
        if (event == E::Stop || event == E::Close) return S::Stopping;
        break;
    case S::Starting:
        if (event == E::TasksReady) return S::Running;
        if (event == E::TaskFailed || event == E::Stop || event == E::Close) return S::Stopping;
        break;
    case S::Running:
        if (event == E::UnrecoverableCrash || event == E::Stop || event == E::Close) return S::Stopping;
        break;
    case S::Stopping:
        if (event == E::JobsEmpty) return preserveFailure ? S::Failed : S::Stopped;
        break;
    case S::Syncing:
        if (event == E::SyncSucceeded) return S::Stopped;
        if (event == E::SyncFailed) return S::Failed;
        // Close cancels the operation; completion is emitted after rollback/cleanup.
        break;
    }
    return std::nullopt;
}

RestartBudget::RestartBudget(RestartPolicy policy) : policy_(policy) {
    if (policy.maxRestarts < 1 || policy.windowMs < 1 || policy.backoffSeconds < 1 || policy.maxBackoffSeconds < 1)
        throw std::invalid_argument("Invalid restart policy");
}

std::optional<qint64> RestartBudget::schedule(qint64 monotonicMs) {
    while (!restarts_.isEmpty() && restarts_.front() <= monotonicMs - policy_.windowMs) restarts_.removeFirst();
    if (restarts_.size() >= policy_.maxRestarts) return std::nullopt;
    // The specified sequence deliberately uses 15 rather than 16 seconds.
    static constexpr int multiplier[] = {1, 2, 4, 8, 15, 30};
    const auto index = std::min<qsizetype>(consecutive_, 5);
    const qint64 delay = std::min<qint64>(qint64(policy_.backoffSeconds) * multiplier[index], policy_.maxBackoffSeconds) * 1000;
    restarts_.append(monotonicMs);
    ++consecutive_;
    return delay;
}
void RestartBudget::reset() { restarts_.clear(); consecutive_ = 0; }
qsizetype RestartBudget::attempts() const { return consecutive_; }

Environment parseEnv(const QByteArray &bytes) {
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    QString text = decoder(bytes);
    if (decoder.hasError()) throw std::invalid_argument("环境文件必须是有效 UTF-8");
    if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
    if (text.contains(QChar::Null)) throw std::invalid_argument("环境文件包含 NUL");
    Environment result;
    static const QRegularExpression keyPattern("^[A-Za-z_][A-Za-z0-9_]*$");
    const auto lines = text.split('\n');
    for (auto line : lines) {
        if (line.endsWith('\r')) line.chop(1);
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const auto equal = line.indexOf('=');
        if (equal < 1) throw std::invalid_argument("环境文件应使用 KEY=VALUE");
        const auto key = line.left(equal);
        if (!keyPattern.match(key).hasMatch()) throw std::invalid_argument("环境变量名称无效，不支持 export");
        auto value = line.mid(equal + 1);
        if (value.startsWith('\'') || value.startsWith('"')) {
            if (value.size() < 2 || value.back() != value.front()) throw std::invalid_argument("环境变量引号不成对");
            value = value.mid(1, value.size() - 2);
        } else if (value.endsWith('\'') || value.endsWith('"')) throw std::invalid_argument("环境变量引号不成对");
        result.insert(key.toUpper(), value);
    }
    return result;
}

Environment mergeEnvironment(bool inheritSystem, const Environment &system, const QList<Environment> &files,
                             const Environment &variables, const Environment &injected) {
    Environment result;
    const auto merge = [&](const Environment &source) {
        for (auto it = source.begin(); it != source.end(); ++it) {
            if (it.key().isEmpty() || it.key().contains(QChar::Null) || it.value().contains(QChar::Null))
                throw std::invalid_argument("无效环境变量");
            result.insert(it.key().toUpper(), it.value());
        }
    };
    if (inheritSystem) merge(system);
    for (const auto &file : files) merge(file);
    merge(variables);
    merge(injected);
    return result;
}

QString environmentBlock(const Environment &environment) {
    const auto canonical = mergeEnvironment(false, {}, {}, environment, {});
    QString block;
    for (auto it = canonical.begin(); it != canonical.end(); ++it) block += it.key() + '=' + it.value() + QChar::Null;
    if (block.isEmpty()) block += QChar::Null;
    block += QChar::Null;
    return block;
}

QString redactSecrets(QString text, const QStringList &knownSecrets) {
    for (const auto &secret : knownSecrets) if (!secret.isEmpty()) text.replace(secret, "[REDACTED]");
    static const QRegularExpression userinfo("(https?://)[^/\\s@]+@", QRegularExpression::CaseInsensitiveOption);
    text.replace(userinfo, "\\1[REDACTED]@");
    static const QRegularExpression authorization("(Authorization\\s*[:=]\\s*)[^\\r\\n]+", QRegularExpression::CaseInsensitiveOption);
    text.replace(authorization, "\\1[REDACTED]");
    static const QRegularExpression fields(
        "((?:[\\\"']?)(?:[A-Za-z0-9_-]*(?:token|password)|credential[ _-]?blob)(?:[\\\"']?)\\s*[:=]\\s*)(?:\\\"[^\\\"]*\\\"|'[^']*'|[^\\s,;&}]+)",
        QRegularExpression::CaseInsensitiveOption);
    text.replace(fields, "\\1[REDACTED]");
    return text;
}
}
