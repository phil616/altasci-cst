#include "ProjectRuntimeService.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QUuid>

namespace cst {
namespace {
QString exceptionText(std::exception_ptr error) {
    try { if (error) std::rethrow_exception(error); }
    catch (const std::exception &e) { return QString::fromUtf8(e.what()); }
    catch (...) { return "未知操作错误"; }
    return {};
}
bool cancelled(std::exception_ptr error) {
    try { if (error) std::rethrow_exception(error); }
    catch (const Cancelled &) { return true; } catch (...) { return false; }
    return false;
}
}
ProjectRuntimeService::ProjectRuntimeService(ProjectConfigService &configuration, ProjectCatalogService &catalog,
    TaskSupervisor &supervisor, PortReclaimService &ports, SourceSyncService &sync, QObject *parent)
    : QObject(parent), configuration_(configuration), catalog_(catalog), supervisor_(supervisor), ports_(ports), sync_(sync),
      queue_(this), cancel_(std::make_shared<Cancellation>()) {
    qRegisterMetaType<ProjectState>(); qRegisterMetaType<TaskStatus>(); qRegisterMetaType<ProcessOutput>();
    connect(&queue_, &OperationQueue::busyChanged, this, [this] { emit availabilityChanged(); });
    supervisor_.taskChanged = [this](const TaskStatus &task) { emit taskChanged(task); };
    supervisor_.output = [this](const QString &id, ProcessOutput output) { emit processOutput(id, std::move(output)); };
    connect(this, &ProjectRuntimeService::taskChanged, this, [this](const TaskStatus &task) { taskStatuses_[task.id] = task; }, Qt::QueuedConnection);
    monitor_.setInterval(100);
    connect(&monitor_, &QTimer::timeout, this, [this] {
        if (state_ != ProjectState::Running || queue_.busy()) return;
        const auto cancel = cancel_;
        queue_.enqueue({}, [this, cancel] { supervisor_.tick(*cancel); }, [this](std::exception_ptr error) {
            if (error && !cancelled(error) && !cancel_->requested.load()) cleanupAfterFailure(error);
        });
    });
    monitor_.start();
}
ProjectRuntimeService::~ProjectRuntimeService() {
    cancel_->requested.store(true); queue_.join();
    supervisor_.taskChanged = {}; supervisor_.output = {};
    try { supervisor_.stop(); } catch (...) { /* Job owners still enforce kill-on-close during destruction. */ }
}
ProjectState ProjectRuntimeService::state() const { return state_; }
QJsonObject ProjectRuntimeService::currentProject() const { return current_; }
bool ProjectRuntimeService::editable() const { return (state_ == ProjectState::Stopped || state_ == ProjectState::Failed) && jobsEmpty_ && !closing_; }
bool ProjectRuntimeService::editing() const { return editing_; }
bool ProjectRuntimeService::busy() const { return queue_.busy(); }
QString ProjectRuntimeService::operationId() const { return operationId_; }
QJsonArray ProjectRuntimeService::taskSnapshot() const {
    QJsonArray result;
    for (const auto &task : taskStatuses_) result.append(QJsonObject{{"id", task.id}, {"name", task.name}, {"state", task.state}, {"restartCount", qint64(task.restartCount)}});
    return result;
}
void ProjectRuntimeService::initialize(const QString &storageDirectory) {
    if (queue_.busy()) return;
    storageDirectory_ = storageDirectory;
    auto document = std::make_shared<QJsonObject>(); newOperation();
    queue_.enqueue({}, [this, storageDirectory, document] {
        Cancellation cancel;
        QDirIterator journals(storageDirectory + "/state", {"sync-journal.json"}, QDir::Files, QDirIterator::Subdirectories);
        while (journals.hasNext()) {
            const auto path = journals.next(); const auto id = QFileInfo(path).dir().dirName();
            const auto raw = readJson(storageDirectory + "/projects/" + id + "/project.json");
            const auto source = raw.value("project").toObject().value("source").toObject();
            sync_.recover({id, source.value("repositoryUrl").toString(), source.value("branch").toString(), source.value("workingDirectory").toString(),
                source.value("gitExecutable").toString(), source.value("credentialTarget").toString(), {}}, cancel);
        }
        sync_.retryCleanup();
        auto id = catalog_.defaultId(); if (id.isEmpty()) id = catalog_.selectedId();
        if (!id.isEmpty()) *document = catalog_.project(id);
    }, [this, document](std::exception_ptr error) {
        if (!error) { current_ = *document; emit projectChanged(current_); }
        finish(error, "配置已加载");
    });
}
void ProjectRuntimeService::setEditing(bool editing) { editing_ = editing; }
QString ProjectRuntimeService::newOperation() { operationId_ = QUuid::createUuid().toString(QUuid::WithoutBraces); return operationId_; }
void ProjectRuntimeService::event(RuntimeEvent value, bool failure) {
    const auto next = transition(state_, value, failure);
    if (!next) throw std::logic_error(("非法状态转换：" + stateName(state_) + " event=" + QString::number(int(value))).toStdString());
    state_ = *next; emit stateChanged(state_);
}
void ProjectRuntimeService::finish(std::exception_ptr error, const QString &success) {
    emit operationFinished(error ? exceptionText(error) : success, !error);
    if (closing_ && jobsEmpty_ && (state_ == ProjectState::Stopped || state_ == ProjectState::Failed)) emit closeReady();
}
void ProjectRuntimeService::start() {
    if (!editable() || queue_.busy() || editing_ || current_.isEmpty()) { emit operationFinished("请先保存有效配置并停止当前操作", false); return; }
    const auto project = current_.value("project").toObject();
    const auto document = current_;
    cancel_ = std::make_shared<Cancellation>(); const auto cancel = cancel_; const auto operationId = newOperation();
    auto failure = std::make_shared<std::exception_ptr>();
    auto empty = std::make_shared<bool>(true);
    queue_.enqueue([this] { event(RuntimeEvent::Start); }, [this, project, document, cancel, operationId, failure, empty] {
        enum class Stage { Preflight, Ports, Tasks }; auto stage = Stage::Preflight;
        try {
            const auto issues = configuration_.validateForRun(document); if (!issues.isEmpty()) throw ConfigurationError(issues);
            cancel->check();
            if (storageDirectory_.isEmpty() || !QDir().mkpath(storageDirectory_ + "/locks")) throw std::runtime_error("项目数据目录未初始化");
            projectLock_ = std::make_unique<QLockFile>(storageDirectory_ + "/locks/" + project.value("id").toString() + ".lock");
            projectLock_->setStaleLockTime(0);
            if (!projectLock_->tryLock(0)) throw std::runtime_error("另一个 CST 操作正在使用该项目");
            const auto target = project.value("source").toObject().value("workingDirectory").toString();
            if (!target.trimmed().isEmpty() && !QFileInfo(target).isDir()) throw std::runtime_error("源码目录不存在；请先同步代码");
            supervisor_.preflight(project, *cancel);
            queue_.post([this] { event(RuntimeEvent::ChecksPassed); }); stage = Stage::Ports;
            const auto settings = project.value("settings").toObject();
            const auto portReclaimTimeoutMs = settings.contains("portReclaimTimeoutMs") ? settings.value("portReclaimTimeoutMs").toInt() : 15000;
            const auto maxAncestorEscalation = settings.contains("maxAncestorEscalation") ? settings.value("maxAncestorEscalation").toInt() : 8;
            ports_.reclaim(PortReclaimService::requirements(project.value("requiredPorts").toArray()),
                portReclaimTimeoutMs, maxAncestorEscalation, {}, *cancel);
            cancel->check(); queue_.post([this] { event(RuntimeEvent::PortsFree); }); stage = Stage::Tasks;
            supervisor_.start(project, operationId, *cancel);
            cancel->check(); *empty = supervisor_.empty();
        } catch (...) {
            *failure = std::current_exception();
            const bool stopped = cancelled(*failure);
            queue_.post([this, stage, stopped] {
                if (stopped) event(RuntimeEvent::Stop);
                else if (stage == Stage::Preflight) event(RuntimeEvent::CheckFailed);
                else if (stage == Stage::Ports) event(RuntimeEvent::ReclaimFailed);
                else event(RuntimeEvent::TaskFailed);
            });
            try { supervisor_.stop(); } catch (...) { *failure = std::current_exception(); }
            *empty = supervisor_.empty();
            if (*empty) projectLock_.reset();
        }
    }, [this, failure, empty](std::exception_ptr error) {
        jobsEmpty_ = *empty;
        if (error) *failure = error;
        if (*failure) {
            if (state_ == ProjectState::Stopping && jobsEmpty_) event(RuntimeEvent::JobsEmpty, !cancelled(*failure));
            if (cancelled(*failure)) finish({}, "启动已取消"); else finish(*failure, {});
        } else { event(RuntimeEvent::TasksReady); finish({}, "项目已运行"); }
    });
}
void ProjectRuntimeService::stop() {
    cancel_->requested.store(true);
    if (state_ == ProjectState::Preflight || state_ == ProjectState::ReclaimingPorts || state_ == ProjectState::Starting || state_ == ProjectState::Stopping || state_ == ProjectState::Syncing) return;
    if (state_ != ProjectState::Running) { if (closing_ && !queue_.busy() && jobsEmpty_) emit closeReady(); return; }
    if (stopQueued_) return;
    stopQueued_ = true;
    newOperation();
    auto empty = std::make_shared<bool>(false);
    queue_.enqueue([this] { event(RuntimeEvent::Stop); }, [this, empty] {
        try { supervisor_.stop(); *empty = supervisor_.empty(); if (*empty) projectLock_.reset(); }
        catch (...) { *empty = supervisor_.empty(); if (*empty) projectLock_.reset(); throw; }
    }, [this, empty](std::exception_ptr error) {
        stopQueued_ = false;
        jobsEmpty_ = *empty;
        if (jobsEmpty_) event(RuntimeEvent::JobsEmpty, bool(error));
        finish(error, "项目已停止");
    });
}
void ProjectRuntimeService::cleanupAfterFailure(std::exception_ptr original) {
    auto empty = std::make_shared<bool>(false);
    queue_.enqueue([this] { event(RuntimeEvent::UnrecoverableCrash); }, [this, empty] {
        try { supervisor_.stop(); *empty = supervisor_.empty(); if (*empty) projectLock_.reset(); }
        catch (...) { *empty = supervisor_.empty(); if (*empty) projectLock_.reset(); throw; }
    }, [this, original, empty](std::exception_ptr error) {
        jobsEmpty_ = *empty; if (jobsEmpty_) event(RuntimeEvent::JobsEmpty, true);
        finish(error ? error : original, {});
    });
}
SyncRequest ProjectRuntimeService::syncRequest() const {
    const auto project = current_.value("project").toObject(); const auto source = project.value("source").toObject();
    return {project.value("id").toString(), source.value("repositoryUrl").toString(), source.value("branch").toString(),
        source.value("workingDirectory").toString(), source.value("gitExecutable").toString(), source.value("credentialTarget").toString(), operationId_};
}
void ProjectRuntimeService::synchronize() {
    if (!editable() || queue_.busy() || editing_ || current_.isEmpty()) { emit operationFinished("同步要求项目已停止且无未保存编辑", false); return; }
    const auto issues = configuration_.validateForSync(current_);
    if (!issues.isEmpty()) { emit operationFinished(QString::fromUtf8(ConfigurationError(issues).what()), false); return; }
    newOperation(); const auto request = syncRequest(); cancel_ = std::make_shared<Cancellation>(); const auto cancel = cancel_;
    auto head = std::make_shared<QString>();
    queue_.enqueue([this] { if (!editable() || editing_) throw std::runtime_error("当前状态禁止同步"); event(RuntimeEvent::Sync); },
        [this, request, cancel, head] { *head = sync_.synchronize(request, *cancel, [this] { return supervisor_.empty(); }); },
        [this, head](std::exception_ptr error) {
            event(error ? RuntimeEvent::SyncFailed : RuntimeEvent::SyncSucceeded);
            if (!error) emit headChanged(*head);
            finish(error, "代码同步完成");
        });
}
void ProjectRuntimeService::close() { closing_ = true; stop(); }
void ProjectRuntimeService::edit(std::function<void()> work, bool reload) {
    if (!editable() || queue_.busy()) { emit operationFinished("当前状态禁止修改项目", false); return; }
    newOperation(); auto document = std::make_shared<QJsonObject>();
    queue_.enqueue({}, [this, work = std::move(work), reload, document] {
        if (!supervisor_.empty()) throw std::runtime_error("仍有托管进程，禁止修改项目");
        work();
        if (reload) { const auto id = catalog_.selectedId(); if (!id.isEmpty()) *document = catalog_.project(id); }
    }, [this, reload, document](std::exception_ptr error) {
        if (!error && reload) { current_ = *document; editing_ = false; emit projectChanged(current_); }
        finish(error, "配置操作完成");
    });
}
void ProjectRuntimeService::switchProject(const QString &id) {
    if (editing_) { emit operationFinished("请先保存或放弃编辑", false); return; }
    auto document = std::make_shared<QJsonObject>();
    if (!editable() || queue_.busy()) { emit operationFinished("当前状态禁止切换项目", false); return; }
    newOperation();
    queue_.enqueue({}, [this, id, document] { catalog_.selectProject(id); *document = catalog_.project(id); },
        [this, document](std::exception_ptr error) { if (!error) { current_ = *document; emit projectChanged(current_); } finish(error, "项目已切换"); });
}
void ProjectRuntimeService::saveConfig(const QJsonObject &document) {
    if (!editable() || queue_.busy()) { emit operationFinished("当前状态禁止保存配置", false); return; }
    newOperation();
    queue_.enqueue({}, [this, document] {
        if (!supervisor_.empty()) throw std::runtime_error("仍有托管进程，禁止保存配置");
        catalog_.saveProject(document.value("project").toObject().value("id").toString(), document);
    }, [this, document](std::exception_ptr error) {
        if (!error) { current_ = document; editing_ = false; emit projectChanged(current_); }
        finish(error, "配置已保存");
    });
}
void ProjectRuntimeService::importProject(const QJsonObject &document) { edit([this, document] { catalog_.importProject(document); }, true); }
void ProjectRuntimeService::removeProject(const QString &id) { edit([this, id] { catalog_.removeProject(id); }, true); }
void ProjectRuntimeService::setDefault(const QString &id) { edit([this, id] { catalog_.setDefault(id); }, false); }
void ProjectRuntimeService::exportProject(const QString &destination) {
    const auto id = current_.value("project").toObject().value("id").toString();
    edit([this, id, destination] { catalog_.exportProject(id, destination); }, false);
}
void ProjectRuntimeService::maintenance(std::function<void(const Cancellation &)> work) {
    if (!editable() || queue_.busy()) { emit operationFinished("当前状态禁止维护操作", false); return; }
    cancel_ = std::make_shared<Cancellation>(); const auto cancel = cancel_; newOperation();
    queue_.enqueue({}, [work = std::move(work), cancel] { work(*cancel); }, [this](std::exception_ptr error) { finish(error, "维护操作完成"); });
}
}
