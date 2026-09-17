#pragma once

#include "domain/Configuration.h"
#include <QJsonObject>
#include <QMutex>
#include <stdexcept>

namespace cst {
class ConfigurationError final : public std::runtime_error {
public:
    explicit ConfigurationError(ValidationIssues issues);
    const ValidationIssues issues;
};

void saveJson(const QString &path, const QJsonObject &object);
QJsonObject readJson(const QString &path);

class ProjectConfigService {
public:
    explicit ProjectConfigService(ConfigurationValidator validator);
    QJsonObject load(const QString &path) const;
    void save(const QString &path, const QJsonObject &document) const;
    ValidationIssues validate(const QJsonObject &document) const;
    ValidationIssues validateForRun(const QJsonObject &document) const;
    ValidationIssues validateForSync(const QJsonObject &document) const;
    QJsonObject create(const QString &name) const;
private:
    ConfigurationValidator validator_;
};

struct CatalogEntry {
    QString id;
    QString name;
    QString configurationPath;
};

// Callers must hold the runtime operation queue while invoking mutations.
// A process-wide mutex and a disk lock also serialize independent service instances.
class ProjectCatalogService {
public:
    ProjectCatalogService(QString storageDirectory, const ProjectConfigService &configuration);
    QList<CatalogEntry> entries() const;
    QString selectedId() const;
    QString defaultId() const;
    QString importProject(const QJsonObject &document);
    void exportProject(const QString &id, const QString &destination) const;
    void removeProject(const QString &id);
    void selectProject(const QString &id);
    void setDefault(const QString &id);
    QJsonObject project(const QString &id) const;
    void saveProject(const QString &id, const QJsonObject &document);
private:
    QJsonObject loadCatalog() const;
    void requireEntry(const QJsonObject &catalog, const QString &id) const;
    QString configurationPath(const QString &id) const;
    QString storage_;
    const ProjectConfigService &configuration_;
};
}
