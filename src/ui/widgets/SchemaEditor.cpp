#include "SchemaEditor.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
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
QString summary(const QJsonValue &value) {
    if (value.isObject()) {
        const auto object = value.toObject();
        for (const auto &field : {"name", "label", "id", "url", "address"}) if (object.contains(field)) return object.value(field).toString();
        return "设置（" + QString::number(object.size()) + " 项）";
    }
    if (value.isArray()) return QString::number(value.toArray().size()) + " 项";
    if (value.isDouble()) return QString::number(value.toDouble());
    return value.toString();
}
std::optional<QJsonValue> editDialog(const QJsonObject &schema, const QJsonObject &rule, const QJsonValue &value, QWidget *parent) {
    QDialog dialog(parent); dialog.setWindowTitle("编辑设置"); dialog.resize(680, 560);
    auto *layout = new QVBoxLayout(&dialog); auto *scroll = new QScrollArea(&dialog); scroll->setWidgetResizable(true);
    auto *editor = new SchemaEditor(schema, rule, value); scroll->setWidget(editor); layout->addWidget(scroll);
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
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0); layout->setSpacing(12);
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
        layout->addWidget(combo); read_ = [combo] { return combo->currentText(); };
        connect(combo, &QComboBox::currentTextChanged, this, &SchemaEditor::changed); return;
    }
    if (rule.contains("const")) {
        const auto value = rule.value("const"); layout->addWidget(new QLabel(summary(value), this)); read_ = [value] { return value; }; return;
    }
    const auto type = rule.value("type").toString();
    if (type == "object" && rule.value("properties").toObject().isEmpty()) {
        auto *table = new QTableWidget(this); table->setColumnCount(2); table->setHorizontalHeaderLabels({"变量名", "值"}); table->horizontalHeader()->setStretchLastSection(true);
        const auto object = initial.toObject(); table->setRowCount(int(object.size())); int row = 0;
        for (auto it = object.begin(); it != object.end(); ++it, ++row) { table->setItem(row,0,new QTableWidgetItem(it.key())); table->setItem(row,1,new QTableWidgetItem(it.value().toString())); }
        layout->addWidget(table); auto *buttons = new QHBoxLayout; layout->addLayout(buttons);
        auto *add = new QPushButton("添加变量",this); auto *remove = new QPushButton("删除变量",this); buttons->addWidget(add); buttons->addWidget(remove);
        connect(add,&QPushButton::clicked,this,[this,table] { const auto row = table->rowCount(); table->insertRow(row); table->setItem(row,0,new QTableWidgetItem("NEW_VARIABLE")); table->setItem(row,1,new QTableWidgetItem("")); emit changed(); });
        connect(remove,&QPushButton::clicked,this,[this,table] { table->removeRow(table->currentRow()); emit changed(); });
        connect(table,&QTableWidget::itemChanged,this,&SchemaEditor::changed);
        read_ = [table] { QJsonObject result; for(int i=0;i<table->rowCount();++i) if(table->item(i,0)) result[table->item(i,0)->text()] = table->item(i,1) ? table->item(i,1)->text() : QString{}; return result; }; return;
    }
    if (type == "object") {
        auto *form = new QFormLayout; form->setVerticalSpacing(12); layout->addLayout(form);
        auto editors = std::make_shared<QMap<QString, SchemaEditor *>>(); const auto properties = rule.value("properties").toObject();
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            auto *editor = new SchemaEditor(schema_, it.value().toObject(), initial.toObject().value(it.key()), this);
            editor->setAccessibleName(fieldLabel(it.key())); editor->setObjectName(it.key());
            form->addRow(fieldLabel(it.key()), editor); editors->insert(it.key(),editor); connect(editor,&SchemaEditor::changed,this,&SchemaEditor::changed);
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
        auto data = std::make_shared<QJsonArray>(initial.toArray());
        auto *list = new QListWidget(this); list->setMinimumHeight(100); layout->addWidget(list);
        const auto refresh = [list,data] { const auto row = list->currentRow(); list->clear(); for(const auto &item:*data) list->addItem(summary(item)); if(!data->isEmpty()) list->setCurrentRow(qBound(0,row,int(data->size())-1)); };
        refresh(); auto *buttons = new QHBoxLayout; layout->addLayout(buttons);
        auto *add = new QPushButton("添加",this); auto *edit = new QPushButton("编辑",this); auto *remove = new QPushButton("删除",this); auto *up = new QPushButton("上移",this); auto *down = new QPushButton("下移",this);
        for(auto *button:{add,edit,remove,up,down}) buttons->addWidget(button);
        const auto itemRule = rule.value("items").toObject();
        connect(add,&QPushButton::clicked,this,[this,data,itemRule,refresh] { if(auto value=editDialog(schema_,itemRule,initialValue(schema_,itemRule),this)) { data->append(*value); refresh(); emit changed(); } });
        const auto editItem = [this,list,data,itemRule,refresh] { const auto row=list->currentRow(); if(row<0)return; if(auto value=editDialog(schema_,itemRule,(*data)[row],this)) { (*data)[row]=*value; refresh(); emit changed(); } };
        connect(edit,&QPushButton::clicked,this,editItem); connect(list,&QListWidget::itemDoubleClicked,this,editItem);
        connect(remove,&QPushButton::clicked,this,[this,list,data,refresh] { if(list->currentRow()<0)return; data->removeAt(list->currentRow()); refresh(); emit changed(); });
        const auto move = [this,list,data,refresh](int delta) { const auto row=list->currentRow(); const auto destination=row+delta; if(row<0||destination<0||destination>=data->size())return; const auto item=data->takeAt(row); data->insert(destination,item); refresh(); list->setCurrentRow(destination); emit changed(); };
        connect(up,&QPushButton::clicked,this,[move]{move(-1);}); connect(down,&QPushButton::clicked,this,[move]{move(1);});
        read_ = [data]{return *data;}; return;
    }
    if (type == "boolean") {
        auto *box = new QCheckBox(this); box->setChecked(initial.toBool()); layout->addWidget(box); read_=[box]{return box->isChecked();}; connect(box,&QCheckBox::toggled,this,&SchemaEditor::changed); return;
    }
    if (type == "integer") {
        auto *spin = new QSpinBox(this); spin->setRange(rule.value("minimum").toInt(std::numeric_limits<int>::min()),rule.value("maximum").toInt(std::numeric_limits<int>::max())); spin->setValue(initial.toInt());
        layout->addWidget(spin); read_=[spin]{return spin->value();}; connect(spin,&QSpinBox::valueChanged,this,&SchemaEditor::changed); return;
    }
    auto *line = new QLineEdit(initial.toString(),this); if(rule.contains("maxLength")) line->setMaxLength(rule.value("maxLength").toInt());
    layout->addWidget(line); read_=[line]{return line->text();}; connect(line,&QLineEdit::textChanged,this,&SchemaEditor::changed);
}
}
