#include "DiagnosticExportService.h"
#include "PortReclaimService.h"
#include "ProjectConfigService.h"
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QSaveFile>
#include <QSysInfo>
#include <QTemporaryDir>

namespace cst {
DiagnosticExportService::DiagnosticExportService(IProcessRunner &runner, IPortManager &ports, IClock &clock, LogService &logs, QString storage)
    : runner_(runner), ports_(ports), clock_(clock), logs_(logs), storage_(std::move(storage)) {}
QString DiagnosticExportService::run(const QString &program, const QStringList &arguments, const QString &workingDirectory, const Cancellation &cancel) {
    struct Capture { QMutex mutex; QString text; };
    auto capture = std::make_shared<Capture>();
    auto process = runner_.start({program, arguments, workingDirectory, runner_.inheritedEnvironment(), {}, {}, {}, {}},
        [capture](ProcessOutput output) { QMutexLocker lock(&capture->mutex); capture->text += output.text + '\n'; });
    const auto deadline = clock_.monotonicMs() + 120000;
    try {
        while (!process->empty()) {
            cancel.check();
            if (clock_.monotonicMs() >= deadline) throw std::runtime_error("诊断工具执行超时");
            clock_.sleep(25, cancel);
        }
        process->forceStop();
        const auto result = process->result();
        if (!result || result->crashed || result->exitCode != 0) throw std::runtime_error("诊断工具失败");
    } catch (...) { process->forceStop(); throw; }
    QMutexLocker lock(&capture->mutex); return redactSecrets(capture->text).trimmed();
}
void DiagnosticExportService::exportZip(const QJsonObject &configuration, const QJsonArray &tasks, const QString &outputPath,
                                       const QString &operationId, const Cancellation &cancel) {
    if (!QFileInfo(outputPath).isAbsolute() || !outputPath.endsWith(".zip", Qt::CaseInsensitive)) throw std::invalid_argument("诊断包必须为绝对路径的 .zip 文件");
    const auto project = configuration.value("project").toObject(); const auto id = project.value("id").toString();
    if (QUuid(id).isNull()) throw std::invalid_argument("无效项目 ID");
    QTemporaryDir temporary; if (!temporary.isValid()) throw std::runtime_error("无法创建诊断临时目录");
    logs_.flush();
    saveJson(temporary.filePath("project.json"), LogService::redactJson(configuration).toObject());
    for (const auto &relative : {QStringLiteral("catalog.json"), "state/" + id + "/runtime.json"}) {
        const auto path = storage_ + '/' + relative;
        if (QFileInfo::exists(path)) saveJson(temporary.filePath(QFileInfo(path).fileName()), LogService::redactJson(readJson(path)).toObject());
    }
    QJsonArray portSnapshot;
    for (const auto &port : PortReclaimService::requirements(project.value("requiredPorts").toArray())) {
        cancel.check();
        for (const auto &owner : ports_.owners(port)) portSnapshot.append(QJsonObject{{"protocol", owner.protocol}, {"address", owner.address},
            {"port", owner.port}, {"pid", qint64(owner.pid)}, {"parentPid", qint64(owner.parentPid)}, {"imagePath", owner.imagePath}});
    }
    const auto gitVersion = run(project.value("source").toObject().value("gitExecutable").toString(), {"--version"}, temporary.path(), cancel);
    saveJson(temporary.filePath("diagnostics.json"), {{"cstVersion", "1.0.0"}, {"windowsVersion", QSysInfo::prettyProductName()},
        {"kernelVersion", QSysInfo::kernelVersion()}, {"gitVersion", gitVersion}, {"ports", portSnapshot}, {"tasks", tasks}, {"operationId", operationId}});
    const auto logDirectory = storage_ + "/logs/" + id;
    QDirIterator iterator(logDirectory, {"*.log", "*.log.[1-4]"}, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        cancel.check(); const auto source = iterator.next();
        const auto destination = temporary.filePath("logs/" + QDir(logDirectory).relativeFilePath(source));
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())) throw std::runtime_error("无法创建诊断日志目录");
        QFile input(source); QSaveFile output(destination);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) throw std::runtime_error("无法复制诊断日志");
        while (!input.atEnd()) {
            cancel.check(); const auto line = input.readLine(); QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line, &error);
            const auto cleaned = error.error == QJsonParseError::NoError && document.isObject()
                ? QJsonDocument(LogService::redactJson(document.object()).toObject()).toJson(QJsonDocument::Compact)
                : QJsonDocument(QJsonObject{{"message", redactSecrets(QString::fromUtf8(line))}}).toJson(QJsonDocument::Compact);
            const auto bytes = cleaned + '\n'; if (output.write(bytes) != bytes.size()) throw std::runtime_error("诊断日志写入失败");
        }
        if (!output.commit()) throw std::runtime_error("诊断日志提交失败");
    }
    const auto tar = runner_.inheritedEnvironment().value("SYSTEMROOT") + "\\System32\\tar.exe";
    const auto stagedZip = temporary.path() + ".zip";
    try {
        run(tar, {"-a", "-c", "-f", stagedZip, "-C", temporary.path(), "."}, temporary.path(), cancel);
        QFile input(stagedZip); QSaveFile destination(outputPath);
        if (!input.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) throw std::runtime_error("无法保存诊断压缩包");
        while (!input.atEnd()) {
            cancel.check(); const auto bytes = input.read(1024 * 1024);
            if (destination.write(bytes) != bytes.size()) throw std::runtime_error("诊断压缩包写入失败");
        }
        if (!destination.commit()) throw std::runtime_error("诊断包提交失败");
        input.close(); QFile::remove(stagedZip);
    } catch (...) { QFile::remove(stagedZip); throw; }
}
}
