#include "../pch.h"
#include "TrayActions.h"

#include "../controller/VirtualDisplayservice.h"
#include "../utils/GuidUtils.h"
#include "../utils/Logger.h"
#include "../utils/DisplayConfigUtils.h"
#include "../utils/HdrUtils.h"
#include "../utils/ConfigStore.h"

#include "CustomCreateDialog.h"

#include <codecvt>
#include <locale>
#include <string>
#include <thread>
#include <chrono>
#include "../utils/StringUtils.h"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace vdc {

    namespace {

        constexpr UINT MENU_EXIT_ID = 1001;
        constexpr UINT MENU_CLEAR_CONFIG_ID = 1999;

        static void LaunchCliCreate(const VirtualDisplay& cfg, HWND hwnd) {
            wchar_t exePath[MAX_PATH];
            if (!GetModuleFileNameW(NULL, exePath, ARRAYSIZE(exePath))) {
                MessageBoxW(hwnd, L"Failed to locate executable.", L"Add Display", MB_OK | MB_ICONERROR);
                return;
            }

            std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" create";

            // Include optional display name as a positional argument (quoted)
            if (!cfg.deviceName.empty()) {
                cmd += L" \"" + cfg.deviceName + L"\"";
            }

            cmd += L" --width " + std::to_wstring(cfg.width) +
                   L" --height " + std::to_wstring(cfg.height) +
                   L" --refresh " + std::to_wstring(cfg.refreshRateMilliHz);

            if (cfg.hdr)
                cmd += L" --hdr";

            // Pipe for stdout/stderr
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;

            HANDLE hRead = NULL, hWrite = NULL;
            if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
                MessageBoxW(hwnd, L"Failed to create pipe.", L"Add Display", MB_OK | MB_ICONERROR);
                return;
            }

            SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(0);

            PROCESS_INFORMATION pi{};
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = hWrite;
            si.hStdError = hWrite;

            BOOL ok = CreateProcessW(
                NULL,
                buf.data(),
                NULL,
                NULL,
                TRUE,
                CREATE_NO_WINDOW,
                NULL,
                NULL,
                &si,
                &pi
            );

            CloseHandle(hWrite);

            if (!ok) {
                std::wstring err = L"Failed to start create process: " + std::to_wstring(GetLastError());
                MessageBoxW(hwnd, err.c_str(), L"Add Display", MB_OK | MB_ICONERROR);
                CloseHandle(hRead);
                return;
            }

            // Read output
            std::string output;
            char temp[4096];
            DWORD readBytes = 0;

            while (ReadFile(hRead, temp, sizeof(temp) - 1, &readBytes, NULL) && readBytes > 0) {
                temp[readBytes] = 0;
                output.append(temp, readBytes);
            }

            WaitForSingleObject(pi.hProcess, INFINITE);

            DWORD exitCode = 0;
            GetExitCodeProcess(pi.hProcess, &exitCode);

            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(hRead);

            std::wstring wout = StringToWString(output);

            if (exitCode != 0) {
                std::wstring msg = L"Create command failed";
                if (!wout.empty())
                    msg += L":\n\n" + wout;

                MessageBoxW(hwnd, msg.c_str(), L"Add Display", MB_OK | MB_ICONERROR);
            }
        }

        static void ShowPhysicalDetails(HWND hwnd, const MenuItem& mi) {
            std::wstring title = mi.physicalLabel;
            std::wstring body;

            auto mode = DisplayConfigUtils::GetCurrentModeForDevice(mi.gdiName);
            if (mode) {
                body += L"Resolution: " + std::to_wstring(mode->width) + L"x" +
                        std::to_wstring(mode->height) + L"\n";

            double refresh = static_cast<double>(mode->refreshRateMilliHz) / 1000.0;
            double truncated = std::floor(refresh * 100.0) / 100.0;
            std::wostringstream oss;
            oss << std::fixed << std::setprecision(2) << truncated;
            body += L"Refresh: " + oss.str() + L"Hz\n";

            }
            else {
                body += L"Mode: unknown\n";
            }

            bool hdr = HdrUtils::IsHdrEnabled(mi.gdiName);
            body += hdr ? L"HDR" : L"SDR";

            MessageBoxW(hwnd, body.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
        }

    }

    void HandleTrayCommand(HWND hWnd, UINT cmd, TrayContext* ctx) {
        if (!ctx || !ctx->service || !ctx->serviceMutex || !ctx->menuMap)
            return;

        // Exit
        if (cmd == MENU_EXIT_ID) {
            {
                std::lock_guard<std::mutex> lk(*ctx->serviceMutex);
                auto list = ctx->service->ListDisplays();
                for (const auto& p : list)
                    ctx->service->RemoveVirtualDisplay(p.first);
            }

            // Wait for removal
            for (int i = 0; i < 20; ++i) {
                {
                    std::lock_guard<std::mutex> lk(*ctx->serviceMutex);
                    if (ctx->service->CountDisplays() == 0)
                        break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            PostMessageW(hWnd, WM_CLOSE, 0, 0);
            return;
        }

        // Clear config
        if (cmd == MENU_CLEAR_CONFIG_ID) {
            int resp = MessageBoxW(hWnd, L"Clear saved display configuration?",
                L"Clear Display Config",
                MB_OKCANCEL | MB_ICONWARNING);

            if (resp == IDOK) {
                try {
                    ConfigStore cs;
                    cs.Clear();
                }
                catch (...) {}
            }
            return;
        }

        // Lookup menu item
        auto it = ctx->menuMap->find(cmd);
        if (it == ctx->menuMap->end())
            return;

        MenuItem mi = it->second;

        // -------------------------------------------------------------------------
        // Action dispatch
        // -------------------------------------------------------------------------
        switch (mi.action) {

        case DisplayAction::Remove: {
            std::lock_guard<std::mutex> lk(*ctx->serviceMutex);
            ctx->service->RemoveVirtualDisplay(mi.guid);
            break;
        }

        case DisplayAction::SetPrimary: {
            std::lock_guard<std::mutex> lk(*ctx->serviceMutex);
            ctx->service->SetPrimary(mi.gdiName);
            break;
        }

        case DisplayAction::ToggleHdr: {
            if (!mi.gdiName.empty()) {
                bool enabled = ctx->service->IsHdrEnabled(mi.gdiName);
                ctx->service->SetHdr(mi.gdiName, !enabled);
            }
            break;
        }

        case DisplayAction::Details: {
            if (!mi.gdiName.empty())
                ShowPhysicalDetails(hWnd, mi);
            break;
        }

        case DisplayAction::Add: {
            if (mi.cfg.has_value())
                LaunchCliCreate(*mi.cfg, hWnd);
            break;
        }

        case DisplayAction::Custom: {
            VirtualDisplay cfg;
            if (ShowCustomCreateDialog(hWnd, cfg))
                LaunchCliCreate(cfg, hWnd);
            break;
        }

        default:
            break;
        }
    }

} 
