#pragma once
#include "application/DiagnosticExportService.h"
#include "application/ProjectRuntimeService.h"
#include "ui/widgets/SchemaEditor.h"
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QLineEdit>
#include <QComboBox>

namespace cst {
class AdminPage final : public QWidget {
    Q_OBJECT
public:
    AdminPage(ProjectRuntimeService &runtime, ProjectConfigService &configuration, ProjectCatalogService &catalog,
              ICredentialStore &credentials, IPortManager &ports, SourceSyncService &sync,
              DiagnosticExportService &diagnostics, LogService &logs, QJsonObject schema, ProjectPaths paths, QWidget *parent = nullptr);
    void setProject(const QJsonObject &document);
    void updateState();
    void appendLog(const QString &task, const QString &line);
    void showConfigurationProblem(const QString &message);
    void showPage(int index);
signals:
    void error(QString message);
    void editingChanged(bool dirty);
private:
    void rebuild();
    void dirty();
    void setField(const QString &field, const QJsonValue &value);
    SchemaEditor *fieldEditor(const QString &field, QWidget *parent);
    QWidget *page(int index);
    void buildTasks(QWidget *page);
    void renderLogs();
    SyncRequest request() const;
    void save();
    void editEnv();
    void createProject();
    void importProject();
    void refreshCredentialStatus();
    ProjectRuntimeService &runtime_;
    ProjectConfigService &configuration_;
    ProjectCatalogService &catalog_;
    ICredentialStore &credentials_;
    IPortManager &ports_;
    SourceSyncService &sync_;
    DiagnosticExportService &diagnostics_;
    LogService &logsService_;
    QJsonObject schema_;
    ProjectPaths paths_;
    QJsonObject draft_;
    bool newProject_ = false;
    bool modified_ = false;
    QListWidget *navigation_;
    QLabel *breadcrumb_;
    QLabel *pageHelp_;
    QStackedWidget *pages_;
    QLabel *saveStatus_;
    QPushButton *save_;
    QPushButton *discard_;
    QPlainTextEdit *logView_ = nullptr;
    QLineEdit *search_ = nullptr;
    QComboBox *taskFilter_ = nullptr;
    QList<QPair<QString,QString>> logLines_;
    QList<QWidget *> editControls_;
    QLabel *issues_ = nullptr;
    QLabel *credentialStatus_ = nullptr;
    QPushButton *testAuthButton_ = nullptr;
    QPushButton *syncButton_ = nullptr;
};
}
