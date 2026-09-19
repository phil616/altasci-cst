#include "UserPage.h"
#include "ui/UiSupport.h"
#include <QFrame>
#include <QScrollArea>
#include <QJsonArray>
#include <QResizeEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QHeaderView>
#include <algorithm>

namespace cst {
UserPage::UserPage(IUrlLauncher &urls, QWidget *parent) : QWidget(parent), urls_(urls) {
    auto *outer=new QVBoxLayout(this);outer->setContentsMargins(24,24,24,24);outer->setSpacing(16);
    auto *scroll=new QScrollArea(this);scroll->setWidgetResizable(true);outer->addWidget(scroll);
    auto *content=new QWidget;scroll->setWidget(content);auto *layout=new QVBoxLayout(content);layout->setContentsMargins(0,0,0,0);layout->setSpacing(20);
    auto *hero=new QFrame(content);hero->setProperty("role","card");auto *heroLayout=new QVBoxLayout(hero);heroLayout->setContentsMargins(24,24,24,24);heroLayout->setSpacing(12);layout->addWidget(hero);
    auto *eyebrow=helpText("项目运行",hero);heroLayout->addWidget(eyebrow);
    title_=new QLabel("尚未选择项目",hero);title_->setProperty("role","heading");title_->setWordWrap(true);heroLayout->addWidget(title_);
    description_=helpText("请到管理员的项目管理页面创建或导入项目。",hero);heroLayout->addWidget(description_);
    statusHelp_=helpText({},hero);statusHelp_->setObjectName("runGuidance");statusHelp_->setProperty("role","status");heroLayout->addWidget(statusHelp_);
    main_=new QPushButton("一键启动",hero);main_->setObjectName("mainAction");main_->setAccessibleName("一键启动或停止当前项目");main_->setMinimumHeight(44);main_->setMinimumWidth(200);main_->setAutoDefault(false);heroLayout->addWidget(main_,0,Qt::AlignLeft);
    connect(main_,&QPushButton::clicked,this,&UserPage::mainAction);
    configure_=new QPushButton("前往管理配置",hero);configure_->setObjectName("configureAction");configure_->setProperty("role","primary");configure_->setAccessibleName("前往管理配置");configure_->setMinimumHeight(44);configure_->setMinimumWidth(200);configure_->setAutoDefault(false);heroLayout->addWidget(configure_,0,Qt::AlignLeft);
    connect(configure_,&QPushButton::clicked,this,&UserPage::configureRequested);
    entries_=new QFrame(content);entries_->setProperty("role","card");auto *entriesLayout=new QVBoxLayout(entries_);entriesLayout->setContentsMargins(24,20,24,20);entriesLayout->setSpacing(12);
    section(entriesLayout,"快捷入口");actionsHelp_=helpText("项目运行后，可以打开服务页面。文档等入口可随时使用。",entries_);entriesLayout->addWidget(actionsHelp_);
    actions_=new QGridLayout;actions_->setSpacing(12);actions_->setColumnStretch(0,1);actions_->setColumnStretch(1,1);entriesLayout->addLayout(actions_);layout->addWidget(entries_);
    detailsToggle_=new QToolButton(this);detailsToggle_->setText("运行详情");detailsToggle_->setCheckable(true);detailsToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);detailsToggle_->setArrowType(Qt::RightArrow);layout->addWidget(detailsToggle_);
    details_=new QWidget(this);auto *detailLayout=new QVBoxLayout(details_);detailLayout->setContentsMargins(0,0,0,0);
    tasks_=new QTableWidget(details_);tasks_->setColumnCount(4);tasks_->setHorizontalHeaderLabels({"任务","状态","重启次数","Console"});tasks_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);tasks_->setMinimumHeight(140);tasks_->setAlternatingRowColors(true);tasks_->setEditTriggers(QAbstractItemView::NoEditTriggers);detailLayout->addWidget(tasks_);
    logs_=new QPlainTextEdit(details_);logs_->setPlaceholderText("运行日志将在启动后显示。需要筛选或导出时，请打开管理员 → 运行日志。");logs_->setMinimumHeight(160);logs_->setProperty("role","code");logs_->setReadOnly(true);logs_->setMaximumBlockCount(5000);logs_->setAccessibleName("最近运行日志");detailLayout->addWidget(logs_);
    details_->hide();layout->addWidget(details_,1);layout->addStretch();
    connect(detailsToggle_,&QToolButton::toggled,this,[this](bool expanded){details_->setVisible(expanded);detailsToggle_->setArrowType(expanded?Qt::DownArrow:Qt::RightArrow);});
    setState(ProjectState::Stopped);
}
void UserPage::setProject(const QJsonObject &document) {
    hasProject_=!document.isEmpty();const auto project=document.value("project").toObject();
    title_->setText(hasProject_?project.value("name").toString():"尚未选择项目");description_->setText(hasProject_?project.value("description").toString():"当前没有可用项目。请先创建或导入项目配置，再回来启动。");
    main_->setVisible(hasProject_);configure_->setVisible(!hasProject_);entries_->setVisible(hasProject_);detailsToggle_->setVisible(hasProject_);if(!hasProject_){detailsToggle_->setChecked(false);details_->hide();}
    for(auto *button:buttons_)delete button;buttons_.clear();
    auto array=project.value("userActions").toArray();QList<QJsonObject> actions;
    for(const auto &value:array)actions.append(value.toObject());
    std::sort(actions.begin(),actions.end(),[](const QJsonObject &a,const QJsonObject &b){return a.value("order").toInt()<b.value("order").toInt();});
    for(const auto &action:actions){
        auto *button=new QPushButton(action.value("label").toString(),this);button->setAccessibleName(button->text());button->setProperty("requiresRunning",action.value("availableWhen")=="running");button->setObjectName(action.value("id").toString());
        button->setMinimumHeight(40);button->setToolTip(action.value("url").toString());button->setAutoDefault(false);buttons_.append(button);
        connect(button,&QPushButton::clicked,this,[this,url=action.value("url").toString()]{try{urls_.open(url);}catch(const std::exception &e){emit error(QString::fromUtf8(e.what()));}});
    }
    actionsHelp_->setText(buttons_.isEmpty()?"尚未配置快捷入口。管理员可在“用户入口”中添加。":"服务入口在项目运行后可用；文档等常用链接可随时打开。");
    tasks_->setRowCount(0);logs_->clear();arrangeActions();setState(state_);
}
void UserPage::setState(ProjectState state) {
    state_=state;QString color="#2563EB",text="一键启动";bool enabled=hasProject_;
    if(state==ProjectState::Stopping){text="正在停止…";color="#B26A00";enabled=false;}
    else if(state==ProjectState::Syncing){text="正在同步代码…";color="#B26A00";enabled=false;}
    else if(state!=ProjectState::Stopped&&state!=ProjectState::Failed){text="一键停止";color="#C62828";}
    const QMap<ProjectState,QString> guidance{
        {ProjectState::Stopped,"已停止 · 点击启动，将依次检查配置、释放端口并启动任务。"},
        {ProjectState::Preflight,"检查配置中 · 正在确认命令与工作目录。可以点击停止取消启动。"},
        {ProjectState::ReclaimingPorts,"释放端口中 · 正在处理端口冲突。可以点击停止取消启动。"},
        {ProjectState::Starting,"启动中 · 正在执行准备步骤并启动任务。"},
        {ProjectState::Running,"运行中 · 任务存活与就绪状态见运行详情。"},
        {ProjectState::Stopping,"停止中 · 正在退出服务并清理进程，请稍候。"},
        {ProjectState::Syncing,"同步中 · 正在更新项目代码，请等待操作完成。"},
        {ProjectState::Failed,"运行失败 · 展开运行详情查看日志，修正配置后可重新启动。"}};
    statusHelp_->setText(hasProject_?guidance.value(state):"尚未选择项目 · 请先创建或导入项目配置。");
    if(configurationDirty_&&hasProject_&&(state==ProjectState::Stopped||state==ProjectState::Failed)){enabled=false;statusHelp_->setText("有未保存的管理配置 · 请先到“管理配置”保存，再启动项目。");}
    if(state==ProjectState::Failed&&hasProject_){detailsToggle_->setChecked(true);details_->show();detailsToggle_->setArrowType(Qt::DownArrow);}
    main_->setText(text);main_->setEnabled(enabled);main_->setProperty("stateColor",color);main_->setProperty("projectState",stateName(state));
    main_->setStyleSheet("QPushButton { background-color: "+color+"; color: white; padding: 12px; } QPushButton:hover { background-color: "+QColor(color).lighter(108).name()+"; } QPushButton:disabled { background: #E2E8F0; color: #64748B; }");
    for(auto *button:buttons_)button->setEnabled(!button->property("requiresRunning").toBool()||state==ProjectState::Running);
}
void UserPage::setActive(bool active){main_->setDefault(active);}
void UserPage::setConfigurationDirty(bool dirty){configurationDirty_=dirty;setState(state_);if(dirty&&hasProject_){statusHelp_->setText("有未保存的管理配置 · 请先到“管理配置”保存，再启动项目。");main_->setToolTip("请先保存管理配置");}else{main_->setToolTip("");}}
void UserPage::setTask(const TaskStatus &task){
    int row=0;for(;row<tasks_->rowCount();++row)if(tasks_->item(row,0)->data(Qt::UserRole).toString()==task.id)break;
    if(row==tasks_->rowCount()){tasks_->insertRow(row);auto *name=new QTableWidgetItem(task.name);name->setData(Qt::UserRole,task.id);tasks_->setItem(row,0,name);}
    else tasks_->item(row,0)->setText(task.name);
    if (!tasks_->cellWidget(row, 3)) {
        auto *open = new QPushButton("打开 Console", tasks_); open->setAutoDefault(false); tasks_->setCellWidget(row, 3, open);
        connect(open, &QPushButton::clicked, this, [this, id = task.id] { emit consoleRequested(id); });
    }
    tasks_->setItem(row,1,new QTableWidgetItem(task.state));tasks_->setItem(row,2,new QTableWidgetItem(QString::number(task.restartCount)));
}
void UserPage::appendLog(const QString &line){logs_->appendHtml(formatLogLineHtml(line));}
void UserPage::arrangeActions(){const int columns=width()<900?1:2;actions_->setColumnStretch(1,columns==2?1:0);for(qsizetype i=0;i<buttons_.size();++i){actions_->removeWidget(buttons_[i]);actions_->addWidget(buttons_[i],int(i)/columns,int(i)%columns);}}
void UserPage::resizeEvent(QResizeEvent *event){QWidget::resizeEvent(event);arrangeActions();}
}
