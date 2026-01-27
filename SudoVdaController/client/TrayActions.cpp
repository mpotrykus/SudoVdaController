#include "../pch.h"
#include "TrayActions.h"

#include "../controller/VirtualDisplayController.h"
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

namespace vdc {

    namespace {

        constexpr UINT MENU_EXIT_ID = 1001;
        constexpr UINT MENU_CLEAR_CONFIG_ID = 1999;

        // Convert UTF‑8 → wide
        static std::wstring Utf8ToWide(const std::string& s) {
            try {
                return std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(s);
            }
            catch (...) {
                return std::wstring(s.begin(), s.end());
            }
        }

        // Launch CLI create command (same exe)
        static void LaunchCliCreate(const VirtualDisplay& cfg, HWND hwnd) {
            wchar_t exePath[MAX_PATH];
            if (!GetModuleFileNameW(NULL, exePath, ARRAYSIZE(exePath))) {
                MessageBoxW(hwnd, L"Failed to locate executable.", L"Add Display", MB_OK | MB_ICONERROR);
                return;
            }

            std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" create"
                L" --width " + std::to_wstring(cfg.width) +
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

            std::wstring wout = Utf8ToWide(output);

            if (exitCode != 0) {
                std::wstring msg = L"Create command failed";
                if (!wout.empty())
                    msg += L":\n\n" + wout;

                MessageBoxW(hwnd, msg.c_str(), L"Add Display", MB_OK | MB_ICONERROR);
            }
        }

        // Show details for a physical display
        static void ShowPhysicalDetails(HWND hwnd, const MenuItem& mi) {
            std::wstring title = mi.physicalLabel;
            std::wstring body;

            auto mode = DisplayConfigUtils::GetCurrentModeForDevice(mi.physicalName);
            if (mode) {
                body += L"Resolution: " + std::to_wstring(mode->width) + L"x" +
                    std::to_wstring(mode->height) + L"\n";
                body += L"Refresh (mHz): " + std::to_wstring(mode->refreshRateMilliHz) + L"\n";
            }
            else {
                body += L"Mode: unknown\n";
            }

            bool hdr = HdrUtils::IsHdrEnabled(mi.physicalName);
            body += L"HDR: ";
            body += hdr ? L"Enabled" : L"Disabled";

            MessageBoxW(hwnd, body.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
        }

    } // anonymous namespace

    // -----------------------------------------------------------------------------
    // HandleTrayCommand
    // -----------------------------------------------------------------------------
    void HandleTrayCommand(HWND hWnd, UINT cmd, TrayContext* ctx) {
        if (!ctx || !ctx->controller || !ctx->controllerMutex || !ctx->menuMap)
            return;

        // Exit
        if (cmd == MENU_EXIT_ID) {
            {
                std::lock_guard<std::mutex> lk(*ctx->controllerMutex);
                auto list = ctx->controller->ListDisplays();
                for (const auto& p : list)
                    ctx->controller->RemoveDisplay(p.first);
            }

            // Wait for removal
            for (int i = 0; i < 20; ++i) {
                {
                    std::lock_guard<std::mutex> lk(*ctx->controllerMutex);
                    if (ctx->controller->CountDisplays() == 0)
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
            std::lock_guard<std::mutex> lk(*ctx->controllerMutex);
            ctx->controller->RemoveDisplay(mi.guid);
            break;
        }

        case DisplayAction::SetPrimary: {
            std::lock_guard<std::mutex> lk(*ctx->controllerMutex);
            ctx->controller->SetPrimary(mi.guid);
            break;
        }

        case DisplayAction::ToggleHdr: {
            if (!mi.physicalName.empty()) {
                bool enabled = HdrUtils::IsHdrEnabled(mi.physicalName);
                HdrUtils::SetHdrState(mi.physicalName, !enabled);
            }
            break;
        }

        case DisplayAction::Details: {
            if (!mi.physicalName.empty())
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

} // namespace vdc
