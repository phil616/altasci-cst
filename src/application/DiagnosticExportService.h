#pragma once
#include "Platform.h"
#include "LogService.h"

namespace cst {
class DiagnosticExportService {
public:
    DiagnosticExportService(IProcessRunner &runner, IPortManager &ports, IClock &clock, LogService &logs, QString storageDirectory);
    void exportZip(const QJsonObject &configuration, const QJsonArray &tasks, const QString &outputPath,
                   const QString &operationId, const Cancellation &cancel);
private:
    QString run(const QString &program, const QStringList &arguments, const QString &workingDirectory, const Cancellation &cancel);
    IProcessRunner &runner_;
    IPortManager &ports_;
    IClock &clock_;
    LogService &logs_;
    QString storage_;
};
}
