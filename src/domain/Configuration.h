#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace cst {
struct ValidationIssue {
    QString path;
    QString message;
};
using ValidationIssues = QList<ValidationIssue>;

struct ProjectPaths {
    QString installDirectory;
    QString storageDirectory;
    QString windowsDirectory;
    QString dataDirectory(const QString &id) const;
    QString logDirectory(const QString &id) const;
};

// Evaluates the Draft 2020-12 vocabulary used by the bundled, immutable schema.
// No external references or user-supplied schemas are accepted.
class ConfigurationValidator {
public:
    explicit ConfigurationValidator(QJsonObject schema, ProjectPaths paths);
    ValidationIssues validate(const QJsonObject &document) const;
    ValidationIssues validateForRun(const QJsonObject &document) const;
    ValidationIssues validateForSync(const QJsonObject &document) const;
private:
    QJsonObject schema_;
    ProjectPaths paths_;
};

bool isWindowsAbsolutePath(const QString &path);
QString normalizeWindowsPathInput(QString path);
QJsonObject normalizeProjectPaths(QJsonObject document);
QString normalizedWindowsPath(QString path);
bool isWithinWindowsPath(const QString &path, const QString &directory);
bool isAllowedUrl(const QString &text, bool repository = false);
QString expandPlaceholders(const QString &text, const QMap<QString, QString> &values);
QString quoteWindowsArgument(const QString &argument);
QString windowsCommandLine(const QString &program, const QStringList &arguments);
bool addressesConflict(const QString &left, const QString &right);
}
