#include "SourceSyncService.h"
#include "ProjectConfigService.h"
#include "domain/Configuration.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QLockFile>
#include <QRegularExpression>
#include <QUuid>

namespace cst {
namespace {
[[noreturn]] void error(const QString &message) { throw std::runtime_error(message.toUtf8().constData()); }
class SyncLock {
public:
    SyncLock(const QString &storage, const QString &id) : lock_(storage + "/locks/" + id + ".lock") {
        if (QUuid(id).isNull() || id.contains('/') || id.contains('\\')) error("同步项目 ID 无效");
        if (!QDir().mkpath(storage + "/locks")) error("无法创建项目锁目录");
        lock_.setStaleLockTime(0);
        if (!lock_.tryLock(0)) error("项目正在被另一个操作使用");
    }
private:
    QLockFile lock_;
};
bool ownedSibling(const QString &target, const QString &candidate, const QString &kind) {
    const QFileInfo destination(target), sibling(candidate);
    const auto prefix = '.' + destination.fileName() + ".cst-" + kind + '-';
    return destination.absolutePath() == sibling.absolutePath() && sibling.fileName().startsWith(prefix) &&
           !QUuid(sibling.fileName().mid(prefix.size())).isNull();
}
}

SourceSyncService::SourceSyncService(IProcessRunner &runner, IFileTransaction &files, IClock &clock,
                                     QString storage, QString askpass, QMutex &operationMutex)
    : runner_(runner), files_(files), clock_(clock), storage_(std::move(storage)), askpass_(std::move(askpass)), operationMutex_(operationMutex) {}
QString SourceSyncService::journalPath(const QString &id) const { return storage_ + "/state/" + id + "/sync-journal.json"; }

QString SourceSyncService::git(const SyncRequest &request, const QStringList &arguments, const QString &directory,
                              const Cancellation &cancel) {
    cancel.check();
    auto env = mergeEnvironment(true, runner_.inheritedEnvironment(), {}, {}, {
        {"GIT_TERMINAL_PROMPT", "0"}, {"GIT_ASKPASS", askpass_}, {"GIT_ASKPASS_REQUIRE", "force"},
        {"CST_CREDENTIAL_TARGET", request.credentialTarget}, {"LC_ALL", "C"},
        // Avoid global credential helpers storing or requesting credentials outside CST.
        {"GIT_CONFIG_COUNT", "1"}, {"GIT_CONFIG_KEY_0", "credential.helper"}, {"GIT_CONFIG_VALUE_0", ""}
    });
    // Ambient repository selection must never redirect the fixed deployment algorithm.
    for (const auto &key : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES"}) env.remove(key);
    struct Output { QMutex mutex; QString out; QString err; };
    auto output = std::make_shared<Output>();
    auto process = runner_.start({request.gitExecutable, arguments, directory, env, {}, request.projectId, {}, request.operationId},
        [output](ProcessOutput line) {
            QMutexLocker lock(&output->mutex);
            auto &destination = line.channel == "stdout" ? output->out : output->err;
            destination += line.text + '\n';
            if (destination.size() > 4 * 1024 * 1024) destination = destination.right(4 * 1024 * 1024);
        });
    try {
        while (!process->empty()) { cancel.check(); clock_.sleep(25, cancel); }
        const auto result = process->result();
        if (!result || result->crashed || result->exitCode != 0) {
            QMutexLocker lock(&output->mutex);
            error("Git 命令失败：" + redactSecrets(output->err));
        }
    } catch (...) { process->forceStop(); throw; }
    QMutexLocker lock(&output->mutex);
    return output->out.trimmed();
}

QString SourceSyncService::validateCheckout(const SyncRequest &request, const QString &directory, const Cancellation &cancel) {
    if (!git(request, {"status", "--porcelain=v1", "--untracked-files=all"}, directory, cancel).isEmpty()) error("新克隆副本不是干净的工作树");
    const auto head = git(request, {"rev-parse", "HEAD"}, directory, cancel);
    static const QRegularExpression hash("^(?:[0-9a-f]{40}|[0-9a-f]{64})$");
    if (!hash.match(head).hasMatch()) error("Git HEAD 无法解析");
    return head;
}
void SourceSyncService::queueCleanup(const QString &path) {
    const auto queueFile = storage_ + "/state/cleanup-queue.json";
    auto queue = QFileInfo::exists(queueFile) ? readJson(queueFile).value("paths").toArray() : QJsonArray{};
    if (!queue.contains(path)) queue.append(path);
    saveJson(queueFile, {{"paths", queue}});
}
void SourceSyncService::cleanup(const QString &path) {
    if (QFileInfo::exists(path) && !files_.removeDirectory(path)) queueCleanup(path);
}
void SourceSyncService::retryCleanup() {
    QMutexLocker guard(&operationMutex_);
    const auto queueFile = storage_ + "/state/cleanup-queue.json";
    if (!QFileInfo::exists(queueFile)) return;
    QJsonArray remaining;
    for (const auto &value : readJson(queueFile).value("paths").toArray()) {
        const auto path = value.toString();
        static const QRegularExpression safeName("^\\..+\\.cst-(?:staging|backup)-[0-9a-f-]{36}$");
        if (!QFileInfo(path).isAbsolute() || !safeName.match(QFileInfo(path).fileName()).hasMatch()) error("清理队列包含不合法的 CST 临时目录");
        if (QFileInfo::exists(path) && !files_.removeDirectory(path)) remaining.append(path);
    }
    saveJson(queueFile, {{"paths", remaining}});
}
void SourceSyncService::recoverLocked(const SyncRequest &request, const Cancellation &cancel) {
    const auto path = journalPath(request.projectId);
    if (!QFileInfo::exists(path)) return;
    const auto journal = readJson(path);
    const auto target = journal.value("target").toString();
    const auto staging = journal.value("staging").toString();
    const auto backup = journal.value("backup").toString();
    if (QDir::cleanPath(target) != QDir::cleanPath(request.target) || !ownedSibling(target, staging, "staging") || !ownedSibling(target, backup, "backup")) error("同步日志路径与项目配置不符；停止恢复");
    if (QFileInfo::exists(target)) {
        cleanup(staging); cleanup(backup);
    } else if (QFileInfo::exists(backup)) {
        files_.renameDirectory(backup, target); cleanup(staging);
    } else if (QFileInfo::exists(staging)) {
        validateCheckout(request, staging, cancel); files_.renameDirectory(staging, target);
    } else error("同步恢复失败；检查 target=" + target + " staging=" + staging + " backup=" + backup);
    if (!QFile::remove(path)) error("无法清除已恢复的同步日志：" + path);
}
void SourceSyncService::recover(const SyncRequest &request, const Cancellation &cancel) {
    QMutexLocker guard(&operationMutex_); SyncLock lock(storage_, request.projectId);
    recoverLocked(request, cancel);
}
void SourceSyncService::testAuthentication(const SyncRequest &request, const Cancellation &cancel) {
    git(request, {"ls-remote", "--exit-code", "--heads", request.repositoryUrl, request.branch}, QFileInfo(request.target).absolutePath(), cancel);
}

QString SourceSyncService::synchronize(const SyncRequest &request, const Cancellation &cancel,
                                       const std::function<bool()> &canSync) {
    QMutexLocker guard(&operationMutex_); SyncLock lock(storage_, request.projectId);
    if (!canSync()) error("项目必须停止、无残留进程且没有未保存编辑，才可同步");
    recoverLocked(request, cancel);
    const auto parent = QFileInfo(request.target).absolutePath();
    if (!QDir().mkpath(parent)) error("无法创建项目父目录");
    const auto version = git(request, {"--version"}, parent, cancel);
    const auto match = QRegularExpression("^git version (\\d+)\\.(\\d+)\\.(\\d+)").match(version);
    if (!match.hasMatch() || match.captured(1).toInt() < 2 || (match.captured(1).toInt() == 2 && match.captured(2).toInt() < 45)) error("Git 版本必须不低于 2.45.0");
    const auto suffix = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto prefix = parent + "/." + QFileInfo(request.target).fileName() + ".cst-";
    const auto staging = prefix + "staging-" + suffix;
    const auto backup = prefix + "backup-" + suffix;
    QJsonObject journal{{"target", request.target}, {"staging", staging}, {"backup", backup}, {"phase", "validated"}};
    bool journalWritten = false;
    bool oldMoved = false;
    bool installed = false;
    try {
        git(request, {"clone", "--branch", request.branch, "--single-branch", "--recurse-submodules", request.repositoryUrl, staging}, parent, cancel);
        git(request, {"submodule", "sync", "--recursive"}, staging, cancel);
        git(request, {"submodule", "update", "--init", "--recursive", "--force"}, staging, cancel);
        const auto head = validateCheckout(request, staging, cancel);
        cancel.check();
        if (!canSync()) error("项目状态已改变，取消同步");
        saveJson(journalPath(request.projectId), journal); journalWritten = true;
        if (QFileInfo::exists(request.target)) { files_.renameDirectory(request.target, backup); oldMoved = true; }
        journal["phase"] = "old-moved"; saveJson(journalPath(request.projectId), journal);
        cancel.check();
        files_.renameDirectory(staging, request.target); installed = true;
        journal["phase"] = "installed"; saveJson(journalPath(request.projectId), journal);
        cleanup(backup);
        saveJson(storage_ + "/state/" + request.projectId + "/sync-result.json", {{"head", head}, {"operationId", request.operationId}});
        if (!QFile::remove(journalPath(request.projectId))) error("同步成功，但无法清除事务日志");
        return head;
    } catch (...) {
        const auto original = std::current_exception();
        if (oldMoved && !installed) {
            try { files_.renameDirectory(backup, request.target); }
            catch (...) { error("目录恢复失败，请人工检查：target=" + request.target + " staging=" + staging + " backup=" + backup); }
        }
        if (!installed) {
            cleanup(staging);
            if (journalWritten && !QFile::remove(journalPath(request.projectId))) error("无法清除失败同步事务日志");
        }
        std::rethrow_exception(original);
    }
}
}
