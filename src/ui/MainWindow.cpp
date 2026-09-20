#include "MainWindow.h"
#include "ui/UiSupport.h"
#include <QButtonGroup>
#include <QCloseEvent>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QMessageBox>
#include <QResizeEvent>
#include <QStatusBar>
#include <QStyle>
#include <QVBoxLayout>
#include <QClipboard>
#include <QApplication>

namespace cst {
MainWindow::MainWindow(ProjectRuntimeService &runtime,UserPage *user,AdminPage *admin,LogService &logs,QWidget *parent)
    :QMainWindow(parent),runtime_(runtime),user_(user),admin_(admin){
    setWindowTitle("Customer Service Terminal");setWindowIcon(style()->standardIcon(QStyle::SP_ComputerIcon));setMinimumSize(900,560);resize(1180,760);
    central_=new QWidget(this);setCentralWidget(central_);auto *layout=new QVBoxLayout(central_);layout->setContentsMargins(0,0,0,0);
    auto *headerFrame=new QFrame(central_);headerFrame->setObjectName("appHeader");auto *header=new QHBoxLayout(headerFrame);header->setContentsMargins(24,16,24,16);header->setSpacing(12);layout->addWidget(headerFrame);
    auto *icon=new QLabel(headerFrame);icon->setPixmap(windowIcon().pixmap(28,28));header->addWidget(icon);
    projectName_=new QLabel("CST",headerFrame);projectName_->setProperty("role","heading");header->addWidget(projectName_,1);
    state_=new QLabel("■ 已停止",headerFrame);state_->setObjectName("projectState");state_->setAccessibleName("项目状态");state_->setStyleSheet("QLabel#projectState { background:#64748B; color:white; border-radius:10px; padding:4px 10px; }");header->addWidget(state_);
    auto *group=new QButtonGroup(this);group->setExclusive(true);
    auto *userButton=new QPushButton("项目运行",central_);auto *adminButton=new QPushButton("管理配置",central_);userButton->setObjectName("userNavigation");adminButton->setObjectName("adminNavigation");
    for(auto *button:{userButton,adminButton}){button->setCheckable(true);button->setAutoDefault(false);button->setAccessibleName(button->text());header->addWidget(button);group->addButton(button);}
    userButton->setShortcut(QKeySequence("Ctrl+1"));adminButton->setShortcut(QKeySequence("Ctrl+2"));userButton->setToolTip("项目运行（Ctrl+1）");adminButton->setToolTip("管理配置（Ctrl+2）");
    pages_=new QStackedWidget(central_);pages_->addWidget(user_);pages_->addWidget(admin_);layout->addWidget(pages_,1);userButton->setChecked(true);user_->setActive(true);
    connect(userButton,&QPushButton::clicked,this,[this]{pages_->setCurrentIndex(0);user_->setActive(true);});
    connect(adminButton,&QPushButton::clicked,this,[this]{pages_->setCurrentIndex(1);user_->setActive(false);});
    closingOverlay_=new QLabel("正在停止所有项目…",central_);closingOverlay_->setObjectName("closingOverlay");closingOverlay_->setAlignment(Qt::AlignCenter);closingOverlay_->setAccessibleName("正在停止所有项目");
    closingOverlay_->setAttribute(Qt::WA_StyledBackground,true);closingOverlay_->setStyleSheet("QLabel#closingOverlay { background: rgba(15,23,42,190); color: white; font-size: 18px; font-weight: 600; border: none; }");closingOverlay_->hide();central_->installEventFilter(this);
    statusBar()->addPermanentWidget(new QLabel("CST 1.0.0",this));head_=new QLabel("HEAD —",this);statusBar()->addPermanentWidget(head_);operation_=new QLabel("就绪",this);operation_->setObjectName("operationStatus");statusBar()->addPermanentWidget(operation_);
    connect(user_,&UserPage::mainAction,this,[this]{if(runtime_.state()==ProjectState::Stopped||runtime_.state()==ProjectState::Failed)runtime_.start();else runtime_.stop();});
    connect(admin_,&AdminPage::editingChanged,user_,&UserPage::setConfigurationDirty);
    connect(user_,&UserPage::configureRequested,this,[this,adminButton]{adminButton->click();admin_->showPage(7);});
    connect(&runtime_,&ProjectRuntimeService::projectChanged,this,[this,adminButton](const QJsonObject &document){
        for (const auto &item : consoles_) delete item.window; consoles_.clear();
        user_->setProject(document);admin_->setProject(document);projectName_->setText(document.value("project").toObject().value("name").toString());
        if(document.isEmpty())adminButton->click();
    });
    connect(&runtime_,&ProjectRuntimeService::stateChanged,this,[this](ProjectState state){
        user_->setState(state);admin_->updateState();state_->setText((state==ProjectState::Running?"● ":state==ProjectState::Failed?"⚠ ":"■ ")+displayState(state));
        const auto color=state==ProjectState::Running?"#16A34A":state==ProjectState::Failed?"#DC2626":(state==ProjectState::Stopped?"#64748B":"#D97706");
        state_->setStyleSheet(QStringLiteral("QLabel#projectState { background:")+color+QStringLiteral("; color:white; border-radius:10px; padding:4px 10px; }"));
    });
    connect(&runtime_,&ProjectRuntimeService::availabilityChanged,admin_,&AdminPage::updateState);
    connect(&runtime_,&ProjectRuntimeService::taskChanged,user_,&UserPage::setTask);
    connect(user_, &UserPage::consoleRequested, this, &MainWindow::openConsole);
    connect(&runtime_, &ProjectRuntimeService::taskChanged, this, [this](const TaskStatus &task) {
        auto &item = console(task.id); item.status->setText(task.name + " · " + task.state);
        item.view->setInputEnabled(task.state == "Preparing" || task.state == "Running" || task.state == "Ready" || task.state == "Starting");
    });
    connect(&runtime_, &ProjectRuntimeService::processOutput, this, [this](const QString &task, const ProcessOutput &output) {
        if (task.isEmpty()) { if (output.channel == "gap") for (auto &item : consoles_) item.view->feed(("\r\n[" + output.text + "]\r\n").toUtf8()); return; }
        auto &item = console(task);
        if (!output.attemptId.isEmpty() && output.attemptId != item.attempt) {
            item.attempt = output.attemptId; item.terminal = output.channel == "terminal";
            item.view->reset();
        }
        if (output.channel == "terminal") item.view->feed(output.bytes);
        else if (output.channel != "cst" && output.channel != "record") {
            auto bytes = output.text.toUtf8(); bytes.replace("\r\n", "\n"); bytes.replace("\n", "\r\n"); item.view->feed(bytes);
        }
    });
    connect(&runtime_,&ProjectRuntimeService::headChanged,this,[this](const QString &head){head_->setText("HEAD "+head.left(10));});
    connect(&runtime_,&ProjectRuntimeService::operationFinished,this,[this,adminButton](const QString &message,bool success){
        operation_->setText((success?"✓ ":"✗ ")+message);operation_->setStyleSheet(success?"color:#166534;padding:0 8px;":"color:#B91C1C;padding:0 8px;");
        statusBar()->showMessage(message,5000);
        if(!success){admin_->showConfigurationProblem(message);if(runtime_.currentProject().isEmpty())adminButton->click();if(!closingOverlay_->isVisible())QMessageBox::critical(this,"操作失败",message);}
    });
    const auto error=[this](const QString &message){QMessageBox::critical(this,"CST",message);};
    connect(user_,&UserPage::error,this,error);connect(admin_,&AdminPage::error,this,error);connect(&logs,&LogService::failed,this,error);
    connect(&logs,&LogService::lineWritten,this,[this](const QString &task,const QString &line){user_->appendLog(line);admin_->appendLog(task,line);});
    connect(&runtime_,&ProjectRuntimeService::closeReady,this,[this]{mayClose_=true;close();});
}
MainWindow::Console &MainWindow::console(const QString &task) {
    if (!consoles_.contains(task)) {
        auto *window = new QDialog(this, Qt::Window); window->setWindowTitle("Console · " + task); window->resize(960, 580);
        auto *layout = new QVBoxLayout(window); auto *status = new QLabel(task, window); layout->addWidget(status);
        auto *view = new TerminalView(window); layout->addWidget(view, 1);
        auto *actions = new QHBoxLayout; layout->addLayout(actions);
        auto *copy = new QPushButton("复制输出", window); actions->addWidget(copy);
        auto *paste = new QPushButton("粘贴", window); actions->addWidget(paste);
        auto *interrupt = new QPushButton("发送 Ctrl+C", window); actions->addWidget(interrupt);
        for (auto *button : {copy, paste, interrupt}) button->setAutoDefault(false);
        actions->addWidget(new QLabel("关闭此窗口不会停止任务；关闭主窗口会停止整个项目。", window), 1);
        connect(copy, &QPushButton::clicked, view, [view] { QApplication::clipboard()->setText(view->plainText()); });
        connect(paste, &QPushButton::clicked, view, &TerminalView::paste);
        connect(interrupt, &QPushButton::clicked, this, [this, task] { runtime_.writeInput(task, QByteArray(1, '\x03')); });
        connect(view, &TerminalView::input, this, [this, task](const QByteArray &bytes) { runtime_.writeInput(task, bytes); });
        connect(view, &TerminalView::terminalResized, this, [this, task](int columns, int rows) { runtime_.resizeTerminal(task, columns, rows); });
        consoles_.insert(task, {window, view, status, {}, false});
    }
    return consoles_[task];
}
void MainWindow::openConsole(const QString &task) { auto &item = console(task); item.window->show(); item.window->raise(); item.view->setFocus(); }
void MainWindow::activate(){if(isMinimized())showNormal();show();raise();activateWindow();positionClosingOverlay();}
void MainWindow::positionClosingOverlay(){if(closingOverlay_&&central_)closingOverlay_->setGeometry(central_->rect());}
void MainWindow::resizeEvent(QResizeEvent *event){QMainWindow::resizeEvent(event);positionClosingOverlay();}
bool MainWindow::eventFilter(QObject *watched,QEvent *event){if(watched==central_&&event->type()==QEvent::Resize)positionClosingOverlay();return QMainWindow::eventFilter(watched,event);}
void MainWindow::closeEvent(QCloseEvent *event){
    if(mayClose_){for(const auto &item:consoles_)item.window->hide();event->accept();return;}
    if(runtime_.editable()&&!runtime_.busy()){
        if(runtime_.editing()){
            const auto answer=QMessageBox::warning(this,"放弃未保存修改？","管理配置中有未保存修改。退出将丢失这些修改，确定退出？",QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel);
            if(answer!=QMessageBox::Yes){event->ignore();return;}
        }
        mayClose_=true;for(const auto &item:consoles_)item.window->hide();event->accept();return;
    }
    event->ignore();pages_->setEnabled(false);positionClosingOverlay();closingOverlay_->show();closingOverlay_->raise();runtime_.close();
}
}
