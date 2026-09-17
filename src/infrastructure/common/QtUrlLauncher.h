#pragma once
#include "application/Platform.h"
#include "domain/Configuration.h"
#include <QDesktopServices>
#include <QUrl>

namespace cst {
class QtUrlLauncher final : public IUrlLauncher {
public:
    void open(const QString &url) override {
        if (!isAllowedUrl(url)) throw std::invalid_argument("只允许打开 HTTP/HTTPS URL");
        if (!QDesktopServices::openUrl(QUrl(url))) throw std::runtime_error("默认浏览器无法打开链接，请检查系统默认应用设置");
    }
};
}
