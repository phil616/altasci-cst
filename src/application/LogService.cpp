#include "LogService.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace cst {
LogService::LogService(QString storage, QObject *parent) : QObject(parent), storage_(std::move(storage)), writer_(new QObject) {
    writer_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, writer_, &QObject::deleteLater);
    thread_.start();
}
LogService::~LogService() { flush(); thread_.quit(); thread_.wait(); }
void LogService::flush() { if (thread_.isRunning()) QMetaObject::invokeMethod(writer_, [] {}, Qt::BlockingQueuedConnection); }
void LogService::addSecret(const QString &secret) {
    QMetaObject::invokeMethod(writer_, [this, secret] { if (!secret.isEmpty() && !secrets_.contains(secret)) secrets_.append(secret); }, Qt::QueuedConnection);
}
void LogService::append(const QString &path, const QByteArray &line, qint64 limit) {
    const auto fail = [](const QString &message) { throw std::runtime_error(message.toUtf8().constData()); };
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) fail("无法创建日志目录：" + path);
    if (QFileInfo(path).size() + line.size() > limit) {
        if (QFileInfo::exists(path + ".4") && !QFile::remove(path + ".4")) fail("不能删除轮转日志：" + path);
        for (int i = 3; i >= 0; --i) {
            const auto from = i == 0 ? path : path + '.' + QString::number(i);
            const auto to = path + '.' + QString::number(i + 1);
            if (QFileInfo::exists(from) && !QFile::rename(from, to)) fail("不能轮转日志：" + from);
        }
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append) || file.write(line) != line.size() || !file.flush()) fail("日志写入失败：" + path + ": " + file.errorString());
}
void LogService::write(QString projectId, QString taskId, QString operationId, QString event, QString message,
                       QString level, QString channel, bool decodeError, QDateTime timestamp, QString attemptId) {
    const auto size = message.size() * 2 + 1024;
    if (queuedBytes_.fetch_add(size) + size > 8 * 1024 * 1024) {
        queuedBytes_.fetch_sub(size);
        if (!overflow_.exchange(true)) emit failed("日志写入落后，已丢弃输出；请检查磁盘性能和空间");
        return;
    }
    QMetaObject::invokeMethod(writer_, [this, projectId = std::move(projectId), taskId = std::move(taskId), operationId = std::move(operationId),
        event = std::move(event), message = std::move(message), level = std::move(level), channel = std::move(channel), decodeError, timestamp, attemptId = std::move(attemptId), size] {
        queuedBytes_.fetch_sub(size); overflow_.store(false);
        try {
            static const QRegularExpression safe("^[a-zA-Z0-9-]+$");
            if (!safe.match(projectId).hasMatch() || (!taskId.isEmpty() && !safe.match(taskId).hasMatch())) throw std::invalid_argument("日志项目或任务标识无效");
            QJsonObject entry{{"ts", timestamp.toUTC().toString(Qt::ISODateWithMs)}, {"level", level}, {"projectId", projectId},
                {"taskId", taskId}, {"operationId", operationId}, {"event", event}, {"message", redactSecrets(message, secrets_)}};
            entry["runId"] = operationId;
            if (!attemptId.isEmpty()) entry["attemptId"] = attemptId;
            if (!channel.isEmpty()) entry["channel"] = channel;
            if (decodeError) entry["decodeError"] = true;
            const auto bytes = QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
            const auto directory = storage_ + "/logs/" + projectId;
            append(directory + "/cst.log", bytes, 10 * 1024 * 1024);
            if (!taskId.isEmpty()) append(directory + "/tasks/" + taskId + ".log", bytes, 20 * 1024 * 1024);
            emit lineWritten(taskId, QString::fromUtf8(bytes).trimmed());
        } catch (const std::exception &error) { emit failed(QString::fromUtf8(error.what())); }
    }, Qt::QueuedConnection);
}
QJsonValue LogService::redactJson(const QJsonValue &value) {
    if (value.isString()) return redactSecrets(value.toString());
    if (value.isArray()) { QJsonArray result; for (const auto &item : value.toArray()) result.append(redactJson(item)); return result; }
    if (value.isObject()) {
        auto result = value.toObject();
        static const QRegularExpression sensitive("token|password|authorization|credential.?blob", QRegularExpression::CaseInsensitiveOption);
        for (auto it = result.begin(); it != result.end(); ++it) it.value() = sensitive.match(it.key()).hasMatch() ? QJsonValue("[REDACTED]") : redactJson(it.value());
        return result;
    }
    return value;
}
}
