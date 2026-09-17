#pragma once
#include "domain/Runtime.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QVBoxLayout>
namespace cst {
inline QLabel *helpText(const QString &text, QWidget *parent=nullptr) {
    auto *label=new QLabel(text,parent); label->setWordWrap(true);label->setProperty("role","help");label->setTextFormat(Qt::PlainText);return label;
}
inline void section(QVBoxLayout *layout,const QString &title,const QString &description={}) {
    auto *label=helpText(title,layout->parentWidget());label->setProperty("role","section");layout->addWidget(label);
    if(!description.isEmpty())layout->addWidget(helpText(description,layout->parentWidget()));
}
inline QString formatLogLine(const QString &line) {
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(line.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return line;
    const auto entry = document.object();
    auto stamp = entry.value("ts").toString();
    const auto parsed = QDateTime::fromString(stamp, Qt::ISODateWithMs);
    if (parsed.isValid()) stamp = parsed.toLocalTime().toString("HH:mm:ss");
    const auto level = entry.value("level").toString();
    const auto channel = entry.value("channel").toString();
    const auto event = entry.value("event").toString();
    const auto message = entry.value("message").toString();
    QString prefix = stamp;
    if (!level.isEmpty() && level.compare("info", Qt::CaseInsensitive) != 0) prefix += " [" + level.toUpper() + "]";
    if (!channel.isEmpty()) prefix += " [" + channel + "]";
    if (!event.isEmpty()) prefix += " " + event;
    return message.isEmpty() ? prefix : prefix + "  " + message;
}
inline QString displayState(ProjectState state) {
    switch(state){
    case ProjectState::Stopped:return "已停止";
    case ProjectState::Preflight:return "检查配置中";
    case ProjectState::ReclaimingPorts:return "释放端口中";
    case ProjectState::Starting:return "启动服务中";
    case ProjectState::Running:return "运行中";
    case ProjectState::Stopping:return "停止中";
    case ProjectState::Failed:return "运行失败";
    case ProjectState::Syncing:return "同步代码中";
    }
    return {};
}
}
