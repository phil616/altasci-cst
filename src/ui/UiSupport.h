#pragma once
#include "domain/Runtime.h"
#include <QDateTime>
#include <QMap>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
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
struct LogLineParts {
    bool parsed = false;
    QString timestamp;
    QString level;
    QString channel;
    QString event;
    QString message;
};
inline LogLineParts parseLogLine(const QString &line) {
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(line.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
    const auto entry = document.object();
    LogLineParts parts;
    parts.parsed = true;
    parts.timestamp = entry.value("ts").toString();
    const auto parsed = QDateTime::fromString(parts.timestamp, Qt::ISODateWithMs);
    if (parsed.isValid()) parts.timestamp = parsed.toLocalTime().toString("HH:mm:ss");
    parts.level = entry.value("level").toString();
    parts.channel = entry.value("channel").toString();
    parts.event = entry.value("event").toString();
    parts.message = entry.value("message").toString();
    return parts;
}
inline QString stripAnsiEscapes(QString text) {
    static const QRegularExpression osc(QStringLiteral("\\x1B\\][^\\x07]*(?:\\x07|\\x1B\\\\)"));
    static const QRegularExpression csi(QStringLiteral("\\x1B\\[[0-?]*[ -/]*[@-~]"));
    text.remove(osc);
    text.remove(csi);
    text.remove(QChar('\r'));
    return text;
}
inline QString ansiToHtml(const QString &text) {
    static const QRegularExpression sgr(QStringLiteral("\\x1B\\[([0-9;]*)m"));
    static const QMap<int, QString> normal{{30,"#0f172a"},{31,"#dc2626"},{32,"#16a34a"},{33,"#d97706"},{34,"#2563eb"},{35,"#9333ea"},{36,"#0891b2"},{37,"#64748b"}};
    static const QMap<int, QString> bright{{90,"#64748b"},{91,"#ef4444"},{92,"#22c55e"},{93,"#f59e0b"},{94,"#3b82f6"},{95,"#a855f7"},{96,"#06b6d4"},{97,"#0f172a"}};
    QString output;
    QString color;
    bool bold = false;
    const auto append = [&](const QString &segment) {
        const auto cleaned = stripAnsiEscapes(segment);
        if (cleaned.isEmpty()) return;
        QString style = "color:" + (color.isEmpty() ? QStringLiteral("#334155") : color) + ';';
        if (bold) style += QStringLiteral("font-weight:600;");
        output += "<span style=\"" + style + "\">" + cleaned.toHtmlEscaped() + "</span>";
    };
    qsizetype last = 0;
    auto matches = sgr.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        append(text.mid(last, match.capturedStart() - last));
        const auto parameters = match.captured(1).split(';', Qt::SkipEmptyParts);
        for (const auto &parameter : parameters) {
            bool ok = false;
            const auto code = parameter.toInt(&ok);
            if (!ok) continue;
            if (code == 0) { color.clear(); bold = false; }
            else if (code == 1) bold = true;
            else if (code == 22) bold = false;
            else if (code == 39) color.clear();
            else if (normal.contains(code)) color = normal.value(code);
            else if (bright.contains(code)) color = bright.value(code);
        }
        last = match.capturedEnd();
    }
    append(text.mid(last));
    return output;
}
inline QString logPrefix(const LogLineParts &parts) {
    QString prefix = parts.timestamp;
    if (!parts.level.isEmpty() && parts.level.compare("info", Qt::CaseInsensitive) != 0) prefix += " [" + parts.level.toUpper() + "]";
    if (!parts.channel.isEmpty()) prefix += " [" + parts.channel + "]";
    if (!parts.event.isEmpty()) prefix += " " + parts.event;
    return prefix;
}
inline QString formatLogLine(const QString &line) {
    const auto parts = parseLogLine(line);
    if (!parts.parsed) return stripAnsiEscapes(line);
    const auto prefix = logPrefix(parts);
    const auto message = stripAnsiEscapes(parts.message);
    return message.isEmpty() ? prefix : prefix + "  " + message;
}
inline QString formatLogLineHtml(const QString &line) {
    const auto parts = parseLogLine(line);
    if (!parts.parsed) return ansiToHtml(line);
    const auto prefix = logPrefix(parts);
    QString html = "<span style=\"color:#94a3b8;\">" + prefix.toHtmlEscaped() + "</span>";
    if (!parts.message.isEmpty()) html += "  " + ansiToHtml(parts.message);
    return html;
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
