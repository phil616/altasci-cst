#include "SchemaEditor.h"
#include "ui/UiSupport.h"
#include <QTabWidget>
#include <QGroupBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFontDatabase>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QUuid>
#include <limits>
#include <algorithm>

namespace cst {
QString fieldLabel(const QString &key) {
    static const QMap<QString, QString> labels{
        {"id", "标识"}, {"name", "名称"}, {"description", "说明"}, {"order", "顺序"}, {"workingDirectory", "工作目录"},
        {"environment", "环境变量"}, {"prepareCommands", "准备命令"}, {"serviceCommand", "长期服务命令"},
        {"readiness", "就绪检查"}, {"restartPolicy", "重启策略"}, {"shutdownGraceMs", "停止宽限期（毫秒）"},
        {"mode", "模式"}, {"program", "程序"}, {"arguments", "参数"}, {"script", "脚本"}, {"timeoutMs", "超时（毫秒）"},
        {"successExitCodes", "成功退出码"}, {"inheritSystem", "继承系统环境"}, {"envFiles", "环境文件"}, {"variables", "变量"},
        {"type", "类型"}, {"address", "地址"}, {"port", "端口"}, {"protocol", "协议"}, {"ownerTaskId", "所属任务"},
        {"url", "URL"}, {"expectedStatusCodes", "预期 HTTP 状态码"}, {"requestTimeoutMs", "请求超时（毫秒）"},
        {"connectTimeoutMs", "连接超时（毫秒）"}, {"pollIntervalMs", "检测间隔（毫秒）"}, {"successThreshold", "连续成功次数"},
        {"probes", "探针"}, {"maxRestarts", "最多重启次数"}, {"windowSeconds", "统计窗口（秒）"}, {"backoffSeconds", "初始退避（秒）"},
        {"maxBackoffSeconds", "最大退避（秒）"}, {"label", "按钮文字"}, {"availableWhen", "可用条件"},
        {"repositoryUrl", "HTTPS 仓库地址"}, {"branch", "分支"}, {"gitExecutable", "Git 可执行文件"},
        {"credentialTarget", "凭据名称"}, {"toolDirectories", "工具查找目录"}, {"portReclaimTimeoutMs", "端口释放超时（毫秒）"},
        {"maxAncestorEscalation", "最多上溯层数"}};
    return labels.value(key, key);
}
namespace {
QJsonObject resolve(const QJsonObject &root, QJsonObject rule) {
    if (rule.contains("$ref")) return root.value("$defs").toObject().value(rule.value("$ref").toString().mid(8)).toObject();
    return rule;
}
QString fieldHelp(const QString &key) {
    static const QMap<QString,QString> hints{
        {"id","唯一标识，用于配置引用。请使用稳定的英文、数字或短横线。"},
        {"order","数值越小越先启动；停止时按相反顺序执行。"},
        {"workingDirectory","使用绝对路径，或 {{PROJECT_DIR}} 等目录占位符。"},
        {"mode","exec 直接运行程序；shell 执行多行脚本。"},
        {"program","填写可执行文件名或完整路径。参数在下方逐项添加。"},
        {"arguments","每项代表一个参数，无需自行添加外层引号。列表顺序即传入顺序。"},
        {"timeoutMs","单位为毫秒。长期服务使用 0；准备命令必须设置正数。"},
        {"repositoryUrl","填写 HTTPS Git 仓库地址；用户名和 PAT 在“环境与凭据”中保存。"},
        {"branch","同步此远程分支的最新完整代码。"},
        {"gitExecutable","本机 git.exe 的完整路径，要求 Git 2.45 或更高版本。"},
        {"credentialTarget","保存项目时自动按项目 ID 和仓库主机生成。"},
        {"envFiles","按列表顺序读取环境文件，后面的值覆盖前面的同名变量。"},
        {"variables","这里的值覆盖环境文件中的同名变量。不要在共享配置中保存密钥。"},
        {"inheritSystem","勾选后继承当前系统的环境变量。"},
        {"ownerTaskId","填写占用该端口的任务标识，需要与任务配置一致。"},
        {"availableWhen","running 表示项目运行后可用；always 表示始终可用。"},
        {"url","点击后由默认浏览器打开此地址。"},
        {"successExitCodes","命令返回这些退出码时视为成功。"},
        {"probes","所有探针通过后，任务才会被视为就绪。"},
        {"maxRestarts","统计窗口内允许的自动重启次数。"},
        {"shutdownGraceMs","先请求正常退出，超过此时间后终止进程树。"}
    };
    return hints.value(key);
}
QString summary(const QJsonValue &value) {
    if (value.isObject()) {
        const auto object = value.toObject();
        if(object.contains("port"))return object.value("protocol").toString().toUpper()+"  "+object.value("address").toString()+":"+QString::number(object.value("port").toInt())+"  →  "+object.value("ownerTaskId").toString();
        if(object.contains("label"))return object.value("label").toString()+"  ·  "+object.value("url").toString();
        for (const auto &field : {"name", "label", "id", "url", "address"}) if (object.contains(field)) return object.value(field).toString();
        return "设置（" + QString::number(object.size()) + " 项）";
    }
    if (value.isArray()) return QString::number(value.toArray().size()) + " 项";
    if (value.isDouble()) return QString::number(value.toDouble());
    return value.toString();
}
std::optional<QJsonValue> editDialog(const QJsonObject &schema, const QJsonObject &rule, const QJsonValue &value, QWidget *parent) {
    QDialog dialog(parent); dialog.setWindowTitle("编辑设置"); dialog.resize(760, 640);
    auto *layout = new QVBoxLayout(&dialog); layout->setContentsMargins(24,24,24,24); layout->setSpacing(16); auto *scroll = new QScrollArea(&dialog); scroll->setWidgetResizable(true);
    auto *editor = new SchemaEditor(schema, rule, value); editor->setContentsMargins(16,16,16,16); scroll->setWidget(editor); layout->addWidget(scroll);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) return editor->value();
    return std::nullopt;
}
}
SchemaEditor::SchemaEditor(QJsonObject schema, QJsonObject rule, QJsonValue value, QWidget *parent)
    : QWidget(parent), schema_(std::move(schema)), rule_(std::move(rule)) { build(rule_, std::move(value)); }
