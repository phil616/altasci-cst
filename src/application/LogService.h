#pragma once
#include "Platform.h"
#include <QObject>
#include <QThread>

namespace cst {
class LogService final : public QObject {
    Q_OBJECT
public:
    explicit LogService(QString storageDirectory, QObject *parent = nullptr);
    ~LogService() override;
    void write(QString projectId, QString taskId, QString operationId, QString event, QString message,
               QString level = "info", QString channel = {}, bool decodeError = false,
               QDateTime timestamp = QDateTime::currentDateTimeUtc());
    void addSecret(const QString &secret);
    void flush();
    static QJsonValue redactJson(const QJsonValue &value);
signals:
    void lineWritten(QString taskId, QString line);
    void failed(QString diagnostic);
private:
    void append(const QString &path, const QByteArray &line, qint64 limit);
    QString storage_;
    QStringList secrets_;
    QThread thread_;
    QObject *writer_;
};
}
