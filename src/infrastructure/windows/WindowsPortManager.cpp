#include <winsock2.h>
#include <ws2tcpip.h>
#include "WindowsPlatform.h"
#include "Win32Support.h"
#include "domain/Configuration.h"
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <winsvc.h>
#include <QFileInfo>
#include <QHostAddress>
#include <QSet>
#include <QThread>
#include <vector>

namespace cst {
namespace {
struct ProcessEntry { quint32 pid; quint32 parent; QString name; };
QList<ProcessEntry> snapshot() {
    win::Handle handle(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!handle) win::fail("CreateToolhelp32Snapshot");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    QList<ProcessEntry> result;
    if (!Process32FirstW(handle.get(), &entry)) win::fail("Process32First");
    do { result.append({entry.th32ProcessID, entry.th32ParentProcessID, QString::fromWCharArray(entry.szExeFile)}); }
    while (Process32NextW(handle.get(), &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) win::fail("Process32Next");
    return result;
}
QString imagePath(quint32 pid, bool required) {
    win::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) {
        if (required) win::fail("无法查询映像 PID=" + QString::number(pid));
        return "<无法查询映像，Win32=" + QString::number(GetLastError()) + '>';
    }
    std::vector<wchar_t> buffer(32768); DWORD count = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, buffer.data(), &count)) {
        if (required) win::fail("QueryFullProcessImageName PID=" + QString::number(pid));
        return "<无法查询映像，Win32=" + QString::number(GetLastError()) + '>';
    }
    return QString::fromWCharArray(buffer.data(), count);
}
bool protectedPid(quint32 pid, const QString &path) {
    static const QSet<QString> names{"smss.exe", "csrss.exe", "wininit.exe", "winlogon.exe", "lsass.exe"};
    return pid == 0 || pid == 4 || pid == GetCurrentProcessId() || names.contains(QFileInfo(path).fileName().toLower());
}
class ServiceHandle {
public:
    explicit ServiceHandle(SC_HANDLE value) : value_(value) {}
    ~ServiceHandle() { if (value_) CloseServiceHandle(value_); }
    SC_HANDLE get() const { return value_; }
private:
    SC_HANDLE value_;
};
template<class Table, class Fetch, class Convert>
void appendTable(QList<PortOwner> &output, Fetch fetch, Convert convert, const PortRequirement &requested,
                 const QList<ProcessEntry> &processes) {
    DWORD size = 0;
    auto result = fetch(nullptr, &size);
    if (result != ERROR_INSUFFICIENT_BUFFER && result != NO_ERROR) win::fail("枚举端口", result);
    std::vector<BYTE> buffer(size);
    for (;;) {
        result = fetch(buffer.data(), &size);
        if (result != ERROR_INSUFFICIENT_BUFFER) break;
        buffer.resize(size);
    }
    if (result != NO_ERROR) win::fail("枚举端口", result);
    if (buffer.empty()) return;
    const auto *table = reinterpret_cast<const Table *>(buffer.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        auto owner = convert(table->table[i]);
        if (owner.port != requested.port || !addressesConflict(owner.address, requested.address)) continue;
        for (const auto &process : processes) if (process.pid == owner.pid) { owner.parentPid = process.parent; break; }
        owner.imagePath = imagePath(owner.pid, false);
        output.append(owner);
    }
}
QString ipv6(const UCHAR *bytes, DWORD scope) {
    Q_IPV6ADDR address{};
    for (int i = 0; i < 16; ++i) address[i] = bytes[i];
    QHostAddress result(address);
    if (scope) result.setScopeId(QString::number(scope));
    return result.toString();
}
}

