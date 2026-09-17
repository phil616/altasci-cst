#pragma once
#include "domain/Runtime.h"
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