QJsonValue SchemaEditor::value() const { return read_(); }
QJsonObject SchemaEditor::resolved(QJsonObject rule) const { return resolve(schema_, std::move(rule)); }
QJsonValue SchemaEditor::initialValue(const QJsonObject &root, QJsonObject rule) {
    rule = resolve(root, rule);
    if (rule.contains("const")) return rule.value("const");
    if (rule.contains("enum")) return rule.value("enum").toArray().first();
    if (rule.contains("oneOf")) return initialValue(root, rule.value("oneOf").toArray().first().toObject());
    const auto type = rule.value("type").toString();
    if (type == "object") {
        QJsonObject result; const auto properties = rule.value("properties").toObject();
        for (auto it = properties.begin(); it != properties.end(); ++it) result[it.key()] = initialValue(root, it.value().toObject());
        static const QMap<QString, int> defaults{{"maxRestarts",5},{"windowSeconds",600},{"backoffSeconds",1},{"maxBackoffSeconds",30},
            {"shutdownGraceMs",5000},{"pollIntervalMs",500},{"successThreshold",2},{"connectTimeoutMs",1000},{"requestTimeoutMs",2000}};
        for (auto it = defaults.begin(); it != defaults.end(); ++it) if (result.contains(it.key())) result[it.key()] = it.value();
        if (result.contains("mode") && result.value("mode") == "exec") { result.remove("script"); result["arguments"] = QJsonArray{}; }
        if (result.contains("successExitCodes")) result["successExitCodes"] = QJsonArray{0};
        if (result.contains("expectedStatusCodes")) result["expectedStatusCodes"] = QJsonArray{200};
        if (result.contains("id")) result["id"] = "item-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        return result;
    }
    if (type == "array") return QJsonArray{};
    if (type == "integer") return rule.value("minimum").toInt();
    if (type == "boolean") return false;
    return "";
}
void SchemaEditor::build(QJsonObject rule, QJsonValue initial) {
    rule = resolved(rule);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0); layout->setSpacing(8); layout->setAlignment(Qt::AlignTop);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    if (rule.contains("oneOf")) {
        auto *selector = new QComboBox(this); selector->setAccessibleName("探针类型");
        const auto alternatives = rule.value("oneOf").toArray(); int selected = 0;
        for (qsizetype i = 0; i < alternatives.size(); ++i) {
            const auto branch = resolved(alternatives[i].toObject());
            const auto type = branch.value("properties").toObject().value("type").toObject().value("const").toString();
            selector->addItem(type); if (initial.toObject().value("type") == type) selected = int(i);
        }
        selector->setCurrentIndex(selected); layout->addWidget(selector);
        auto current = std::make_shared<SchemaEditor *>(new SchemaEditor(schema_, alternatives[selected].toObject(), initial, this));
        layout->addWidget(*current); connect(*current, &SchemaEditor::changed, this, &SchemaEditor::changed);
        connect(selector, &QComboBox::currentIndexChanged, this, [this, layout, current, alternatives](int index) {
            layout->removeWidget(*current); (*current)->deleteLater();
            const auto branch = alternatives[index].toObject();
            *current = new SchemaEditor(schema_, branch, initialValue(schema_, branch), this); layout->addWidget(*current);
            connect(*current, &SchemaEditor::changed, this, &SchemaEditor::changed); emit changed();
        });
        read_ = [current] { return (*current)->value(); }; return;
    }
    if (rule.contains("enum")) {
        auto *combo = new QComboBox(this); const auto values = rule.value("enum").toArray();
        for (const auto &value : values) combo->addItem(value.toString()); combo->setCurrentText(initial.toString());
        setFocusProxy(combo); layout->addWidget(combo); read_ = [combo] { return combo->currentText(); };
        connect(combo, &QComboBox::currentTextChanged, this, &SchemaEditor::changed); return;
    }
    if (rule.contains("const")) {
        const auto value = rule.value("const"); layout->addWidget(new QLabel(summary(value), this)); read_ = [value] { return value; }; return;
    }
    const auto type = rule.value("type").toString();
    if (type == "object" && rule.value("properties").toObject().contains("serviceCommand") && rule.value("properties").toObject().contains("readiness")) {
        auto *tabs=new QTabWidget(this);tabs->setObjectName("taskEditorTabs");tabs->setMinimumHeight(360);layout->addWidget(tabs);
        const auto properties=rule.value("properties").toObject();
        const QList<QPair<QString,QStringList>> groups{
            {"基本信息",{"id","name","order","workingDirectory"}},
            {"命令",{"serviceCommand","prepareCommands"}},
            {"环境",{"environment"}},
            {"健康检查",{"readiness"}},
            {"重启与停止",{"restartPolicy","shutdownGraceMs"}}};
        auto editors=std::make_shared<QList<SchemaEditor *>>();
        for(const auto &group:groups){
            QJsonObject fields;for(const auto &key:group.second)if(properties.contains(key))fields[key]=properties[key];
            auto *scroll=new QScrollArea(tabs);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
            auto *editor=new SchemaEditor(schema_,{{"type","object"},{"properties",fields}},initial,this);editor->setContentsMargins(16,16,16,16);
            scroll->setWidget(editor);tabs->addTab(scroll,group.first);editors->append(editor);connect(editor,&SchemaEditor::changed,this,&SchemaEditor::changed);
        }
        read_=[editors]{QJsonObject result;for(auto *editor:*editors){const auto fields=editor->value().toObject();for(auto it=fields.begin();it!=fields.end();++it)result[it.key()]=it.value();}return result;};return;
    }
    if (type == "object" && rule.value("properties").toObject().isEmpty()) {
        auto *table = new QTableWidget(this); table->setMinimumHeight(144); table->setMaximumHeight(240); table->setColumnCount(2); table->setHorizontalHeaderLabels({"变量名", "值"}); table->horizontalHeader()->setStretchLastSection(true);
        const auto object = initial.toObject(); table->setRowCount(int(object.size())); int row = 0;
        for (auto it = object.begin(); it != object.end(); ++it, ++row) { table->setItem(row,0,new QTableWidgetItem(it.key())); table->setItem(row,1,new QTableWidgetItem(it.value().toString())); }
        layout->addWidget(table); auto *buttons = new QHBoxLayout; layout->addLayout(buttons);
        auto *add = new QPushButton("添加变量",this); auto *remove = new QPushButton("删除变量",this); buttons->addWidget(add); buttons->addWidget(remove); buttons->addStretch();
        connect(add,&QPushButton::clicked,this,[this,table] { const auto row = table->rowCount(); table->insertRow(row); table->setItem(row,0,new QTableWidgetItem("NEW_VARIABLE")); table->setItem(row,1,new QTableWidgetItem("")); emit changed(); });
        connect(remove,&QPushButton::clicked,this,[this,table] { table->removeRow(table->currentRow()); emit changed(); });
        connect(table,&QTableWidget::itemChanged,this,&SchemaEditor::changed);
        read_ = [table] { QJsonObject result; for(int i=0;i<table->rowCount();++i) if(table->item(i,0)) result[table->item(i,0)->text()] = table->item(i,1) ? table->item(i,1)->text() : QString{}; return result; }; return;
    }
    if (type == "object") {
        auto *form = new QFormLayout; form->setVerticalSpacing(16); form->setHorizontalSpacing(20); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); form->setRowWrapPolicy(QFormLayout::WrapLongRows); form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop); layout->addLayout(form);
        auto editors = std::make_shared<QMap<QString, SchemaEditor *>>(); const auto properties = rule.value("properties").toObject();
        const QStringList preferredOrder{"id","name","description","order","mode","program","arguments","script","workingDirectory","repositoryUrl","branch","gitExecutable","credentialTarget","timeoutMs","successExitCodes","serviceCommand","prepareCommands","environment","inheritSystem","envFiles","variables","readiness","type","address","port","url","restartPolicy","shutdownGraceMs"};
        auto keys = properties.keys();
        std::stable_sort(keys.begin(), keys.end(), [&preferredOrder](const QString &a, const QString &b) {
            const auto rank = [&preferredOrder](const QString &key) { const auto index = preferredOrder.indexOf(key); return index < 0 ? preferredOrder.size() : index; };
            return rank(a) < rank(b);
        });
        for (const auto &key : keys) {
            auto fieldRule = properties.value(key).toObject();
            if (key == "script" || key == "description") fieldRule["uiMultiline"] = key;
            auto *editor = new SchemaEditor(schema_, fieldRule, initial.toObject().value(key), this);
            editor->setAccessibleName(fieldLabel(key)); editor->setObjectName(key);
            const auto resolvedRule = resolved(fieldRule);
            if (resolvedRule.value("type") == "object" || resolvedRule.contains("oneOf")) {
                auto *group=new QGroupBox(fieldLabel(key),this);auto *groupLayout=new QVBoxLayout(group);groupLayout->setSpacing(12);groupLayout->addWidget(editor);form->addRow(group);
            } else {
                auto *label=new QLabel(fieldLabel(key),this);label->setWordWrap(true);label->setFixedWidth(132);label->setContentsMargins(0,8,0,0);label->setBuddy(editor);form->addRow(label, editor);
            }
            const auto hint=key=="workingDirectory"&&properties.contains("repositoryUrl")?QString("同步代码的本地绝对路径。此目录的内容会被远程代码替换。"):fieldHelp(key);
            if(!hint.isEmpty()) {editor->setToolTip(hint);editor->layout()->addWidget(helpText(hint,editor));}
            if(key=="credentialTarget")if(auto *line=editor->findChild<QLineEdit *>())line->setReadOnly(true);
            editors->insert(key,editor); connect(editor,&SchemaEditor::changed,this,&SchemaEditor::changed);
        }
        auto applyMode = [editors,form] {
            if (!editors->contains("mode") || !editors->contains("program")) return;
            const bool exec = editors->value("mode")->value() == "exec";
            form->setRowVisible(editors->value("program"), exec); form->setRowVisible(editors->value("arguments"), exec); form->setRowVisible(editors->value("script"), !exec);
        };
        if (editors->contains("mode")) connect(editors->value("mode"),&SchemaEditor::changed,this,applyMode); applyMode();
        read_ = [editors] {
            QJsonObject result; for(auto it=editors->begin();it!=editors->end();++it) result[it.key()]=it.value()->value();
            if (result.contains("mode") && result.contains("program")) {
                if (result.value("mode") == "exec") result.remove("script"); else { result.remove("program"); result.remove("arguments"); }
            }
            return result;
        }; return;
    }
    if (type == "array") {
        auto items = std::make_shared<QJsonArray>(initial.toArray());
        auto *list = new QListWidget(this); list->setFixedHeight(128); layout->addWidget(list);
        const auto refresh = [list,items] { const auto row = list->currentRow(); list->clear(); for(const auto &item:*items) list->addItem(summary(item)); if(!items->isEmpty()) list->setCurrentRow(qBound(0,row,int(items->size())-1)); };
        auto *empty=helpText("暂无条目。点击“添加”创建第一项。",this);layout->addWidget(empty);
        const auto updateEmpty=[empty,items]{empty->setVisible(items->isEmpty());};
        connect(this,&SchemaEditor::changed,this,updateEmpty);updateEmpty();
        refresh(); auto *buttons = new QHBoxLayout; layout->addLayout(buttons);
        auto *add = new QPushButton("添加",this); auto *edit = new QPushButton("编辑",this); auto *remove = new QPushButton("删除",this); auto *up = new QPushButton("上移",this); auto *down = new QPushButton("下移",this);
        for(auto *button:{add,edit,remove,up,down}) buttons->addWidget(button);
        buttons->addStretch();
        const auto updateButtons=[list,edit,remove,up,down]{const auto row=list->currentRow();edit->setEnabled(row>=0);remove->setEnabled(row>=0);up->setEnabled(row>0);down->setEnabled(row>=0&&row+1<list->count());};
        connect(list,&QListWidget::currentRowChanged,this,updateButtons);updateButtons();
        const auto itemRule = rule.value("items").toObject();
        connect(add,&QPushButton::clicked,this,[this,items,itemRule,refresh] { if(auto value=editDialog(schema_,itemRule,initialValue(schema_,itemRule),this)) { items->append(*value); refresh(); emit changed(); } });
        const auto editItem = [this,list,items,itemRule,refresh] { const auto row=list->currentRow(); if(row<0)return; if(auto value=editDialog(schema_,itemRule,(*items)[row],this)) { (*items)[row]=*value; refresh(); emit changed(); } };
        connect(edit,&QPushButton::clicked,this,editItem); connect(list,&QListWidget::itemDoubleClicked,this,editItem);
        connect(remove,&QPushButton::clicked,this,[this,list,items,refresh] { if(list->currentRow()<0)return; items->removeAt(list->currentRow()); refresh(); emit changed(); });
        const auto move = [this,list,items,refresh](int delta) { const auto row=list->currentRow(); const auto destination=row+delta; if(row<0||destination<0||destination>=items->size())return; const auto item=items->takeAt(row); items->insert(destination,item); refresh(); list->setCurrentRow(destination); emit changed(); };
        connect(up,&QPushButton::clicked,this,[move]{move(-1);}); connect(down,&QPushButton::clicked,this,[move]{move(1);});
        read_ = [items]{return *items;}; return;
    }
    if (type == "boolean") {
        auto *box = new QCheckBox("启用",this); box->setChecked(initial.toBool()); layout->addWidget(box); read_=[box]{return box->isChecked();}; connect(box,&QCheckBox::toggled,this,&SchemaEditor::changed); return;
    }
    if (type == "integer") {
        auto *spin = new QSpinBox(this); spin->setMaximumWidth(220); spin->setRange(rule.value("minimum").toInt(std::numeric_limits<int>::min()),rule.value("maximum").toInt(std::numeric_limits<int>::max())); spin->setValue(initial.toInt());
        setFocusProxy(spin); layout->addWidget(spin); read_=[spin]{return spin->value();}; connect(spin,&QSpinBox::valueChanged,this,&SchemaEditor::changed); return;
    }
    if (rule.contains("uiMultiline")) {
        auto *text = new QPlainTextEdit(initial.toString(), this);
        const bool script = rule.value("uiMultiline") == "script";
        text->setFixedHeight(script ? 180 : 104);
        text->setPlaceholderText(script ? "输入要执行的脚本，可包含多行命令" : "简要说明项目的用途");
        if (script) { text->setProperty("role", "code"); text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); text->setLineWrapMode(QPlainTextEdit::NoWrap); }
        setFocusProxy(text); layout->addWidget(text); read_ = [text] { return text->toPlainText(); };
        connect(text, &QPlainTextEdit::textChanged, this, &SchemaEditor::changed); return;
    }
    auto *line = new QLineEdit(initial.toString(),this); line->setClearButtonEnabled(true); if(rule.contains("maxLength")) line->setMaxLength(rule.value("maxLength").toInt());
    setFocusProxy(line); layout->addWidget(line); read_=[line]{return line->text();}; connect(line,&QLineEdit::textChanged,this,&SchemaEditor::changed);
}
}
