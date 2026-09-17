#pragma once
#include <QJsonObject>
#include <QWidget>
#include <functional>
#include <memory>
#include <optional>

namespace cst {
class SchemaEditor final : public QWidget {
    Q_OBJECT
public:
    using PathResolver = std::function<QString(const QString &)>;
    SchemaEditor(QJsonObject schema, QJsonObject rule, QJsonValue value, QWidget *parent = nullptr, PathResolver pathResolver = {});
    QJsonValue value() const;
    static QJsonValue initialValue(const QJsonObject &root, QJsonObject rule);
signals:
    void changed();
private:
    QJsonObject resolved(QJsonObject rule) const;
    void build(QJsonObject rule, QJsonValue value);
    QJsonObject schema_;
    QJsonObject rule_;
    PathResolver pathResolver_;
    std::function<QJsonValue()> read_;
};
QString fieldLabel(const QString &key);
}
