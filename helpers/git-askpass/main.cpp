#include "infrastructure/windows/WindowsPlatform.h"
#include <QCoreApplication>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (application.arguments().size() != 2) return 2;
    const auto prompt = application.arguments()[1];
    const auto target = qEnvironmentVariable("CST_CREDENTIAL_TARGET");
    if (!target.startsWith("CST/git/")) return 3;
    try {
        cst::WindowsCredentialStore store;
        const auto credential = store.read(target); if (!credential) return 4;
        QByteArray output;
        if (prompt.contains("Username", Qt::CaseInsensitive)) output = credential->username.toUtf8();
        else if (prompt.contains("Password", Qt::CaseInsensitive)) output = credential->password.toUtf8();
        else return 5;
        output.append('\n');
        const auto written = std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout);
        const bool ok = written == static_cast<size_t>(output.size()) && std::fflush(stdout) == 0;
        output.fill('\0');
        return ok ? 0 : 6;
    } catch (...) { std::fputs("CST credential lookup failed\n", stderr); return 7; }
}