QList<PortOwner> WindowsPortManager::owners(const PortRequirement &port) const {
    QList<PortOwner> result;
    const auto processes = snapshot();
    if (port.protocol == "tcp") {
        appendTable<MIB_TCPTABLE_OWNER_PID>(result,
            [](void *buffer, DWORD *size) { return GetExtendedTcpTable(buffer, size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0); },
            [](const MIB_TCPROW_OWNER_PID &row) { return PortOwner{"tcp", QHostAddress(ntohl(row.dwLocalAddr)).toString(), ntohs(static_cast<u_short>(row.dwLocalPort)), row.dwOwningPid, 0, {}}; }, port, processes);
        appendTable<MIB_TCP6TABLE_OWNER_PID>(result,
            [](void *buffer, DWORD *size) { return GetExtendedTcpTable(buffer, size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, 0); },
            [](const MIB_TCP6ROW_OWNER_PID &row) { return PortOwner{"tcp", ipv6(row.ucLocalAddr, row.dwLocalScopeId), ntohs(static_cast<u_short>(row.dwLocalPort)), row.dwOwningPid, 0, {}}; }, port, processes);
    } else if (port.protocol == "udp") {
        appendTable<MIB_UDPTABLE_OWNER_PID>(result,
            [](void *buffer, DWORD *size) { return GetExtendedUdpTable(buffer, size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0); },
            [](const MIB_UDPROW_OWNER_PID &row) { return PortOwner{"udp", QHostAddress(ntohl(row.dwLocalAddr)).toString(), ntohs(static_cast<u_short>(row.dwLocalPort)), row.dwOwningPid, 0, {}}; }, port, processes);
        appendTable<MIB_UDP6TABLE_OWNER_PID>(result,
            [](void *buffer, DWORD *size) { return GetExtendedUdpTable(buffer, size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0); },
            [](const MIB_UDP6ROW_OWNER_PID &row) { return PortOwner{"udp", ipv6(row.ucLocalAddr, row.dwLocalScopeId), ntohs(static_cast<u_short>(row.dwLocalPort)), row.dwOwningPid, 0, {}}; }, port, processes);
    } else throw std::invalid_argument("端口协议无效");
    return result;
}
quint32 WindowsPortManager::parentPid(quint32 pid) const {
    for (const auto &entry : snapshot()) if (entry.pid == pid) return entry.parent;
    return 0;
}
void WindowsPortManager::terminateTree(quint32 pid, const PortRequirement &port, const Cancellation &cancel) {
    const auto processes = snapshot();
    QSet<quint32> visited;
    QList<quint32> ordered;
    const auto descend = [&](auto &&self, quint32 current) -> void {
        if (visited.contains(current)) return;
        visited.insert(current);
        for (const auto &entry : processes) if (entry.parent == current && entry.pid != current) self(self, entry.pid);
        ordered.append(current);
    };
    descend(descend, pid);
    struct Victim { quint32 pid; QString image; win::Handle handle; };
    std::vector<Victim> victims;
    // Validate every boundary and acquire handles before terminating any part of the tree.
    for (const auto target : ordered) {
        cancel.check();
        const auto image = imagePath(target, false);
        const auto context = "PID=" + QString::number(target) + " 映像=" + image + " 端口=" + port.protocol + ":" + port.address + ':' + QString::number(port.port);
        if (protectedPid(target, image)) win::fail("拒绝终止受保护进程 " + context, ERROR_ACCESS_DENIED);
        win::Handle handle(OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, target));
        if (!handle) {
            const auto error = GetLastError(); if (error == ERROR_INVALID_PARAMETER) continue;
            win::fail("无法终止占用者 " + context, error);
        }
        std::vector<wchar_t> path(32768); DWORD length = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(handle.get(), 0, path.data(), &length)) win::fail("无法确认占用者映像 " + context);
        const auto liveImage = QString::fromWCharArray(path.data(), length);
        if (protectedPid(target, liveImage)) win::fail("拒绝终止受保护进程 " + context, ERROR_ACCESS_DENIED);
        victims.push_back({target, liveImage, std::move(handle)});
    }
    for (const auto &victim : victims) {
        cancel.check();
        if (WaitForSingleObject(victim.handle.get(), 0) == WAIT_OBJECT_0) continue;
        const auto context = "PID=" + QString::number(victim.pid) + " 映像=" + victim.image + " 端口=" + QString::number(port.port);
        if (!TerminateProcess(victim.handle.get(), win::forcedExit)) win::fail("TerminateProcess " + context);
        const auto wait = WaitForSingleObject(victim.handle.get(), 2000);
        if (wait != WAIT_OBJECT_0) win::fail("等待占用者退出 " + context, wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    }
}
bool WindowsPortManager::stopService(quint32 pid, const Cancellation &cancel) {
    cancel.check();
    if (protectedPid(pid, imagePath(pid, false))) win::fail("受保护服务 PID=" + QString::number(pid), ERROR_ACCESS_DENIED);
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT));
    if (!manager.get()) win::fail("OpenSCManager");
    DWORD size = 0, count = 0, resume = 0;
    EnumServicesStatusExW(manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &size, &count, &resume, nullptr);
    const auto firstError = GetLastError();
    if (firstError != ERROR_MORE_DATA && size == 0) win::fail("EnumServicesStatusEx", firstError);
    std::vector<BYTE> buffer(size); resume = 0;
    if (!EnumServicesStatusExW(manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buffer.data(), size, &size, &count, &resume, nullptr)) win::fail("EnumServicesStatusEx");
    const auto *services = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW *>(buffer.data());
    bool found = false;
    for (DWORD i = 0; i < count; ++i) {
        cancel.check();
        if (services[i].ServiceStatusProcess.dwProcessId != pid) continue;
        found = true;
        ServiceHandle service(OpenServiceW(manager.get(), services[i].lpServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS));
        if (!service.get()) win::fail("OpenService PID=" + QString::number(pid));
        SERVICE_STATUS status{};
        if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &status)) {
            const auto error = GetLastError();
            if (error != ERROR_SERVICE_NOT_ACTIVE && error != ERROR_SERVICE_CANNOT_ACCEPT_CTRL) win::fail("ControlService PID=" + QString::number(pid), error);
        }
    }
    return found;
}
}
