#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <QString>
#include <stdexcept>
#include <utility>

namespace cst::win {
inline const wchar_t *wide(const QString &text) { return reinterpret_cast<const wchar_t *>(text.utf16()); }
inline QString errorText(const QString &operation, DWORD code = GetLastError()) {
    wchar_t *buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    const auto detail = buffer ? QString::fromWCharArray(buffer).trimmed() : QString{};
    if (buffer) LocalFree(buffer);
    return operation + " (Win32 " + QString::number(code) + "): " + detail;
}
[[noreturn]] inline void fail(const QString &operation, DWORD code = GetLastError()) {
    throw std::runtime_error(errorText(operation, code).toUtf8().constData());
}
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : value_(other.release()) {}
    Handle &operator=(Handle &&other) noexcept { reset(other.release()); return *this; }
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE release() { return std::exchange(value_, nullptr); }
    void reset(HANDLE value = nullptr) { if (*this) CloseHandle(value_); value_ = value; }
private:
    HANDLE value_ = nullptr;
};
inline constexpr UINT forcedExit = 0xC5700001u;
}
