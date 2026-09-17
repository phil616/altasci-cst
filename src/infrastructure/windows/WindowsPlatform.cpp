#include "WindowsPlatform.h"
#include "Win32Support.h"
#include <wincred.h>
#include <sddl.h>
#include <QDir>
#include <QFileInfo>
#include <vector>

namespace cst {
void WindowsPrivilegeService::requireElevationAndDebugPrivilege() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &raw)) win::fail("CST 必须以管理员权限运行并取得调试权限：OpenProcessToken");
    win::Handle token(raw);
    TOKEN_ELEVATION elevation{}; DWORD size = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size)) win::fail("CST 必须以管理员权限运行并取得调试权限：TokenElevation");
    if (!elevation.TokenIsElevated) win::fail("CST 必须以管理员权限运行并取得调试权限", ERROR_ELEVATION_REQUIRED);
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid)) win::fail("CST 必须以管理员权限运行并取得调试权限：LookupPrivilegeValue");
    SetLastError(ERROR_SUCCESS);
    const auto adjusted = AdjustTokenPrivileges(token.get(), FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    const auto error = GetLastError();
    if (!adjusted || error != ERROR_SUCCESS) win::fail("CST 必须以管理员权限运行并取得调试权限：AdjustTokenPrivileges", error);
}
QString currentUserSid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) win::fail("OpenProcessToken");
    win::Handle token(raw); DWORD length = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
    std::vector<BYTE> buffer(length);
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), length, &length)) win::fail("TokenUser");
    wchar_t *sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &sid)) win::fail("ConvertSidToStringSid");
    const auto result = QString::fromWCharArray(sid); LocalFree(sid); return result;
}
void WindowsCredentialStore::write(const QString &target, const Credential &credential) {
    if (!target.startsWith("CST/git/") || credential.username.isEmpty() || credential.password.isEmpty()) throw std::invalid_argument("凭据名称、用户名和 PAT 必须有效");
    auto secret = credential.password.toUtf8();
    if (secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) throw std::invalid_argument("PAT 超过 Credential Manager 大小限制");
    CREDENTIALW value{};
    value.Type = CRED_TYPE_GENERIC; value.Persist = CRED_PERSIST_LOCAL_MACHINE;
    value.TargetName = const_cast<wchar_t *>(win::wide(target)); value.UserName = const_cast<wchar_t *>(win::wide(credential.username));
    value.CredentialBlob = reinterpret_cast<LPBYTE>(secret.data()); value.CredentialBlobSize = static_cast<DWORD>(secret.size());
    const auto success = CredWriteW(&value, 0); const auto error = GetLastError();
    SecureZeroMemory(secret.data(), static_cast<SIZE_T>(secret.size()));
    if (!success) win::fail("CredWrite", error);
}
std::optional<Credential> WindowsCredentialStore::read(const QString &target) const {
    PCREDENTIALW value = nullptr;
    if (!CredReadW(win::wide(target), CRED_TYPE_GENERIC, 0, &value)) {
        const auto error = GetLastError(); if (error == ERROR_NOT_FOUND) return std::nullopt;
        win::fail("CredRead", error);
    }
    Credential result{QString::fromWCharArray(value->UserName), QString::fromUtf8(reinterpret_cast<const char *>(value->CredentialBlob), value->CredentialBlobSize)};
    SecureZeroMemory(value->CredentialBlob, value->CredentialBlobSize); CredFree(value); return result;
}
void WindowsCredentialStore::remove(const QString &target) {
    if (!CredDeleteW(win::wide(target), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) win::fail("CredDelete");
}
void WindowsFileTransaction::renameDirectory(const QString &from, const QString &to) {
    if (!MoveFileExW(win::wide(QDir::toNativeSeparators(from)), win::wide(QDir::toNativeSeparators(to)), MOVEFILE_WRITE_THROUGH))
        win::fail("目录交换：" + from + " → " + to);
}
bool WindowsFileTransaction::removeDirectory(const QString &path) {
    return !QFileInfo::exists(path) || QDir(path).removeRecursively();
}
}
