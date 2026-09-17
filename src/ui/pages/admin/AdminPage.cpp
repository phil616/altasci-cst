#include "AdminPage.h"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QMessageBox>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QUuid>

namespace cst {
namespace {
QPushButton *button(const QString &label, QVBoxLayout *layout, const std::function<void()> &action) {
    auto *widget=new QPushButton(label,layout->parentWidget());widget->setAutoDefault(false);widget->setAccessibleName(label);layout->addWidget(widget);
    QObject::connect(widget,&QPushButton::clicked,widget,action);return widget;
}
QVBoxLayout *column(QWidget *widget){auto *layout=new QVBoxLayout(widget);layout->setSpacing(12);layout->setContentsMargins(0,0,0,0);return layout;}
QString issuesText(const ValidationIssues &issues){QStringList result;for(const auto &issue:issues)result.append(issue.path+": "+issue.message);return result.join('\n');}
}
AdminPage::AdminPage(ProjectRuntimeService &runtime, ProjectConfigService &configuration, ProjectCatalogService &catalog,
    ICredentialStore &credentials, IPortManager &ports, SourceSyncService &sync, DiagnosticExportService &diagnostics,
    LogService &logs, QJsonObject schema, ProjectPaths paths, QWidget *parent)
    : QWidget(parent),runtime_(runtime),configuration_(configuration),catalog_(catalog),credentials_(credentials),ports_(ports),sync_(sync),
      diagnostics_(diagnostics),logsService_(logs),schema_(std::move(schema)),paths_(std::move(paths)) {
    auto *outer=new QHBoxLayout(this);outer->setContentsMargins(24,24,24,24);outer->setSpacing(24);
    navigation_=new QListWidget(this);navigation_->setFixedWidth(220);navigation_->setAccessibleName("管理员页面导航");
    navigation_->addItems({"项目 / 概览","项目 / 任务与命令","项目 / 端口","项目 / 用户按钮","维护 / 代码同步","维护 / 环境与凭据","诊断 / 日志","配置 / 导入导出"});
    for(int i=0;i<navigation_->count();++i)navigation_->item(i)->setSizeHint(QSize(200,qMax(40,fontMetrics().height()+16)));
    outer->addWidget(navigation_);auto *right=new QVBoxLayout;outer->addLayout(right,1);breadcrumb_=new QLabel(this);right->addWidget(breadcrumb_);
    pages_=new QStackedWidget(this);right->addWidget(pages_,1);auto *footer=new QHBoxLayout;right->addLayout(footer);
    saveStatus_=new QLabel("没有未保存修改",this);footer->addWidget(saveStatus_,1);save_=new QPushButton("保存",this);discard_=new QPushButton("放弃",this);footer->addWidget(save_);footer->addWidget(discard_);
    connect(save_,&QPushButton::clicked,this,&AdminPage::save);connect(discard_,&QPushButton::clicked,this,[this]{runtime_.setEditing(false);setProject(runtime_.currentProject());});
    connect(navigation_,&QListWidget::currentRowChanged,this,[this](int row){pages_->setCurrentIndex(row);if(row>=0)breadcrumb_->setText(navigation_->item(row)->text());});
    rebuild();navigation_->setCurrentRow(0);
}
void AdminPage::setProject(const QJsonObject &document){draft_=document;newProject_=false;modified_=false;rebuild();updateState();}
void AdminPage::dirty(){modified_=true;runtime_.setEditing(true);saveStatus_->setText("有未保存修改");updateState();}
void AdminPage::setField(const QString &field,const QJsonValue &value){auto project=draft_.value("project").toObject();project[field]=value;draft_["project"]=project;dirty();}
SchemaEditor *AdminPage::fieldEditor(const QString &field,QWidget *parent){
    const auto rule=schema_.value("$defs").toObject().value("project").toObject().value("properties").toObject().value(field).toObject();
    auto *editor=new SchemaEditor(schema_,rule,draft_.value("project").toObject().value(field),parent);
    connect(editor,&SchemaEditor::changed,this,[this,editor,field]{setField(field,editor->value());});editControls_.append(editor);return editor;
}
QWidget *AdminPage::page(int index){return qobject_cast<QScrollArea *>(pages_->widget(index))->widget();}
void AdminPage::updateState(){
    const bool editable=runtime_.editable()&&!runtime_.busy();
    for(auto *control:editControls_)control->setEnabled(editable);
    save_->setEnabled(editable&&modified_);discard_->setEnabled(editable&&modified_);
    saveStatus_->setText(modified_?"有未保存修改":"没有未保存修改");
}
SyncRequest AdminPage::request()const{
    const auto project=draft_.value("project").toObject(),source=project.value("source").toObject();
    return {project.value("id").toString(),source.value("repositoryUrl").toString(),source.value("branch").toString(),source.value("workingDirectory").toString(),source.value("gitExecutable").toString(),source.value("credentialTarget").toString(),runtime_.operationId()};
}
void AdminPage::save(){
    auto project=draft_.value("project").toObject();auto source=project.value("source").toObject();
    source["credentialTarget"]="CST/git/"+project.value("id").toString()+'/'+QUrl(source.value("repositoryUrl").toString()).host();project["source"]=source;draft_["project"]=project;
    const auto issues=configuration_.validate(draft_);if(!issues.isEmpty()){showConfigurationProblem(issuesText(issues));return;}
    if(newProject_)runtime_.importProject(draft_);else runtime_.saveConfig(draft_);
}
void AdminPage::buildTasks(QWidget *parent){
    auto *layout=column(parent);auto *splitter=new QSplitter(parent);layout->addWidget(splitter,1);
    auto *left=new QWidget(splitter);auto *leftLayout=column(left);auto *list=new QListWidget(left);leftLayout->addWidget(list);
    auto *detail=new QWidget(splitter);auto *details=column(detail);splitter->setStretchFactor(1,1);
    auto data=std::make_shared<QJsonArray>(draft_.value("project").toObject().value("tasks").toArray());
    auto active=std::make_shared<SchemaEditor *>(nullptr);const auto taskRule=QJsonObject{{"$ref","#/$defs/task"}};
    const auto refresh=[list,data]{QSignalBlocker blocker(list);list->clear();for(const auto &value:*data)list->addItem(value.toObject().value("name").toString());};refresh();
    connect(list,&QListWidget::currentRowChanged,this,[this,detail,details,data,active,taskRule,list](int row){
        if(*active){editControls_.removeAll(*active);details->removeWidget(*active);(*active)->deleteLater();*active=nullptr;}
        if(row<0||row>=data->size())return;
        *active=new SchemaEditor(schema_,taskRule,(*data)[row],detail);details->addWidget(*active);editControls_.append(*active);(*active)->setEnabled(runtime_.editable());
        connect(*active,&SchemaEditor::changed,this,[this,data,active,list,row]{(*data)[row]=(*active)->value();list->item(row)->setText((*data)[row].toObject().value("name").toString());setField("tasks",*data);});
    });
    editControls_.append(button("添加任务",leftLayout,[this,data,list,refresh,taskRule]{auto value=SchemaEditor::initialValue(schema_,taskRule).toObject();value["name"]="新任务";value["order"]=int(data->size()+1)*10;auto service=value.value("serviceCommand").toObject();service["timeoutMs"]=0;value["serviceCommand"]=service;data->append(value);refresh();list->setCurrentRow(int(data->size())-1);setField("tasks",*data);}));
    editControls_.append(button("删除任务",leftLayout,[this,data,list,refresh]{const auto row=list->currentRow();if(row<0)return;data->removeAt(row);list->setCurrentRow(-1);refresh();list->setCurrentRow(data->isEmpty()?-1:qMin(row,int(data->size())-1));setField("tasks",*data);}));
    if(!data->isEmpty())list->setCurrentRow(0);
}
void AdminPage::rebuild(){
    const auto selected=qMax(0,navigation_->currentRow());editControls_.clear();logView_=nullptr;search_=nullptr;taskFilter_=nullptr;issues_=nullptr;
    while(pages_->count()){auto *old=pages_->widget(0);pages_->removeWidget(old);delete old;}
    for(int i=0;i<8;++i){auto *scroll=new QScrollArea(pages_);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setWidget(new QWidget);pages_->addWidget(scroll);}
    const auto project=draft_.value("project").toObject();
    auto *overview=column(page(0));
    overview->addWidget(new QLabel("当前状态："+stateName(runtime_.state()),page(0)));
    for(const auto &field:{"name","description"}){overview->addWidget(new QLabel(fieldLabel(field),page(0)));overview->addWidget(fieldEditor(field,page(0)));}
    overview->addWidget(new QLabel("项目 ID："+project.value("id").toString(),page(0)));
    overview->addWidget(new QLabel("启动配置",page(0)));overview->addWidget(fieldEditor("settings",page(0)));
    editControls_.append(button("设为默认项目",overview,[this]{runtime_.setDefault(draft_.value("project").toObject().value("id").toString());}));overview->addStretch();
    buildTasks(page(1));
    auto *portsLayout=column(page(2));auto *portTable=new QTableWidget(page(2));portTable->setColumnCount(4);portTable->setHorizontalHeaderLabels({"协议","地址","端口","所属任务"});portTable->setEditTriggers(QAbstractItemView::NoEditTriggers);portTable->horizontalHeader()->setStretchLastSection(true);
    const auto required=project.value("requiredPorts").toArray();portTable->setRowCount(int(required.size()));
    for(qsizetype row=0;row<required.size();++row){const auto port=required[row].toObject();int col=0;for(const auto &field:{"protocol","address","port","ownerTaskId"}){const auto value=port.value(field);portTable->setItem(int(row),col++,new QTableWidgetItem(value.isDouble()?QString::number(value.toInt()):value.toString()));}}
    portsLayout->addWidget(portTable);portsLayout->addWidget(fieldEditor("requiredPorts",page(2)));
    editControls_.append(button("立即检测",portsLayout,[this]{const auto ports=PortReclaimService::requirements(draft_.value("project").toObject().value("requiredPorts").toArray());
        runtime_.maintenance([this,ports](const Cancellation &cancel){QStringList report;for(const auto &port:ports){cancel.check();const auto owners=ports_.owners(port);if(owners.isEmpty())report.append(QString::number(port.port)+"：空闲");for(const auto &owner:owners)report.append(QString::number(port.port)+" PID="+QString::number(owner.pid)+" "+owner.imagePath);}const auto text=report.join('\n');QMetaObject::invokeMethod(this,[this,text]{QMessageBox::information(this,"端口检测",text.isEmpty()?"未配置端口":text);},Qt::QueuedConnection);});}));
    auto *actions=column(page(3));actions->addWidget(fieldEditor("userActions",page(3)));actions->addWidget(new QLabel("按钮预览（保存后更新用户页）",page(3)));
    for(const auto &value:project.value("userActions").toArray()){auto *preview=new QPushButton(value.toObject().value("label").toString(),page(3));preview->setEnabled(false);actions->addWidget(preview);}actions->addStretch();
    auto *sourceLayout=column(page(4));auto *warning=new QLabel("同步会以远程分支替换整个本地项目目录，本地修改和生成文件不会保留。",page(4));warning->setWordWrap(true);sourceLayout->addWidget(warning);
    sourceLayout->addWidget(fieldEditor("source",page(4)));
    auto *credentialStatus=new QLabel(page(4));
    try{credentialStatus->setText(credentials_.read(request().credentialTarget)?"凭据：已保存":"凭据：未保存");}catch(const std::exception &e){credentialStatus->setText(QString::fromUtf8(e.what()));}sourceLayout->addWidget(credentialStatus);
    editControls_.append(button("测试认证",sourceLayout,[this]{if(modified_){emit error("请先保存配置");return;}const auto value=request();runtime_.maintenance([this,value](const Cancellation &cancel){sync_.testAuthentication(value,cancel);});}));
    editControls_.append(button("同步代码",sourceLayout,[this]{runtime_.synchronize();}));
    auto *head=new QLabel("最近 HEAD：尚未同步",page(4));sourceLayout->addWidget(head);connect(&runtime_,&ProjectRuntimeService::headChanged,head,[head](const QString &value){head->setText("最近 HEAD："+value);});
    auto *environment=column(page(5));environment->addWidget(new QLabel("工具查找目录",page(5)));environment->addWidget(fieldEditor("toolDirectories",page(5)));
    editControls_.append(button("编辑环境文件",environment,[this]{editEnv();}));
    auto *username=new QLineEdit(page(5));username->setAccessibleName("Git 用户名");auto *password=new QLineEdit(page(5));password->setEchoMode(QLineEdit::Password);password->setAccessibleName("Git PAT");
    auto *credentialForm=new QFormLayout;credentialForm->addRow("用户名",username);credentialForm->addRow("PAT",password);environment->addLayout(credentialForm);editControls_.append(username);editControls_.append(password);
    editControls_.append(button("保存凭据",environment,[this,username,password]{const auto target=request().credentialTarget;const Credential value{username->text(),password->text()};logsService_.addSecret(value.password);password->clear();runtime_.maintenance([this,target,value](const Cancellation &cancel){cancel.check();credentials_.write(target,value);});}));
    editControls_.append(button("删除凭据",environment,[this]{const auto target=request().credentialTarget;runtime_.maintenance([this,target](const Cancellation &cancel){cancel.check();credentials_.remove(target);});}));environment->addStretch();
    auto *logging=column(page(6));auto *filter=new QHBoxLayout;logging->addLayout(filter);taskFilter_=new QComboBox(page(6));taskFilter_->addItem("全部任务","");for(const auto &value:project.value("tasks").toArray()){const auto task=value.toObject();taskFilter_->addItem(task.value("name").toString(),task.value("id").toString());}filter->addWidget(taskFilter_);
    search_=new QLineEdit(page(6));search_->setPlaceholderText("搜索日志");search_->setAccessibleName("搜索日志");filter->addWidget(search_,1);
    logView_=new QPlainTextEdit(page(6));logView_->setReadOnly(true);logView_->setMaximumBlockCount(5000);logging->addWidget(logView_,1);
    connect(search_,&QLineEdit::textChanged,this,&AdminPage::renderLogs);connect(taskFilter_,&QComboBox::currentIndexChanged,this,&AdminPage::renderLogs);
    button("复制日志",logging,[this]{QApplication::clipboard()->setText(logView_->textCursor().hasSelection()?logView_->textCursor().selectedText():logView_->toPlainText());});
    button("打开日志目录",logging,[this]{const auto id=draft_.value("project").toObject().value("id").toString();if(!QDesktopServices::openUrl(QUrl::fromLocalFile(paths_.logDirectory(id))))emit error("无法打开日志目录");});
    editControls_.append(button("导出诊断包",logging,[this]{const auto path=QFileDialog::getSaveFileName(this,"导出诊断包",{},"ZIP (*.zip)");if(path.isEmpty())return;const auto config=runtime_.currentProject();const auto tasks=runtime_.taskSnapshot();const auto operation=runtime_.operationId();runtime_.maintenance([this,config,tasks,path,operation](const Cancellation &cancel){diagnostics_.exportZip(config,tasks,path,operation,cancel);});}));renderLogs();
    auto *configLayout=column(page(7));issues_=new QLabel("配置验证结果将在保存或校验后显示。",page(7));issues_->setWordWrap(true);issues_->setTextInteractionFlags(Qt::TextSelectableByMouse);configLayout->addWidget(issues_);
    button("校验当前配置",configLayout,[this]{const auto issues=configuration_.validate(draft_);issues_->setText(issues.isEmpty()?"配置有效":issuesText(issues));});
    editControls_.append(button("创建项目",configLayout,[this]{if(modified_){emit error("请先保存或放弃编辑");return;}bool ok=false;const auto name=QInputDialog::getText(this,"创建项目","项目名称",QLineEdit::Normal,{},&ok);if(!ok||name.isEmpty())return;draft_=configuration_.create(name,"C:\\CSTProjects\\"+QUuid::createUuid().toString(QUuid::WithoutBraces),"C:\\Program Files\\Git\\cmd\\git.exe");newProject_=true;dirty();rebuild();navigation_->setCurrentRow(0);}));
    editControls_.append(button("导入 JSON",configLayout,[this]{if(modified_){emit error("请先保存或放弃编辑");return;}const auto path=QFileDialog::getOpenFileName(this,"导入项目",{},"JSON (*.json)");if(path.isEmpty())return;try{runtime_.importProject(configuration_.load(path));}catch(const std::exception &e){showConfigurationProblem(QString::fromUtf8(e.what()));}}));
    editControls_.append(button("导出 JSON",configLayout,[this]{const auto path=QFileDialog::getSaveFileName(this,"导出项目",{},"JSON (*.json)");if(!path.isEmpty())runtime_.exportProject(path);}));
    auto *projects=new QComboBox(page(7));projects->setAccessibleName("切换项目");
    try{for(const auto &entry:catalog_.entries())projects->addItem(entry.name,entry.id);}catch(const std::exception &e){issues_->setText(QString::fromUtf8(e.what()));}
    projects->setCurrentIndex(projects->findData(project.value("id").toString()));configLayout->addWidget(projects);editControls_.append(projects);
    editControls_.append(button("切换到所选项目",configLayout,[this,projects]{if(projects->currentIndex()>=0)runtime_.switchProject(projects->currentData().toString());}));
    editControls_.append(button("所选项目设为默认",configLayout,[this,projects]{if(projects->currentIndex()>=0)runtime_.setDefault(projects->currentData().toString());}));
    editControls_.append(button("删除所选项目配置",configLayout,[this,projects]{if(modified_){emit error("请先保存或放弃编辑");return;}if(projects->currentIndex()>=0)runtime_.removeProject(projects->currentData().toString());}));configLayout->addStretch();
    pages_->setCurrentIndex(selected);updateState();
}
void AdminPage::showConfigurationProblem(const QString &message){navigation_->setCurrentRow(7);issues_->setText(message);}
void AdminPage::appendLog(const QString &task,const QString &line){logLines_.append({task,line});if(logLines_.size()>5000)logLines_.removeFirst();if(logView_&&(taskFilter_->currentData().toString().isEmpty()||taskFilter_->currentData().toString()==task)&&line.contains(search_->text(),Qt::CaseInsensitive))logView_->appendPlainText(line);}
void AdminPage::renderLogs(){if(!logView_)return;QStringList lines;const auto task=taskFilter_->currentData().toString();for(const auto &line:logLines_)if((task.isEmpty()||line.first==task)&&line.second.contains(search_->text(),Qt::CaseInsensitive))lines.append(line.second);logView_->setPlainText(lines.join('\n'));}
void AdminPage::editEnv(){
    const auto project=draft_.value("project").toObject();const auto id=project.value("id").toString();const auto directory=paths_.dataDirectory(id)+"\\env";
    const auto path=QFileDialog::getSaveFileName(this,"选择或创建环境文件",directory,"环境文件 (*.env)");if(path.isEmpty())return;
    if(!isWithinWindowsPath(path,paths_.dataDirectory(id))){emit error("环境文件必须放在当前项目的数据目录内");return;}
    QDialog dialog(this);dialog.setWindowTitle("编辑环境文件");dialog.resize(680,480);auto *layout=new QVBoxLayout(&dialog);auto *editor=new QPlainTextEdit(&dialog);layout->addWidget(editor);
    QFile file(path);if(file.exists()){if(!file.open(QIODevice::ReadOnly)){emit error(file.errorString());return;}const auto bytes=file.readAll();try{parseEnv(bytes);}catch(const std::exception &e){emit error(QString::fromUtf8(e.what()));return;}editor->setPlainText(QString::fromUtf8(bytes));}
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&dialog);layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted)return;const auto bytes=editor->toPlainText().toUtf8();
    try{parseEnv(bytes);}catch(const std::exception &e){emit error(QString::fromUtf8(e.what()));return;}
    runtime_.maintenance([path,bytes](const Cancellation &cancel){cancel.check();if(!QDir().mkpath(QFileInfo(path).absolutePath()))throw std::runtime_error("无法创建环境文件目录");QSaveFile output(path);if(!output.open(QIODevice::WriteOnly)||output.write(bytes)!=bytes.size()||!output.commit())throw std::runtime_error("环境文件保存失败");});
}
}
