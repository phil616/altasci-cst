#include "MainWindow.h"
#include "ui/UiSupport.h"
#include <QFrame>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QStatusBar>
#include <QStyle>
#include <QVBoxLayout>

namespace cst {
MainWindow::MainWindow(ProjectRuntimeService &runtime,UserPage *user,AdminPage *admin,LogService &logs,QWidget *parent)
    :QMainWindow(parent),runtime_(runtime),user_(user),admin_(admin){
    setWindowTitle("Customer Service Terminal");setWindowIcon(style()->standardIcon(QStyle::SP_ComputerIcon));setMinimumSize(1040,680);resize(1180,760);
    auto *central=new QWidget(this);setCentralWidget(central);auto *layout=new QVBoxLayout(central);layout->setContentsMargins(0,0,0,0);
    auto *headerFrame=new QFrame(central);headerFrame->setObjectName("appHeader");auto *header=new QHBoxLayout(headerFrame);header->setContentsMargins(24,16,24,16);header->setSpacing(12);layout->addWidget(headerFrame);
    auto *icon=new QLabel(headerFrame);icon->setPixmap(windowIcon().pixmap(28,28));header->addWidget(icon);
    projectName_=new QLabel("CST",headerFrame);projectName_->setProperty("role","heading");header->addWidget(projectName_,1);state_=new QLabel("■ 已停止",headerFrame);state_->setAccessibleName("项目状态");header->addWidget(state_);
    auto *group=new QButtonGroup(this);group->setExclusive(true);
    auto *userButton=new QPushButton("项目运行",central);auto *adminButton=new QPushButton("管理配置",central);userButton->setObjectName("userNavigation");adminButton->setObjectName("adminNavigation");
    for(auto *button:{userButton,adminButton}){button->setCheckable(true);button->setAutoDefault(false);button->setAccessibleName(button->text());header->addWidget(button);group->addButton(button);}
    pages_=new QStackedWidget(central);pages_->addWidget(user_);pages_->addWidget(admin_);layout->addWidget(pages_,1);userButton->setChecked(true);user_->setActive(true);
    connect(userButton,&QPushButton::clicked,this,[this]{pages_->setCurrentIndex(0);user_->setActive(true);});
    connect(adminButton,&QPushButton::clicked,this,[this]{pages_->setCurrentIndex(1);user_->setActive(false);});
    closingOverlay_=new QLabel("正在停止所有项目…",central);closingOverlay_->setAlignment(Qt::AlignCenter);closingOverlay_->setAccessibleName("正在停止所有项目");closingOverlay_->hide();layout->addWidget(closingOverlay_);
    statusBar()->addPermanentWidget(new QLabel("CST 1.0.0",this));head_=new QLabel("HEAD —",this);statusBar()->addPermanentWidget(head_);
    connect(user_,&UserPage::mainAction,this,[this]{if(runtime_.state()==ProjectState::Stopped||runtime_.state()==ProjectState::Failed)runtime_.start();else runtime_.stop();});
    connect(&runtime_,&ProjectRuntimeService::projectChanged,this,[this,adminButton](const QJsonObject &document){
        user_->setProject(document);admin_->setProject(document);projectName_->setText(document.value("project").toObject().value("name").toString());
        if(document.isEmpty())adminButton->click();
    });
    connect(&runtime_,&ProjectRuntimeService::stateChanged,this,[this](ProjectState state){
        user_->setState(state);admin_->updateState();state_->setText((state==ProjectState::Running?"● ":state==ProjectState::Failed?"⚠ ":"■ ")+displayState(state));
    });
    connect(&runtime_,&ProjectRuntimeService::availabilityChanged,admin_,&AdminPage::updateState);
    connect(&runtime_,&ProjectRuntimeService::taskChanged,user_,&UserPage::setTask);
    connect(&runtime_,&ProjectRuntimeService::headChanged,this,[this](const QString &head){head_->setText("HEAD "+head.left(10));});
    connect(&runtime_,&ProjectRuntimeService::operationFinished,this,[this,adminButton](const QString &message,bool success){
        statusBar()->showMessage(message);
        if(!success){admin_->showConfigurationProblem(message);if(runtime_.currentProject().isEmpty())adminButton->click();if(!closingOverlay_->isVisible())QMessageBox::critical(this,"操作失败",message);}
    });
    const auto error=[this](const QString &message){QMessageBox::critical(this,"CST",message);};
    connect(user_,&UserPage::error,this,error);connect(admin_,&AdminPage::error,this,error);connect(&logs,&LogService::failed,this,error);
    connect(&logs,&LogService::lineWritten,this,[this](const QString &task,const QString &line){user_->appendLog(line);admin_->appendLog(task,line);});
    connect(&runtime_,&ProjectRuntimeService::closeReady,this,[this]{mayClose_=true;close();});
}
void MainWindow::activate(){if(isMinimized())showNormal();show();raise();activateWindow();}
void MainWindow::closeEvent(QCloseEvent *event){
    if(mayClose_){event->accept();return;}
    if(runtime_.editable()&&!runtime_.busy()){mayClose_=true;event->accept();return;}
    event->ignore();pages_->setEnabled(false);closingOverlay_->show();runtime_.close();
}
}
