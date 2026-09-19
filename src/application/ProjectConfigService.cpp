#include "ProjectConfigService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <stdexcept>

namespace cst {
namespace {
QMutex catalogMutex;
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toUtf8().constData()); }
QString issuesText(const ValidationIssues &issues) {
    QStringList lines;
    for (const auto &issue : issues) lines.append(issue.path + ": " + issue.message);
    return lines.join('\n');
}
void validateId(const QString &id) {
    static const QRegularExpression pattern("^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    if (!pattern.match(id).hasMatch()) fail("项目 ID 无效");
}
class CatalogLock {
public:
    explicit CatalogLock(const QString &storage) : process_(&catalogMutex), file_(storage + "/locks/catalog.lock") {
        if (!QDir().mkpath(storage + "/locks")) fail("不能创建目录锁路径");
        file_.setStaleLockTime(0);
        if (!file_.tryLock(0)) fail("项目目录正在由另一个操作使用");
    }
private:
    QMutexLocker<QMutex> process_;
    QLockFile file_;
};
}

ConfigurationError::ConfigurationError(ValidationIssues errors)
    : std::runtime_error(issuesText(errors).toUtf8().constData()), issues(std::move(errors)) {}

void saveJson(const QString &path, const QJsonObject &object) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) fail("无法创建文件目录：" + path);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) fail("无法写入 " + path + ": " + file.errorString());
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) fail("原子写入失败 " + path + ": " + file.errorString());
}

QJsonObject readJson(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail("无法读取 " + path + ": " + file.errorString());
    if (file.size() > 16 * 1024 * 1024) fail("JSON 文件超过 16 MiB：" + path);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) fail("JSON 无效 " + path + ": " + error.errorString());
    return document.object();
}

ProjectConfigService::ProjectConfigService(ConfigurationValidator validator) : validator_(std::move(validator)) {}
ValidationIssues ProjectConfigService::validate(const QJsonObject &document) const { return validator_.validate(document); }
ValidationIssues ProjectConfigService::validateForRun(const QJsonObject &document) const { return validator_.validateForRun(document); }
ValidationIssues ProjectConfigService::validateForSync(const QJsonObject &document) const { return validator_.validateForSync(document); }
QJsonObject ProjectConfigService::load(const QString &path) const {
    auto document = normalizeProjectPaths(readJson(path));
    if (document.value("schemaVersion").toInt() == 1) document["schemaVersion"] = 2;
    const auto issues = validate(document);
    if (!issues.isEmpty()) throw ConfigurationError(issues);
    return document;
}
void ProjectConfigService::save(const QString &path, const QJsonObject &document) const {
    const auto normalized = normalizeProjectPaths(document);
    const auto issues = validate(normalized);
    if (!issues.isEmpty()) throw ConfigurationError(issues);
    if (QFileInfo::exists(path) && !QFileInfo::exists(path + ".v1.bak") && readJson(path).value("schemaVersion").toInt() == 1 && normalized.value("schemaVersion").toInt() == 2)
        if (!QFile::copy(path, path + ".v1.bak")) fail("不能创建 v1 配置备份：" + path);
    saveJson(path, normalized);
}
QJsonObject ProjectConfigService::create(const QString &name) const {
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return {{"schemaVersion", 2}, {"project", QJsonObject{
        {"id", id}, {"name", name}, {"description", ""},
        {"source", QJsonObject{}},
        {"toolDirectories", QJsonArray{}}, {"requiredPorts", QJsonArray{}}, {"tasks", QJsonArray{}},
        {"userActions", QJsonArray{}}, {"settings", QJsonObject{{"portPolicy", "fail"}, {"portReclaimTimeoutMs", 15000}, {"maxAncestorEscalation", 8}}}
    }}};
}

ProjectCatalogService::ProjectCatalogService(QString storage, const ProjectConfigService &configuration)
    : storage_(std::move(storage)), configuration_(configuration) {}
