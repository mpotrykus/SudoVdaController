#include "TrayClient.h"
#include "../utils/Logger.h"
#include "../utils/StringUtils.h"

#include <windows.h>
#include <thread>
#include <chrono>
#include <sstream>

static const std::wstring PIPE_NAME = L"SudoVdaTrayPipe";

bool StartTrayProcessIfNeeded() {
    std::wstring pipePath = L"\\\\.\\pipe\\" + PIPE_NAME;

    if (WaitNamedPipeW(pipePath.c_str(), 50))
        return true;

    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exePath, ARRAYSIZE(exePath))) {
        LOG_WARN("GetModuleFileNameW failed.");
        return false;
    }

    std::wstring cmd = std::wstring(L"\"") + exePath + L"\" --tray";
    LOG_INFO("Starting tray process with command: %s",
        vdc::WStringToString(cmd).c_str());

    PROCESS_INFORMATION pi{};
    STARTUPINFOW si{};
    si.cb = sizeof(si);

    BOOL ok = CreateProcessW(
        NULL, &cmd[0], NULL, NULL, FALSE,
        0, NULL, NULL, &si, &pi
    );

    if (!ok) {
        LOG_ERROR("CreateProcessW failed: %d", GetLastError());
        return false;
    }

    LOG_INFO("Started tray process, pid=%d", pi.dwProcessId);

    const int maxAttempts = 40;
    const std::chrono::milliseconds delay(250);

    for (int i = 0; i < maxAttempts; ++i) {
        HANDLE h = CreateFileW(pipePath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING, 0, NULL);

        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return true;
        }

        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            LOG_ERROR("Access denied opening pipe.");
            return false;
        }

        std::this_thread::sleep_for(delay);
    }

    LOG_ERROR("Tray pipe did not become available.");
    return false;
}

bool SendToTray(const std::string& message, std::string& outResponse) {
    std::wstring fullPipe = L"\\\\.\\pipe\\" + PIPE_NAME;

    if (!StartTrayProcessIfNeeded()) {
        LOG_ERROR("Failed to start/connect to tray.");
        return false;
    }

    HANDLE h = INVALID_HANDLE_VALUE;
    const int maxAttempts = 40;

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        h = CreateFileW(fullPipe.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING, 0, NULL);

        if (h != INVALID_HANDLE_VALUE)
            break;

        Sleep(1000);
    }

    if (h == INVALID_HANDLE_VALUE)
        return false;

	auto msg = "Sending to tray: " + message;
    LOG_INFO(msg.c_str());

    DWORD written = 0;
    BOOL ok = WriteFile(h, message.c_str(),
        (DWORD)message.size(), &written, NULL);

    if (!ok || written != message.size()) {
        CloseHandle(h);
        return false;
    }

    char buf[8192];
    DWORD read = 0;
    ok = ReadFile(h, buf, sizeof(buf) - 1, &read, NULL);

    if (ok && read > 0) {
        buf[read] = 0;
        outResponse = buf;
    }
    else {
        outResponse.clear();
    }

    CloseHandle(h);
    return true;
}

std::pair<bool, std::string>
SendVerb(const std::string& verb,
    const std::map<std::string, std::string>& kv)
{
    std::ostringstream msg;
    msg << verb << "\n";

    bool first = true;
    for (const auto& p : kv) {
        if (!first) msg << "&";
        first = false;
        msg << p.first << "=" << p.second;
    }

    std::string resp;
    if (!SendToTray(msg.str(), resp))
        return { false, "{\"error\":\"failed to contact tray\"}" };

    return { true, resp };
}
