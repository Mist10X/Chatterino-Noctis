// Compatibility entry point for shortcuts created before the executable was renamed.
#include <windows.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int)
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
    {
        return ERROR_BAD_PATHNAME;
    }
    path.resize(length);
    path.replace(path.find_last_of(L'\\') + 1, std::wstring::npos, L"Chatterino Noctis.exe");
    std::wstring command = L"\"" + path + L"\" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup, &child))
    {
        return static_cast<int>(GetLastError());
    }
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return 0;
}
