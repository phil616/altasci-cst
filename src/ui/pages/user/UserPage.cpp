#include "UserPage.h"
#include <QJsonArray>
#include <QResizeEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QHeaderView>
#include <algorithm>

namespace cst {
UserPage::UserPage(IUrlLauncher &urls, QWidget *parent) : QWidget(parent), urls_(urls) {
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(24,24,24,24); layout->setSpacing(12);
    title_=new QLabel("尚未选择项目",this); auto font=title_->font();font.setPointSize(font.pointSize()+5);title_->setFont(font);layout->addWidget(title_);
    description_=new QLabel("请到管理员页面创建或导入项目。",this);description_->setProperty("role","muted");description_->setWordWrap(true);layout->addWidget(description_);
    main_=new QPushButton("一键启动",this); main_->setObjectName("mainAction");main_->setAccessibleName("一键启动或停止当前项目");main_->setMinimumHeight(48);main_->setAutoDefault(false);layout->addWidget(main_);
    connect(main_,&QPushButton::clicked,this,&UserPage::mainAction);
    actions_=new QGridLayout;actions_->setSpacing(12);layout->addLayout(actions_);
    auto *toggle=new QToolButton(this);toggle->setText("运行详情");toggle->setCheckable(true);toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);toggle->setArrowType(Qt::RightArrow);layout->addWidget(toggle);
    auto *details=new QWidget(this);auto *detailLayout=new QVBoxLayout(details);detailLayout->setContentsMargins(0,0,0,0);
    tasks_=new QTableWidget(details);tasks_->setColumnCount(3);tasks_->setHorizontalHeaderLabels({"任务","状态","重启次数"});tasks_->horizontalHeader()->setStretchLastSection(true);tasks_->setEditTriggers(QAbstractItemView::NoEditTriggers);detailLayout->addWidget(tasks_);
    logs_=new QPlainTextEdit(details);logs_->setProperty("role","code");logs_->setReadOnly(true);logs_->setMaximumBlockCount(5000);logs_->setAccessibleName("最近运行日志");detailLayout->addWidget(logs_);
    details->hide();layout->addWidget(details,1);layout->addStretch();
    connect(toggle,&QToolButton::toggled,this,[details,toggle](bool expanded){details->setVisible(expanded);toggle->setArrowType(expanded?Qt::DownArrow:Qt::RightArrow);});
    setState(ProjectState::Stopped);
}
void UserPage::setProject(const QJsonObject &document) {
    hasProject_=!document.isEmpty();const auto project=document.value("project").toObject();
    title_->setText(hasProject_?project.value("name").toString():"尚未选择项目");description_->setText(project.value("description").toString());
    for(auto *button:buttons_)delete button;buttons_.clear();
    auto array=project.value("userActions").toArray();QList<QJsonObject> actions;
    for(const auto &value:array)actions.append(value.toObject());
    std::sort(actions.begin(),actions.end(),[](const QJsonObject &a,const QJsonObject &b){return a.value("order").toInt()<b.value("order").toInt();});
    for(const auto &action:actions){
        auto *button=new QPushButton(action.value("label").toString(),this);button->setAccessibleName(button->text());button->setProperty("requiresRunning",action.value("availableWhen")=="running");button->setObjectName(action.value("id").toString());
        button->setAutoDefault(false);buttons_.append(button);
        connect(button,&QPushButton::clicked,this,[this,url=action.value("url").toString()]{try{urls_.open(url);}catch(const std::exception &e){emit error(QString::fromUtf8(e.what()));}});
    }
    tasks_->setRowCount(0);logs_->clear();arrangeActions();setState(state_);
}
void UserPage::setState(ProjectState state) {
    state_=state;QString color="#2563EB",text="一键启动";bool enabled=hasProject_;
    if(state==ProjectState::Stopping){text="正在停止…";color="#B26A00";enabled=false;}
    else if(state==ProjectState::Syncing){text="正在同步代码…";color="#B26A00";enabled=false;}
    else if(state!=ProjectState::Stopped&&state!=ProjectState::Failed){text="一键停止";color="#C62828";}
    main_->setText(text);main_->setEnabled(enabled);main_->setProperty("stateColor",color);main_->setProperty("projectState",stateName(state));
    main_->setStyleSheet("QPushButton { background-color: "+color+"; color: white; padding: 12px; } QPushButton:hover { background-color: "+QColor(color).lighter(108).name()+"; } QPushButton:disabled { background: #E2E8F0; color: #64748B; }");
    for(auto *button:buttons_)button->setEnabled(!button->property("requiresRunning").toBool()||state==ProjectState::Running);
}
void UserPage::setActive(bool active){main_->setDefault(active);}
void UserPage::setTask(const TaskStatus &task){
    int row=0;for(;row<tasks_->rowCount();++row)if(tasks_->item(row,0)->data(Qt::UserRole).toString()==task.id)break;
    if(row==tasks_->rowCount()){tasks_->insertRow(row);auto *name=new QTableWidgetItem(task.name);name->setData(Qt::UserRole,task.id);tasks_->setItem(row,0,name);}
    tasks_->setItem(row,1,new QTableWidgetItem(task.state));tasks_->setItem(row,2,new QTableWidgetItem(QString::number(task.restartCount)));
}
void UserPage::appendLog(const QString &line){logs_->appendPlainText(line);}
void UserPage::arrangeActions(){const int columns=width()<900?1:2;for(qsizetype i=0;i<buttons_.size();++i){actions_->removeWidget(buttons_[i]);actions_->addWidget(buttons_[i],int(i)/columns,int(i)%columns);}}
void UserPage::resizeEvent(QResizeEvent *event){QWidget::resizeEvent(event);arrangeActions();}
}