QString ProjectCatalogService::configurationPath(const QString &id) const {
    validateId(id);
    return storage_ + "/projects/" + id + "/project.json";
}
QJsonObject ProjectCatalogService::loadCatalog() const {
    if (!QFileInfo::exists(storage_ + "/catalog.json")) return {{"projects", QJsonArray{}}, {"defaultProjectId", ""}, {"lastSelectedId", ""}};
    const auto catalog = readJson(storage_ + "/catalog.json");
    if (catalog.size() != 3 || !catalog.value("projects").isArray() || !catalog.value("defaultProjectId").isString() || !catalog.value("lastSelectedId").isString()) fail("catalog.json 格式无效");
    QSet<QString> ids;
    for (const auto &value : catalog.value("projects").toArray()) {
        const auto entry = value.toObject(); const auto id = entry.value("id").toString();
        validateId(id);
        if (entry.size() != 3 || !entry.value("name").isString() || entry.value("configurationPath").toString() != configurationPath(id) || ids.contains(id)) fail("catalog.json 项目信息无效");
        ids.insert(id);
    }
    for (const auto &field : {"defaultProjectId", "lastSelectedId"})
        if (!catalog.value(field).toString().isEmpty() && !ids.contains(catalog.value(field).toString())) fail("catalog.json 引用了不存在的项目");
    return catalog;
}
void ProjectCatalogService::requireEntry(const QJsonObject &catalog, const QString &id) const {
    validateId(id);
    for (const auto &value : catalog.value("projects").toArray()) if (value.toObject().value("id") == id) return;
    fail("项目不存在：" + id);
}
QList<CatalogEntry> ProjectCatalogService::entries() const {
    CatalogLock lock(storage_); QList<CatalogEntry> result;
    for (const auto &value : loadCatalog().value("projects").toArray()) {
        const auto entry = value.toObject();
        result.append({entry.value("id").toString(), entry.value("name").toString(), entry.value("configurationPath").toString()});
    }
    return result;
}
QString ProjectCatalogService::selectedId() const {
    CatalogLock lock(storage_); const auto catalog = loadCatalog();
    const auto selected = catalog.value("lastSelectedId").toString();
    return selected.isEmpty() ? catalog.value("defaultProjectId").toString() : selected;
}
QString ProjectCatalogService::defaultId() const { CatalogLock lock(storage_); return loadCatalog().value("defaultProjectId").toString(); }
QString ProjectCatalogService::importProject(const QJsonObject &document) {
    const auto normalized = normalizeProjectPaths(document);
    const auto issues = configuration_.validate(normalized); if (!issues.isEmpty()) throw ConfigurationError(issues);
    CatalogLock lock(storage_); auto catalog = loadCatalog(); auto entries = catalog.value("projects").toArray();
    const auto project = normalized.value("project").toObject(); const auto id = project.value("id").toString();
    for (const auto &entry : entries) if (entry.toObject().value("id") == id) fail("同 ID 项目已存在；请使用保存配置更新");
    configuration_.save(configurationPath(id), normalized);
    entries.append(QJsonObject{{"id", id}, {"name", project.value("name")}, {"configurationPath", configurationPath(id)}});
    catalog["projects"] = entries;
    if (catalog.value("defaultProjectId").toString().isEmpty()) catalog["defaultProjectId"] = id;
    catalog["lastSelectedId"] = id;
    saveJson(storage_ + "/catalog.json", catalog);
    return id;
}
QJsonObject ProjectCatalogService::project(const QString &id) const {
    CatalogLock lock(storage_); requireEntry(loadCatalog(), id); return configuration_.load(configurationPath(id));
}
void ProjectCatalogService::exportProject(const QString &id, const QString &destination) const { configuration_.save(destination, project(id)); }
void ProjectCatalogService::removeProject(const QString &id) {
    CatalogLock lock(storage_); auto catalog = loadCatalog(); requireEntry(catalog, id);
    auto entries = catalog.value("projects").toArray();
    for (qsizetype i = entries.size(); i > 0; --i) if (entries[i - 1].toObject().value("id") == id) entries.removeAt(i - 1);
    catalog["projects"] = entries;
    const auto fallback = entries.isEmpty() ? QString{} : entries[0].toObject().value("id").toString();
    for (const auto &field : {"defaultProjectId", "lastSelectedId"}) if (catalog.value(field) == id) catalog[field] = fallback;
    saveJson(storage_ + "/catalog.json", catalog);
    // Source, environment, logs and credentials are deliberately not deleted by removing a catalog entry.
    if (QFileInfo::exists(configurationPath(id)) && !QFile::remove(configurationPath(id))) fail("项目已移出目录，但无法删除配置文件：" + configurationPath(id));
}
void ProjectCatalogService::selectProject(const QString &id) {
    CatalogLock lock(storage_); auto catalog = loadCatalog(); requireEntry(catalog, id);
    catalog["lastSelectedId"] = id; saveJson(storage_ + "/catalog.json", catalog);
}
void ProjectCatalogService::setDefault(const QString &id) {
    CatalogLock lock(storage_); auto catalog = loadCatalog(); requireEntry(catalog, id);
    catalog["defaultProjectId"] = id; saveJson(storage_ + "/catalog.json", catalog);
}
void ProjectCatalogService::saveProject(const QString &id, const QJsonObject &document) {
    CatalogLock lock(storage_); auto catalog = loadCatalog(); requireEntry(catalog, id);
    if (document.value("project").toObject().value("id") != id) fail("编辑时不能改变项目 ID");
    configuration_.save(configurationPath(id), document);
    auto entries = catalog.value("projects").toArray();
    for (qsizetype i = 0; i < entries.size(); ++i) {
        auto entry = entries[i].toObject(); if (entry.value("id") != id) continue;
        entry["name"] = document.value("project").toObject().value("name"); entries[i] = entry;
    }
    catalog["projects"] = entries; saveJson(storage_ + "/catalog.json", catalog);
}
}
