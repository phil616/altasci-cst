#include "AdminPage.h"
#include "domain/Configuration.h"
#include "ui/UiSupport.h"
#include <QGridLayout>
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
#include <QKeySequence>
#include <QMessageBox>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace cst {
namespace {
QPushButton *button(const QString &label, QVBoxLayout *layout, const std::function<void()> &action) {
    auto *widget=new QPushButton(label,layout->parentWidget());widget->setAutoDefault(false);widget->setAccessibleName(label);
    widget->setProperty("requiresProject",label!="创建项目"&&label!="导入 JSON");
    if(label.startsWith("删除"))widget->setProperty("role","danger");
    if(label=="同步代码"||label=="创建项目"||label=="保存凭据")widget->setProperty("role","primary");
    QWidget *toolbar=nullptr;
    if(layout->count())toolbar=layout->itemAt(layout->count()-1)->widget();
    if(!toolbar||toolbar->objectName()!="actionToolbar"){
        toolbar=new QWidget(layout->parentWidget());toolbar->setObjectName("actionToolbar");toolbar->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
        auto *grid=new QGridLayout(toolbar);grid->setContentsMargins(0,0,0,0);grid->setSpacing(8);grid->setAlignment(Qt::AlignTop|Qt::AlignLeft);layout->addWidget(toolbar);
    }
    auto *grid=qobject_cast<QGridLayout *>(toolbar->layout());const int count=toolbar->property("buttonCount").toInt();
    const int columns=layout->parentWidget()->maximumWidth()<=220?1:3;
    grid->addWidget(widget,count/columns,count%columns);toolbar->setProperty("buttonCount",count+1);
    QObject::connect(widget,&QPushButton::clicked,widget,action);return widget;
}
QVBoxLayout *column(QWidget *widget){auto *layout=new QVBoxLayout(widget);layout->setSpacing(16);layout->setAlignment(Qt::AlignTop);layout->setContentsMargins(widget->objectName()=="contentCard"?20:0,widget->objectName()=="contentCard"?20:0,widget->objectName()=="contentCard"?20:0,widget->objectName()=="contentCard"?20:0);return layout;}
QString issuesText(const ValidationIssues &issues){QStringList result;for(const auto &issue:issues)result.append(issue.path+": "+issue.message);return result.join('\n');}
}
AdminPage::AdminPage(ProjectRuntimeService &runtime, ProjectConfigService &configuration, ProjectCatalogService &catalog,
    ICredentialStore &credentials, IPortManager &ports, SourceSyncService &sync, DiagnosticExportService &diagnostics,
    LogService &logs, QJsonObject schema, ProjectPaths paths, QWidget *parent)
    : QWidget(parent),runtime_(runtime),configuration_(configuration),catalog_(catalog),credentials_(credentials),ports_(ports),sync_(sync),
      diagnostics_(diagnostics),logsService_(logs),schema_(std::move(schema)),paths_(std::move(paths)) {
    auto *outer=new QHBoxLayout(this);outer->setContentsMargins(24,24,24,24);outer->setSpacing(24);
    navigation_=new QListWidget(this);navigation_->setObjectName("adminSidebar");navigation_->setFixedWidth(220);navigation_->setAccessibleName("管理员页面导航");
    navigation_->addItems({"项目概览","任务与命令","端口管理","用户入口","代码同步","环境与凭据","运行日志","项目管理"});
    for(int i=0;i<navigation_->count();++i)navigation_->item(i)->setSizeHint(QSize(200,qMax(40,fontMetrics().height()+16)));
    outer->addWidget(navigation_);auto *right=new QVBoxLayout;outer->addLayout(right,1);breadcrumb_=new QLabel(this);breadcrumb_->setProperty("role","heading");right->setSpacing(12);right->addWidget(breadcrumb_);pageHelp_=helpText({},this);right->addWidget(pageHelp_);
    pages_=new QStackedWidget(this);right->addWidget(pages_,1);auto *footer=new QHBoxLayout;right->addLayout(footer);
    saveStatus_=new QLabel("没有未保存修改",this);footer->addWidget(saveStatus_,1);save_=new QPushButton("保存配置",this);save_->setProperty("role","primary");save_->setShortcut(QKeySequence::Save);save_->setToolTip("保存配置（Ctrl+S）");discard_=new QPushButton("撤销修改",this);discard_->setToolTip("放弃当前未保存修改");footer->addWidget(save_);footer->addWidget(discard_);
    connect(save_,&QPushButton::clicked,this,&AdminPage::save);connect(discard_,&QPushButton::clicked,this,[this]{runtime_.setEditing(false);setProject(runtime_.currentProject());});
    connect(navigation_,&QListWidget::currentRowChanged,this,[this](int row){pages_->setCurrentIndex(row);if(row>=0){breadcrumb_->setText(navigation_->item(row)->text());
        const QStringList descriptions{
            "设置项目名称与启动参数。修改后点击下方“保存配置”使其生效。",
            "先选择任务，再编辑命令、环境和健康检查。任务按顺序值启动，按相反顺序停止。",
            "声明服务需要的端口及所属任务。启动前会检查并尝试释放冲突端口。",
            "配置运行页的快捷入口。可以设置为始终可用，或仅在项目运行后可用。",
            "填写仓库 → 保存配置 → 测试认证 → 同步代码。项目必须停止后才能同步。",
            "设置工具搜索路径与 Git 凭据。PAT 存入 Windows 凭据管理器，不写入项目 JSON。",
            "按任务与关键词查看最近日志。复制所选内容，或导出诊断包用于排查问题。",
            "首次使用请创建或导入项目；已有项目可以切换、导出备份或设为默认项目。"};
        pageHelp_->setText(descriptions.value(row));}});
    rebuild();navigation_->setCurrentRow(0);
}
void AdminPage::setProject(const QJsonObject &document){runtime_.setEditing(false);draft_=document;newProject_=false;modified_=false;emit editingChanged(false);rebuild();updateState();}
void AdminPage::dirty(){modified_=true;runtime_.setEditing(true);emit editingChanged(true);saveStatus_->setText("有未保存修改");updateState();}
void AdminPage::setField(const QString &field,const QJsonValue &value){auto project=draft_.value("project").toObject();project[field]=value;draft_["project"]=project;dirty();}
SchemaEditor *AdminPage::fieldEditor(const QString &field,QWidget *parent){
    auto rule=schema_.value("$defs").toObject().value("project").toObject().value("properties").toObject().value(field).toObject();
    if(field=="description")rule["uiMultiline"]="description";
    auto *editor=new SchemaEditor(schema_,rule,draft_.value("project").toObject().value(field),parent,[this](const QString &text){return resolvePathForPreview(text);});editor->setObjectName("project."+field);
    connect(editor,&SchemaEditor::changed,this,[this,editor,field]{setField(field,editor->value());});editControls_.append(editor);return editor;
}
QWidget *AdminPage::page(int index){return qobject_cast<QScrollArea *>(pages_->widget(index))->widget();}
void AdminPage::updateState(){
    const bool editable=runtime_.editable()&&!runtime_.busy();
    for(auto *control:editControls_){
        const bool requiresProject=!control->property("requiresProject").isValid()||control->property("requiresProject").toBool();
        control->setEnabled(editable&&(!requiresProject||!draft_.isEmpty()));
    }
    if(testAuthButton_)testAuthButton_->setEnabled(editable&&!draft_.isEmpty()&&!modified_);
    if(syncButton_)syncButton_->setEnabled(editable&&!draft_.isEmpty()&&!modified_);
    save_->setEnabled(editable&&!draft_.isEmpty()&&modified_);discard_->setEnabled(editable&&!draft_.isEmpty()&&modified_);
    saveStatus_->setText(!editable?"项目运行或操作中：停止后可编辑配置":draft_.isEmpty()?"尚未选择项目：请创建或导入项目":modified_?"有未保存修改 · 保存后生效":"配置已保存");
    refreshCredentialStatus();
}
SyncRequest AdminPage::request()const{
    const auto project=draft_.value("project").toObject(),source=project.value("source").toObject();
    return {project.value("id").toString(),source.value("repositoryUrl").toString(),source.value("branch").toString(),source.value("workingDirectory").toString(),source.value("gitExecutable").toString(),source.value("credentialTarget").toString(),runtime_.operationId()};
}
void AdminPage::save(){
    normalizeDraftPaths();
    auto project=draft_.value("project").toObject();auto source=project.value("source").toObject();
    source["credentialTarget"]="CST/git/"+project.value("id").toString()+'/'+QUrl(source.value("repositoryUrl").toString()).host();project["source"]=source;draft_["project"]=project;
    const auto issues=configuration_.validate(draft_);if(!issues.isEmpty()){showConfigurationProblem(issuesText(issues));return;}
    if(newProject_)runtime_.importProject(draft_);else runtime_.saveConfig(draft_);
}
void AdminPage::buildTasks(QWidget *parent){
    auto *layout=column(parent);auto *splitter=new QSplitter(parent);layout->addWidget(splitter,1);
    auto *left=new QWidget(splitter);left->setMinimumWidth(140);left->setMaximumWidth(180);auto *leftLayout=column(left);leftLayout->setAlignment(Qt::AlignTop);auto *list=new QListWidget(left);list->setMinimumHeight(120);list->setMaximumHeight(240);leftLayout->addWidget(list);
    auto *detail=new QWidget(splitter);auto *details=column(detail);auto *empty=helpText("尚无任务。点击左侧“添加任务”，然后填写基本信息和服务命令。",detail);details->addWidget(empty);splitter->setStretchFactor(1,1);
    auto items=std::make_shared<QJsonArray>(draft_.value("project").toObject().value("tasks").toArray());
    auto active=std::make_shared<SchemaEditor *>(nullptr);const auto taskRule=QJsonObject{{"$ref","#/$defs/task"}};
    const auto refresh=[list,items]{QSignalBlocker blocker(list);list->clear();for(const auto &value:*items)list->addItem(value.toObject().value("name").toString());};refresh();
    connect(list,&QListWidget::currentRowChanged,this,[this,detail,details,items,active,taskRule,list,empty](int row){
        if(*active){editControls_.removeAll(*active);details->removeWidget(*active);(*active)->deleteLater();*active=nullptr;}
        empty->setVisible(row<0||row>=items->size());if(row<0||row>=items->size())return;
        *active=new SchemaEditor(schema_,taskRule,(*items)[row],detail,[this](const QString &text){return resolvePathForPreview(text);});details->addWidget(*active);editControls_.append(*active);(*active)->setEnabled(runtime_.editable());
        connect(*active,&SchemaEditor::changed,this,[this,items,active,list,row]{(*items)[row]=(*active)->value();list->item(row)->setText((*items)[row].toObject().value("name").toString());setField("tasks",*items);});
    });
    editControls_.append(button("添加任务",leftLayout,[this,items,list,refresh,taskRule]{auto value=SchemaEditor::initialValue(schema_,taskRule).toObject();value["name"]="新任务";value["order"]=int(items->size()+1)*10;auto service=value.value("serviceCommand").toObject();service["timeoutMs"]=0;value["serviceCommand"]=service;items->append(value);refresh();list->setCurrentRow(int(items->size())-1);setField("tasks",*items);}));
    editControls_.append(button("删除任务",leftLayout,[this,items,list,refresh]{const auto row=list->currentRow();if(row<0)return;items->removeAt(row);list->setCurrentRow(-1);refresh();list->setCurrentRow(items->isEmpty()?-1:qMin(row,int(items->size())-1));setField("tasks",*items);}));
    if(!items->isEmpty())list->setCurrentRow(0);
}
void AdminPage::rebuild(){
    const auto selected=qMax(0,navigation_->currentRow());
    editControls_.clear();logView_=nullptr;search_=nullptr;taskFilter_=nullptr;issues_=nullptr;credentialStatus_=nullptr;testAuthButton_=nullptr;syncButton_=nullptr;
    while(pages_->count()){auto *old=pages_->widget(0);pages_->removeWidget(old);delete old;}
    for(int i=0;i<8;++i){auto *scroll=new QScrollArea(pages_);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);auto *card=new QWidget;card->setObjectName("contentCard");scroll->setWidget(card);pages_->addWidget(scroll);}
    const auto project=draft_.value("project").toObject();

    auto *overview=column(page(0));
    if(draft_.isEmpty()){
        section(overview,"还没有可用项目","创建新项目或导入已有的 CST JSON 配置。创建后请按左侧顺序填写代码、任务、端口与用户入口。");
        auto *emptyNotice=new QLabel("当前未选择项目。完成后回到“项目运行”即可启动。",page(0));emptyNotice->setProperty("role","notice");emptyNotice->setWordWrap(true);overview->addWidget(emptyNotice);
        editControls_.append(button("创建项目",overview,[this]{createProject();}));
        editControls_.append(button("导入 JSON",overview,[this]{importProject();}));
        overview->addStretch();
    }else{
        section(overview,"项目资料","名称与说明会显示在用户运行页。启动前请完成任务、代码目录和端口配置。");
        for(const auto &field:{"name","description"}){overview->addWidget(new QLabel(fieldLabel(field),page(0)));overview->addWidget(fieldEditor(field,page(0)));}
        overview->addWidget(new QLabel("项目 ID："+project.value("id").toString(),page(0)));
        section(overview,"启动与端口回收","一般情况下保留默认值即可。端口释放超时后，项目启动会报告失败。");overview->addWidget(fieldEditor("settings",page(0)));
        editControls_.append(button("设为默认项目",overview,[this]{runtime_.setDefault(draft_.value("project").toObject().value("id").toString());}));
        overview->addStretch();
    }
    buildTasks(page(1));

    auto *portsLayout=column(page(2));section(portsLayout,"所需端口","添加协议、监听地址、端口号和所属任务。选中条目后可以编辑或调整顺序。");
    portsLayout->addWidget(fieldEditor("requiredPorts",page(2)));
    editControls_.append(button("立即检测",portsLayout,[this]{const auto ports=PortReclaimService::requirements(draft_.value("project").toObject().value("requiredPorts").toArray());
        runtime_.maintenance([this,ports](const Cancellation &cancel){QStringList report;for(const auto &port:ports){cancel.check();const auto owners=ports_.owners(port);if(owners.isEmpty())report.append(QString::number(port.port)+"：空闲");for(const auto &owner:owners)report.append(QString::number(port.port)+" PID="+QString::number(owner.pid)+" "+owner.imagePath);}const auto text=report.join('\n');QMetaObject::invokeMethod(this,[this,text]{QMessageBox::information(this,"端口检测",text.isEmpty()?"未配置端口":text);},Qt::QueuedConnection);});}));
    portsLayout->addStretch();

    auto *actions=column(page(3));section(actions,"快捷入口","添加入口名称、URL 和可用条件。保存配置后，用户页会更新。");
    auto *actionsEditor=fieldEditor("userActions",page(3));actions->addWidget(actionsEditor);
    section(actions,"预览","仅显示最终布局；实际按钮由用户页在保存后生成。");
    auto *preview=new QWidget(page(3));auto *previewGrid=new QGridLayout(preview);previewGrid->setContentsMargins(0,0,0,0);previewGrid->setSpacing(8);previewGrid->setColumnStretch(0,1);previewGrid->setColumnStretch(1,1);actions->addWidget(preview);
    const auto refreshPreview=[this,preview,previewGrid]{
        while(auto *item=previewGrid->takeAt(0)){delete item->widget();delete item;}
        QList<QJsonObject> entries;for(const auto &value:draft_.value("project").toObject().value("userActions").toArray())entries.append(value.toObject());
        std::sort(entries.begin(),entries.end(),[](const QJsonObject &a,const QJsonObject &b){return a.value("order").toInt()<b.value("order").toInt();});
        if(entries.isEmpty()){auto *empty=helpText("尚未配置快捷入口。",preview);previewGrid->addWidget(empty,0,0,1,2);return;}
        int index=0;for(const auto &entry:entries){auto *label=new QLabel(entry.value("label").toString()+(entry.value("availableWhen").toString()=="running"?QString(" · 运行后可用"):QString()),preview);label->setProperty("role","previewButton");label->setAlignment(Qt::AlignCenter);label->setWordWrap(true);label->setToolTip(entry.value("url").toString());previewGrid->addWidget(label,index/2,index%2);++index;}
    };
    connect(actionsEditor,&SchemaEditor::changed,preview,[refreshPreview]{refreshPreview();});refreshPreview();
    actions->addStretch();

    auto *sourceLayout=column(page(4));auto *warning=new QLabel("同步会以远程分支替换整个本地项目目录，本地修改和生成文件不会保留。",page(4));warning->setProperty("role","notice");warning->setWordWrap(true);sourceLayout->addWidget(warning);
    sourceLayout->addWidget(fieldEditor("source",page(4)));
    credentialStatus_=new QLabel(page(4));credentialStatus_->setObjectName("credentialStatus");credentialStatus_->setWordWrap(true);sourceLayout->addWidget(credentialStatus_);
    testAuthButton_=button("测试认证",sourceLayout,[this]{if(modified_){emit error("请先保存配置");return;}const auto value=request();runtime_.maintenance([this,value](const Cancellation &cancel){sync_.testAuthentication(value,cancel);});});editControls_.append(testAuthButton_);
    syncButton_=button("同步代码",sourceLayout,[this]{if(modified_){emit error("请先保存配置，再同步代码。");return;}runtime_.synchronize();});editControls_.append(syncButton_);
    auto *head=new QLabel("最近 HEAD：尚未同步",page(4));sourceLayout->addWidget(head);sourceLayout->addStretch();connect(&runtime_,&ProjectRuntimeService::headChanged,head,[head](const QString &value){head->setText("最近 HEAD："+value);});

    auto *environment=column(page(5));section(environment,"工具与环境","按顺序查找可执行文件。环境文件可包含 KEY=VALUE，并在任务的环境页中引用。");environment->addWidget(fieldEditor("toolDirectories",page(5)));
    editControls_.append(button("编辑环境文件",environment,[this]{editEnv();}));
    section(environment,"Git 仓库认证","填写仓库账号及访问令牌，然后保存凭据；再到代码同步页测试认证。");
    auto *username=new QLineEdit(page(5));username->setPlaceholderText("Git 仓库用户名");username->setAccessibleName("Git 用户名");auto *password=new QLineEdit(page(5));password->setPlaceholderText("粘贴访问令牌（PAT），保存后自动清空");password->setEchoMode(QLineEdit::Password);password->setAccessibleName("Git PAT");
    auto *credentialForm=new QFormLayout;credentialForm->setRowWrapPolicy(QFormLayout::WrapAllRows);credentialForm->setVerticalSpacing(8);credentialForm->addRow("用户名",username);credentialForm->addRow("PAT",password);environment->addLayout(credentialForm);editControls_.append(username);editControls_.append(password);
    editControls_.append(button("保存凭据",environment,[this,username,password]{if(username->text().trimmed().isEmpty()||password->text().isEmpty()){emit error("请填写 Git 用户名和访问令牌，再保存凭据。");return;}const auto target=request().credentialTarget;const Credential value{username->text(),password->text()};logsService_.addSecret(value.password);password->clear();runtime_.maintenance([this,target,value](const Cancellation &cancel){cancel.check();credentials_.write(target,value);});}));
    editControls_.append(button("删除凭据",environment,[this]{const auto target=request().credentialTarget;runtime_.maintenance([this,target](const Cancellation &cancel){cancel.check();credentials_.remove(target);});}));environment->addStretch();

    auto *logging=column(page(6));auto *filter=new QHBoxLayout;logging->addLayout(filter);taskFilter_=new QComboBox(page(6));taskFilter_->addItem("全部任务","");for(const auto &value:project.value("tasks").toArray()){const auto task=value.toObject();taskFilter_->addItem(task.value("name").toString(),task.value("id").toString());}filter->addWidget(taskFilter_);
    search_=new QLineEdit(page(6));search_->setClearButtonEnabled(true);search_->setPlaceholderText("搜索日志（支持任务、事件、输出内容）");search_->setAccessibleName("搜索日志");filter->addWidget(search_,1);
    logView_=new QPlainTextEdit(page(6));logView_->setObjectName("logView");logView_->setProperty("role","code");logView_->setPlaceholderText("暂无日志。启动项目或执行维护操作后，日志会显示在这里。");logView_->setMinimumHeight(200);logView_->setReadOnly(true);logView_->setMaximumBlockCount(5000);logging->addWidget(logView_,1);
    connect(search_,&QLineEdit::textChanged,this,&AdminPage::renderLogs);connect(taskFilter_,&QComboBox::currentIndexChanged,this,&AdminPage::renderLogs);
    button("复制日志",logging,[this]{QApplication::clipboard()->setText(logView_->textCursor().hasSelection()?logView_->textCursor().selectedText():logView_->toPlainText());});
    button("打开日志目录",logging,[this]{const auto id=draft_.value("project").toObject().value("id").toString();if(!QDesktopServices::openUrl(QUrl::fromLocalFile(paths_.logDirectory(id))))emit error("无法打开日志目录");});
    editControls_.append(button("导出诊断包",logging,[this]{const auto path=QFileDialog::getSaveFileName(this,"导出诊断包",{},"ZIP (*.zip)");if(path.isEmpty())return;const auto config=runtime_.currentProject();const auto tasks=runtime_.taskSnapshot();const auto operation=runtime_.operationId();runtime_.maintenance([this,config,tasks,path,operation](const Cancellation &cancel){diagnostics_.exportZip(config,tasks,path,operation,cancel);});}));renderLogs();

    auto *configLayout=column(page(7));section(configLayout,"创建与备份","首次使用可导入已有 JSON，或创建项目后逐页填写配置。");
    editControls_.append(button("创建项目",configLayout,[this]{createProject();}));
    editControls_.append(button("导入 JSON",configLayout,[this]{importProject();}));
    editControls_.append(button("导出 JSON",configLayout,[this]{const auto path=QFileDialog::getSaveFileName(this,"导出项目",{},"JSON (*.json)");if(!path.isEmpty())runtime_.exportProject(path);}));
    section(configLayout,"项目切换","切换前请保存或撤销修改。删除配置不会删除项目源代码。");
    auto *projects=new QComboBox(page(7));projects->setAccessibleName("切换项目");
    issues_=new QLabel("配置验证结果将在保存或校验后显示。",page(7));issues_->setObjectName("validationIssues");issues_->setWordWrap(true);issues_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    try{for(const auto &entry:catalog_.entries())projects->addItem(entry.name,entry.id);}catch(const std::exception &e){issues_->setText(QString::fromUtf8(e.what()));}
    projects->setCurrentIndex(projects->findData(project.value("id").toString()));configLayout->addWidget(projects);editControls_.append(projects);
    editControls_.append(button("切换到所选项目",configLayout,[this,projects]{if(projects->currentIndex()>=0)runtime_.switchProject(projects->currentData().toString());}));
    editControls_.append(button("所选项目设为默认",configLayout,[this,projects]{if(projects->currentIndex()>=0)runtime_.setDefault(projects->currentData().toString());}));
    editControls_.append(button("删除所选项目配置",configLayout,[this,projects]{if(modified_){emit error("请先保存或放弃编辑");return;}if(projects->currentIndex()>=0&&QMessageBox::question(this,"删除项目配置","确定删除所选项目的配置？此操作无法撤销，源代码目录会保留。",QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)==QMessageBox::Yes)runtime_.removeProject(projects->currentData().toString());}));
    section(configLayout,"配置检查","保存前会自动校验；也可手动查看当前草稿的问题。");configLayout->addWidget(issues_);
    editControls_.append(button("校验当前配置",configLayout,[this]{const auto issues=configuration_.validate(draft_);issues_->setText(issues.isEmpty()?"配置有效":issuesText(issues));}));
    configLayout->addStretch();
    pages_->setCurrentIndex(selected);updateState();
}
void AdminPage::showPage(int index){if(index>=0&&index<navigation_->count())navigation_->setCurrentRow(index);}
void AdminPage::createProject(){
    if(modified_){emit error("请先保存或放弃当前修改");return;}
    bool ok=false;const auto name=QInputDialog::getText(this,"创建项目","项目名称",QLineEdit::Normal,{},&ok).trimmed();if(!ok||name.isEmpty())return;
    draft_=configuration_.create(name);
    newProject_=true;dirty();rebuild();navigation_->setCurrentRow(0);
}
void AdminPage::importProject(){
    if(modified_){emit error("请先保存或放弃当前修改");return;}
    const auto path=QFileDialog::getOpenFileName(this,"导入项目",{},"JSON (*.json)");if(path.isEmpty())return;
    try{runtime_.importProject(configuration_.load(path));}catch(const std::exception &e){showConfigurationProblem(QString::fromUtf8(e.what()));}
}
void AdminPage::refreshCredentialStatus(){
    if(!credentialStatus_)return;
    if(draft_.isEmpty()){credentialStatus_->setText("凭据：选择项目后可用");return;}
    const auto target=request().credentialTarget;
    if(target.isEmpty()){credentialStatus_->setText("凭据：保存项目后自动生成名称");return;}
    try{credentialStatus_->setText(credentials_.read(target)?"凭据：已保存":"凭据：未保存");}
    catch(const std::exception &e){credentialStatus_->setText(QString::fromUtf8(e.what()));}
}
QString AdminPage::resolvePathForPreview(const QString &text) const{
    const auto project=draft_.value("project").toObject();const auto id=project.value("id").toString();const auto source=project.value("source").toObject().value("workingDirectory").toString();
    try{return expandPlaceholders(text,{{"PROJECT_DIR",source},{"DATA_DIR",paths_.dataDirectory(id)},{"LOG_DIR",paths_.logDirectory(id)}});}catch(...){return text;}
}
void AdminPage::normalizeDraftPaths(){
    if(draft_.isEmpty())return;
    auto project=draft_.value("project").toObject();
    auto source=project.value("source").toObject();source["workingDirectory"]=normalizeWindowsPathInput(source.value("workingDirectory").toString());source["gitExecutable"]=normalizeWindowsPathInput(source.value("gitExecutable").toString());project["source"]=source;
    QJsonArray tools;for(const auto &value:project.value("toolDirectories").toArray())tools.append(normalizeWindowsPathInput(value.toString()));project["toolDirectories"]=tools;
    QJsonArray tasks=project.value("tasks").toArray();
    for(qsizetype i=0;i<tasks.size();++i){
        auto task=tasks[i].toObject();const auto taskDirectory=normalizeWindowsPathInput(task.value("workingDirectory").toString());
        auto env=task.value("environment").toObject();QJsonArray envFiles;for(const auto &value:env.value("envFiles").toArray())envFiles.append(normalizeWindowsPathInput(value.toString()));env["envFiles"]=envFiles;task["environment"]=env;
        auto normalizeCommand=[taskDirectory](QJsonObject command){
            if(command.value("mode").toString()=="exec")command["program"]=normalizeWindowsPathInput(command.value("program").toString());
            const auto directory=command.value("workingDirectory").toString().trimmed();
            if(!directory.isEmpty())command["workingDirectory"]=normalizeWindowsPathInput(directory);else if(!taskDirectory.isEmpty())command["workingDirectory"]=taskDirectory;
            return command;};
        auto prepare=task.value("prepareCommands").toArray();for(qsizetype j=0;j<prepare.size();++j)prepare[j]=normalizeCommand(prepare[j].toObject());task["prepareCommands"]=prepare;
        task["serviceCommand"]=normalizeCommand(task.value("serviceCommand").toObject());task.remove("workingDirectory");
        tasks[i]=task;
    }
    project["tasks"]=tasks;draft_["project"]=project;
}
void AdminPage::showConfigurationProblem(const QString &message){navigation_->setCurrentRow(7);if(issues_)issues_->setText(message);}
void AdminPage::appendLog(const QString &task,const QString &line){const auto display=formatLogLine(line);logLines_.append({task,line});if(logLines_.size()>5000)logLines_.removeFirst();if(logView_&&(taskFilter_->currentData().toString().isEmpty()||taskFilter_->currentData().toString()==task)&&display.contains(search_->text(),Qt::CaseInsensitive))logView_->appendHtml(formatLogLineHtml(line));}
void AdminPage::renderLogs(){if(!logView_)return;logView_->clear();const auto task=taskFilter_->currentData().toString();for(const auto &line:logLines_)if((task.isEmpty()||line.first==task)&&formatLogLine(line.second).contains(search_->text(),Qt::CaseInsensitive))logView_->appendHtml(formatLogLineHtml(line.second));}
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
