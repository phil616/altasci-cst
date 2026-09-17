#include <windows.h>
#include <cwchar>
#include <cstdlib>

int wmain(int argc, wchar_t **argv) {
    if (argc != 2) return 2;
    wchar_t *end = nullptr;
    const auto pid = std::wcstoul(argv[1], &end, 10);
    if (!end || *end || pid == 0) return 3;
    FreeConsole();
    if (!SetConsoleCtrlHandler(nullptr, TRUE)) return 4;
    if (!AttachConsole(static_cast<DWORD>(pid))) return 5;
    // AttachConsole resets the handler table, so install the ignore handler again.
    if (!SetConsoleCtrlHandler([](DWORD) -> BOOL { return TRUE; }, TRUE)) return 6;
    const bool sent = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0) != FALSE;
    Sleep(50);
    FreeConsole();
    return sent ? 0 : 7;
}
