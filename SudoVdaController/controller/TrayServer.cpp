#include "../pch.h"
#include "TrayServer.h"
#include "VirtualDisplayController.h"
#include "../utils/GuidUtils.h"

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <sstream>
#include <map>
#include <vector>
#include <thread>
#include <atomic>
#include <locale>
#include <codecvt>
#include <mutex>
#include <unordered_map>
#include <optional>
#include <sddl.h>
#include <iostream>
#include "../Utils/JsonUtils.h"
#include "../utils/DisplayConfigUtils.h"
#include "../utils/HdrUtils.h"

using namespace vdc;

namespace {
    constexpr UINT WM_TRAYICON = WM_APP + 1;
    constexpr wchar_t WINDOW_CLASS_NAME[] = L"SudoVdaTrayWindowClass";
    constexpr UINT MENU_BASE_ID = 2000;
    constexpr UINT MENU_EXIT_ID = 1001;

    static std::atomic<bool> g_running{ false };

    enum class DisplayAction : int {
        None =       0,
        Details =    1,
        Remove =     2,
        Add =        3,
        SetPrimary = 4,
        ToggleHdr =  5,
        Custom =     6
    };

    struct MenuItem {
        GUID guid;
        DisplayAction action = DisplayAction::None;
        std::optional<vdc::VirtualDisplayConfig> cfg; // optional config for Add action
        std::wstring physicalName; // for physical displays (GUID() sentinel), store GDI name
        std::wstring physicalLabel; // full label shown in menu for physical displays
    };

    // Window -> context with controller pointer, mutex and menu map
    struct TrayContext {
        vdc::VirtualDisplayController* controller;
        std::mutex* controllerMutex;
        std::unordered_map<UINT, MenuItem>* menuMap;
    };

    // single static context instance used by WndProc and initialized in RunTrayServer
    static TrayContext g_ctx{};

    static std::map<std::string,std::string> ParseKvForm(const std::string& s) {
        std::map<std::string,std::string> m;
        size_t pos=0;
        while (pos < s.size()) {
            auto amp = s.find('&', pos);
            std::string tok = s.substr(pos, (amp==std::string::npos)? std::string::npos : amp - pos);
            auto eq = tok.find('=');
            if (eq != std::string::npos) {
                m[tok.substr(0,eq)] = tok.substr(eq+1);
            }
            if (amp==std::string::npos) break;
            pos = amp+1;
        }
        return m;
    }

    static int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    }

    static std::string PercentDecode(const std::string& s) {
        std::string out;
        for (size_t i=0;i<s.size();++i) {
            if (s[i]=='%' && i+2 < s.size()) {
                unsigned char v = (unsigned char)((hexVal(s[i+1])<<4) | hexVal(s[i+2]));
                out.push_back((char)v);
                i += 2;
            } else if (s[i] == '+') {
                out.push_back(' ');
            } else {
                out.push_back(s[i]);
            }
        }
        return out;
    }

    static bool ReadAllFromPipe(HANDLE hPipe, std::string& out) {
        out.clear();
        char buf[4096];
        DWORD read = 0;
        for (;;) {
            BOOL ok = ReadFile(hPipe, buf, (DWORD)sizeof(buf)-1, &read, NULL);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_MORE_DATA && read > 0) {
                    buf[read] = 0;
                    out.append(buf, read);
                    continue;
                }
                return !out.empty();
            }
            if (read == 0) break;
            buf[read] = 0;
            out.append(buf, read);
            if (read < sizeof(buf)-1) break;
        }
        return true;
    }

    static bool WriteAllToPipe(HANDLE hPipe, const std::string& msg) {
        DWORD written = 0;
        return WriteFile(hPipe, msg.c_str(), (DWORD)msg.size(), &written, NULL) && written == msg.size();
    }

    // Launch the CLI (same exe) to perform a create command using the selected mode string.
    static void LaunchCliCreate(const vdc::VirtualDisplayConfig& cfg, HWND hwnd) {
        // command line
        wchar_t exePath[MAX_PATH];
        if (!GetModuleFileNameW(NULL, exePath, ARRAYSIZE(exePath))) {
            MessageBoxW(hwnd, L"Failed to locate executable.", L"Add Display", MB_OK | MB_ICONERROR);
            return;
        }
        std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" create --width " + std::to_wstring(cfg.width)
            + L" --height " + std::to_wstring(cfg.height)
            + L" --refresh " + std::to_wstring(cfg.refreshRateMilliHz);
        if (cfg.hdr) cmd += L" --hdr";

        // create pipe for stdout/stderr
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = NULL;

        HANDLE hRead = NULL;
        HANDLE hWrite = NULL;
        if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
            MessageBoxW(hwnd, L"Failed to create pipe.", L"Add Display", MB_OK | MB_ICONERROR);
            return;
        }
        // ensure the read handle is not inherited
        SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

        // prepare process startup with redirected handles
        std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
        PROCESS_INFORMATION pi{};
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;
        si.hStdInput = NULL;

        BOOL ok = CreateProcessW(NULL, buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
        // parent can close write handle immediately; child has its own handle
        CloseHandle(hWrite);

        if (!ok) {
            std::wstring err = L"Failed to start create process: " + std::to_wstring(GetLastError());
            MessageBoxW(hwnd, err.c_str(), L"Add Display", MB_OK | MB_ICONERROR);
            CloseHandle(hRead);
            return;
        }

        // Read all output from child process while it runs (nonblocking loop)
        std::string output;
        const DWORD bufSize = 4096;
        char buffer[bufSize];
        DWORD readBytes = 0;
        // Read until pipe closed by child
        for (;;) {
            BOOL r = ReadFile(hRead, buffer, bufSize - 1, &readBytes, NULL);
            if (!r || readBytes == 0) break;
            buffer[readBytes] = 0;
            output.append(buffer, readBytes);
        }

        // Wait for child exit and fetch exit code
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(hRead);

        // Convert output (assumed UTF-8) to wide string for MessageBox
        std::wstring wout;
        try {
            wout = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(output);
        } catch (...) {
            // fallback: crudely widen bytes
            wout.assign(output.begin(), output.end());
        }

        if (exitCode != 0) {
            std::wstring msg = L"Create command failed";
            if (!wout.empty()) msg += L":\n\n" + wout;
            MessageBoxW(hwnd, msg.c_str(), L"Add Display", MB_OK | MB_ICONERROR);
        }
    }

    // Show a simple modal custom-create dialog. Returns true when user pressed Create and outCfg is filled.
    // control IDs for custom dialog
    constexpr int IDC_NAME = 1001;
    constexpr int IDC_WIDTH = 1002;
    constexpr int IDC_HEIGHT = 1003;
    constexpr int IDC_REFRESH = 1004;
    constexpr int IDC_HDR = 1005;
    constexpr int IDC_CREATE = 1006;
    constexpr int IDC_CANCEL = 1007;

    struct CustomDialogState { HWND dlg; HWND eName,eWidth,eHeight,eRefresh,hdr; vdc::VirtualDisplayConfig out; bool ok; };

    static LRESULT CALLBACK CustomCreateDlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        CustomDialogState* st = (CustomDialogState*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (!st) break;
            if (id == IDC_CREATE) {
                // gather values
                wchar_t buf[256];
                GetWindowTextW(st->eName, buf, _countof(buf)); st->out.deviceName = buf;
                GetWindowTextW(st->eWidth, buf, _countof(buf)); try { st->out.width = std::stoi(std::wstring(buf)); } catch(...) {}
                GetWindowTextW(st->eHeight, buf, _countof(buf)); try { st->out.height = std::stoi(std::wstring(buf)); } catch(...) {}
                GetWindowTextW(st->eRefresh, buf, _countof(buf)); {
                    std::wstring s(buf);
                    try {
                        if (s.find(L'.') != std::wstring::npos) {
                            double hz = std::stod(std::wstring(s)); st->out.refreshRateMilliHz = static_cast<int>(hz * 1000.0 + 0.5);
                        } else {
                            long long v = std::stoll(std::wstring(s));
                            if (v < 1000) st->out.refreshRateMilliHz = static_cast<int>(v * 1000);
                            else st->out.refreshRateMilliHz = static_cast<int>(v);
                        }
                    } catch(...) {}
                }
                st->out.hdr = (SendMessageW(st->hdr, BM_GETCHECK, 0, 0) == BST_CHECKED);
                st->ok = true;
                DestroyWindow(hWnd);
                return 0;
            }
            if (id == IDC_CANCEL) {
                st->ok = false;
                DestroyWindow(hWnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hWnd);
            return 0;
        case WM_DESTROY:
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    static bool ShowCustomCreateDialog(HWND parent, vdc::VirtualDisplayConfig& outCfg) {
        CustomDialogState* st = new CustomDialogState();
        st->dlg = NULL; st->eName = NULL; st->eWidth = NULL; st->eHeight = NULL; st->eRefresh = NULL; st->hdr = NULL; st->ok = false;

        const int W = 380, H = 220;
        st->dlg = CreateWindowExW(0, L"Static", L"Create Display", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, W, H, parent, NULL, GetModuleHandleW(NULL), NULL);
        if (!st->dlg) { delete st; return false; }

        // Create controls
        int x = 12, y = 12, labelW = 80, editW = 260, h = 22, gap = 6;
        CreateWindowExW(0, L"Static", L"Name:", WS_CHILD | WS_VISIBLE, x, y, labelW, h, st->dlg, NULL, GetModuleHandleW(NULL), NULL);
        st->eName = CreateWindowExW(0, L"Edit", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | WS_TABSTOP, x+labelW, y, editW, h, st->dlg, (HMENU)IDC_NAME, GetModuleHandleW(NULL), NULL);
        y += h + gap;
        CreateWindowExW(0, L"Static", L"Width:", WS_CHILD | WS_VISIBLE, x, y, labelW, h, st->dlg, NULL, GetModuleHandleW(NULL), NULL);
        st->eWidth = CreateWindowExW(0, L"Edit", L"1920", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | WS_TABSTOP, x+labelW, y, 100, h, st->dlg, (HMENU)IDC_WIDTH, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"Static", L"Height:", WS_CHILD | WS_VISIBLE, x+labelW+110, y, 60, h, st->dlg, NULL, GetModuleHandleW(NULL), NULL);
        st->eHeight = CreateWindowExW(0, L"Edit", L"1080", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | WS_TABSTOP, x+labelW+170, y, 90, h, st->dlg, (HMENU)IDC_HEIGHT, GetModuleHandleW(NULL), NULL);
        y += h + gap;
        CreateWindowExW(0, L"Static", L"Refresh:", WS_CHILD | WS_VISIBLE, x, y, labelW, h, st->dlg, NULL, GetModuleHandleW(NULL), NULL);
        st->eRefresh = CreateWindowExW(0, L"Edit", L"60", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | WS_TABSTOP, x+labelW, y, 100, h, st->dlg, (HMENU)IDC_REFRESH, GetModuleHandleW(NULL), NULL);
        st->hdr = CreateWindowExW(0, L"Button", L"HDR", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, x+labelW+110, y, 80, h, st->dlg, (HMENU)IDC_HDR, GetModuleHandleW(NULL), NULL);
        y += h + gap*2;

        HWND bCreate = CreateWindowExW(0, L"Button", L"Create", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, W - 200, H - 75, 80, 26, st->dlg, (HMENU)IDC_CREATE, GetModuleHandleW(NULL), NULL);
        HWND bCancel = CreateWindowExW(0, L"Button", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, W - 110, H - 75, 80, 26, st->dlg, (HMENU)IDC_CANCEL, GetModuleHandleW(NULL), NULL);

        // subclass dialog to handle commands
        SetWindowLongPtrW(st->dlg, GWLP_USERDATA, (LONG_PTR)st);
        // subclass dialog to custom proc
        SetWindowLongPtrW(st->dlg, GWLP_WNDPROC, (LONG_PTR)CustomCreateDlgProc);

        // Always center on the primary screen so the dialog is reachable
        int sx = GetSystemMetrics(SM_CXSCREEN);
        int sy = GetSystemMetrics(SM_CYSCREEN);
        int px = sx/2 - W/2;
        int py = sy/2 - H/2;
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        SetWindowPos(st->dlg, NULL, px, py, 0,0, SWP_NOSIZE | SWP_NOZORDER);

        EnableWindow(parent, FALSE);
        ShowWindow(st->dlg, SW_SHOW);
        // set initial focus to first control
        SetFocus(st->eName);

        // modal message loop until dialog destroyed using GetMessage + IsDialogMessage for tab handling
        MSG msg{};
        while (IsWindow(st->dlg)) {
            if (!GetMessageW(&msg, NULL, 0, 0)) break;
            if (!IsDialogMessageW(st->dlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        // after window destroyed, check st->ok and populate outCfg
        bool okRes = st->ok;
        if (okRes) outCfg = st->out;
        EnableWindow(parent, TRUE);
        delete st;
        return okRes;
    }

    // Proper window proc
    LRESULT CALLBACK TrayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        TrayContext* ctx = &g_ctx;
        if (msg == WM_TRAYICON) {
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                POINT pt; GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                if (hMenu && ctx && ctx->controller && ctx->controllerMutex && ctx->menuMap) {
                    // Build menu from current displays
                    std::lock_guard<std::mutex> lk(*ctx->controllerMutex);
                    auto list = ctx->controller->ListDisplays();
                    ctx->menuMap->clear();
                    UINT id = MENU_BASE_ID;
                    for (const auto& p : list) {
                        // label: deviceName (guid)
                        std::wstring label = p.second;
                        std::string gstr = vdc::GuidToString(p.first);
                        std::wstring gw = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(gstr);

                        // Submenu
						HMENU hSub = CreatePopupMenu();
                        if (!hSub) continue;

                        bool isPhysical = (p.first == GUID());

                        // Set Primary
                        {
                            AppendMenuW(hSub, MF_STRING, id, L"Set Primary");
                            MenuItem it{}; it.guid = p.first; it.action = DisplayAction::SetPrimary;
                            if (isPhysical) {
                                // parse GDI name from label: last parenthesized part
                                size_t l = label.rfind(L'(');
                                size_t r = label.rfind(L')');
                                if (l != std::wstring::npos && r != std::wstring::npos && r > l) {
                                    it.physicalName = label.substr(l+1, r-l-1);
                                }
                                it.physicalLabel = label;
                            }
                            ctx->menuMap->emplace(id, it);
                            ++id;
                        }

                        // Toggle HDR
                        {
                            AppendMenuW(hSub, MF_STRING, id, L"Toggle HDR");
                            MenuItem it{}; it.guid = p.first; it.action = DisplayAction::ToggleHdr;
                            if (isPhysical) { it.physicalName = label.substr(label.rfind(L'(')+1, label.rfind(L')') - label.rfind(L'(') - 1); it.physicalLabel = label; }
                            ctx->menuMap->emplace(id, it);
                            ++id;
                        }

                        AppendMenuW(hSub, MF_SEPARATOR, 0, nullptr);

                        // Details
                        {
                            AppendMenuW(hSub, MF_STRING, id, L"Details");
                            MenuItem it{}; it.guid = p.first; it.action = DisplayAction::Details;
                            if (isPhysical) { it.physicalName = label.substr(label.rfind(L'(')+1, label.rfind(L')') - label.rfind(L'(') - 1); it.physicalLabel = label; }
                            ctx->menuMap->emplace(id, it);
                            ++id;
                        }

                        // Remove (only for virtual displays)
                        if (!isPhysical) {
                            AppendMenuW(hSub, MF_STRING, id, L"Remove");
                            ctx->menuMap->emplace(id, MenuItem{ p.first, DisplayAction::Remove });
                            ++id;
                        }

                        // Append popup submenu for this display with the label
                        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSub, label.c_str());
                    }
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

                    // Add submenu for "Add" modes above Exit
                    HMENU hRes = CreatePopupMenu();
                    if (hRes) {
                        
                        std::vector<std::string> resolutions = { "3840x2160", "2560x1440", "1920x1080", "1280x720" };
                        std::vector<std::string> refreshRates = { "240hz", "144hz", "120hz", "199.97hz", "60hz", "59.97hz" , "24hz" };
                        std::vector<std::string> colorSpace = { "HDR", "SDR" };

                        for (const auto& res : resolutions) {

                            HMENU hRate = CreatePopupMenu();
                            if (!hRate) continue;

                            std::wstring wRes = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(res);

                            for (const auto& rate : refreshRates) {

                                HMENU hAdd = CreatePopupMenu();
                                if (!hAdd) continue;

                                std::wstring wRate = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(rate);

                                for (const auto& cs : colorSpace) {
                                    std::wstring wCs = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(cs);
                                    AppendMenuW(hAdd, MF_STRING, id, wCs.c_str());

                                    vdc::VirtualDisplayConfig cfg;
                                    // parse res like "1920x1080"
                                    try {
                                        auto x = res.find('x');
                                        if (x!=std::string::npos) {
                                            cfg.width = std::stoi(res.substr(0,x));
                                            cfg.height = std::stoi(res.substr(x+1));
                                        }
                                    } catch(...) {}
                                    // parse rate like "60hz" or "59.97hz"
                                    try {
                                        std::string rateClean;
                                        for (char c: rate) if ((c>='0' && c<='9')||c=='.') rateClean.push_back(c);
                                        if (!rateClean.empty()) {
                                            double rf = std::stod(rateClean);
                                            cfg.refreshRateMilliHz = static_cast<int>(rf * 1000.0);
                                        }
                                    } catch(...) {}
                                    cfg.hdr = (cs == "HDR");

                                    ctx->menuMap->emplace(id, MenuItem{ GUID(), DisplayAction::Add, cfg});
                                    ++id;
                                }
                                AppendMenuW(hRate, MF_POPUP, (UINT_PTR)hAdd, wRate.c_str());

                            }
                            AppendMenuW(hRes, MF_POPUP, (UINT_PTR)hRate, wRes.c_str());

                        }
                        // Custom option
                        AppendMenuW(hRes, MF_SEPARATOR, 0, nullptr);
                        AppendMenuW(hRes, MF_STRING, id, L"Custom");
                        ctx->menuMap->emplace(id, MenuItem{ GUID(), DisplayAction::Custom, std::optional<vdc::VirtualDisplayConfig>{} });
                        ++id;

                        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hRes, L"Add display");
                    }

                    AppendMenuW(hMenu, MF_STRING, MENU_EXIT_ID, L"Exit");

                    SetForegroundWindow(hWnd); // required for TrackPopupMenu
                    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);

                    DestroyMenu(hMenu);
                }
                return 0;
            }
        }

        // Tray Commands
        if (msg == WM_COMMAND) {
            UINT cmd = LOWORD(wParam);

			// Exit selection
            if (cmd == MENU_EXIT_ID) {
                // Remove all displays before exiting to ensure clean shutdown.
                // Do removals under the controller mutex, then wait briefly for them to be torn down.
                if (ctx && ctx->controller && ctx->controllerMutex) {
                    {
                        std::lock_guard<std::mutex> lk(*ctx->controllerMutex);
                        auto list = ctx->controller->ListDisplays();
                        for (const auto& p : list) {
                            ctx->controller->RemoveDisplay(p.first);
                        }
                    }
                    // Wait for displays to be removed (poll with timeout).
                    const int maxChecks = 20;
                    const std::chrono::milliseconds waitInterval(100);
                    for (int i = 0; i < maxChecks; ++i) {
                        {
                            std::lock_guard<std::mutex> lk(*ctx->controllerMutex);
                            if (ctx->controller->CountDisplays() == 0) break;
                        }
                        std::this_thread::sleep_for(waitInterval);
                    }
                }
                g_running.store(false);
                PostMessageW(hWnd, WM_CLOSE, 0, 0);
                return 0;
            }

            // per-display submenu selection
            if (ctx && ctx->menuMap) {
                auto it = ctx->menuMap->find(cmd);
                if (it != ctx->menuMap->end()) {

					MenuItem mi = it->second;
					GUID toActOn = mi.guid;
                    if (mi.action == DisplayAction::Details) {
                        if (!mi.physicalName.empty()) {
                            // Physical display: build details using DisplayConfigUtils + HdrUtils
                            std::wstring title = mi.physicalLabel;
                            std::wstring body = L"";
                            auto mode = vdc::DisplayConfigUtils::GetCurrentModeForDevice(mi.physicalName);
                            if (mode) {
                                body += L"Resolution: " + std::to_wstring(mode->width) + L"x" + std::to_wstring(mode->height) + L"\n";
                                body += L"Refresh (mHz): " + std::to_wstring(mode->refreshRateMilliHz) + L"\n";
                            } else {
                                body += L"Mode: unknown\n";
                            }
                            bool hdr = vdc::HdrUtils::IsHdrEnabled(mi.physicalName);
                            body += L"HDR: "; body += hdr ? L"enabled" : L"disabled"; body += L"\n";
                            MessageBoxW(hWnd, body.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
                        } else {
                            std::string json;
                            {
                                std::lock_guard<std::mutex> lk(*ctx->controllerMutex);
                                auto res = ctx->controller->Query(toActOn);
                                json = res.json;
                            }
                            std::string formatted = vdc::JsonBuilder::FormatJsonAsList(json);
                            std::wstring wformatted = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(formatted);
                            MessageBoxW(hWnd, wformatted.c_str(), L"Display Details", MB_OK | MB_ICONINFORMATION);
                        }
                    }
                    else if (mi.action == DisplayAction::Remove) {
                        bool removed = false;
                        {
                            std::lock_guard<std::mutex> lk(*ctx->controllerMutex);
                            auto res = ctx->controller->RemoveDisplay(toActOn);
                            removed = res.success;
                            if (removed && ctx->controller->CountDisplays() == 0) {
                                g_running.store(false);
                                PostMessageW(hWnd, WM_CLOSE, 0, 0);
                            }
                        }
                        if (!removed) {
                            MessageBoxW(hWnd, L"Failed to remove display.", L"Error", MB_OK | MB_ICONERROR);
						}
                    }
                    else if (mi.action == DisplayAction::Add && mi.cfg) {
                        // Launch CLI create flow for the selected mode using typed config
                        LaunchCliCreate(*mi.cfg, hWnd);
                        return 0;
                    }
                    else if (mi.action == DisplayAction::SetPrimary) {
                        if (!mi.physicalName.empty()) {
                            bool ok = vdc::DisplayConfigUtils::MakeDevicePrimary(mi.physicalName);
                            if (!ok) MessageBoxW(hWnd, L"Failed to set physical display as primary.", L"Error", MB_OK | MB_ICONERROR);
                            return 0;
                        } else {
                            bool ok = false;
                            {
                                std::lock_guard<std::mutex> lk(*ctx->controllerMutex);
                                auto res = ctx->controller->SetPrimary(toActOn);
                                ok = res.success;
                            }
                            if (!ok) MessageBoxW(hWnd, L"Failed to set display as primary.", L"Error", MB_OK | MB_ICONERROR);
                            return 0;
                        }
                    }
                    else if (mi.action == DisplayAction::ToggleHdr) {
                        if (!mi.physicalName.empty()) {
                            bool cur = vdc::HdrUtils::IsHdrEnabled(mi.physicalName);
                            bool ok = vdc::HdrUtils::SetHdrState(mi.physicalName, !cur);
                            if (!ok) MessageBoxW(hWnd, L"Failed to toggle HDR for physical display.", L"Error", MB_OK | MB_ICONERROR);
                            return 0;
                        } else {
                            bool ok = false;
                            {
                                std::lock_guard<std::mutex> lk(*ctx->controllerMutex);
                                auto q = ctx->controller->Query(toActOn);
                                if (q.success) {
                                    bool hdr = q.json.find("\"hdr\":true") != std::string::npos;
                                    auto res = ctx->controller->SetHdr(toActOn, !hdr);
                                    ok = res.success;
                                }
                            }
                            if (!ok) MessageBoxW(hWnd, L"Failed to toggle HDR for display.", L"Error", MB_OK | MB_ICONERROR);
                            return 0;
                        }
                    }
                    else if (mi.action == DisplayAction::Custom) {
                        vdc::VirtualDisplayConfig cfg;
                        if (ShowCustomCreateDialog(hWnd, cfg)) {
                            LaunchCliCreate(cfg, hWnd);
                        }
                        return 0;
                    }
                    return 0;
                }
            }
            return 0;
        }
        if (msg == WM_CLOSE) {
            // signal running=false to help server thread exit promptly
            g_running.store(false);
            DestroyWindow(hWnd);
            return 0;
        }
        if (msg == WM_NCDESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

int vdc::RunTrayServer(const std::wstring& pipeName) {
    VirtualDisplayController controller;

    // Register hidden window class
    WNDCLASSW wc{};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = WINDOW_CLASS_NAME;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, WINDOW_CLASS_NAME, L"SudoVdaTrayWindow", 0,
        0,0,0,0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) {
        return 1;
    }

    // Add tray icon
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"SudoVda Controller");
    Shell_NotifyIconW(NIM_ADD, &nid);

    g_running.store(true);

    // Mutex protecting controller access and menu id mapping
    std::mutex controllerMutex;
    std::unordered_map<UINT, MenuItem> menuMap;
    // initialize static context so WndProc can access controller/menu safely
    g_ctx.controller = &controller; // No-op reassignment for consistency
    g_ctx.controllerMutex = &controllerMutex; // No-op reassignment for consistency
    g_ctx.menuMap = &menuMap; // No-op reassignment for consistency

    // On startup, attempt to apply persisted topology to restore enabled/disabled state
    try {
        vdc::ConfigStore cs;
        auto topo = cs.GetTopologyMap();
        if (!topo.empty()) {
            vdc::DisplayConfigUtils::ApplyTopologyFromStore(topo);
        }
    } catch(...) {}

    // Start pipe server thread
    constexpr wchar_t READY_EVENT_NAME[] = L"SudoVdaTray_ReadyEvent";
    // shutdown event used to interrupt ConnectNamedPipe waits
    HANDLE shutdownEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    std::thread serverThread([&]() {
        std::wstring fullPipe = L"\\\\.\\pipe\\" + pipeName;

        // Build a permissive security descriptor for the pipe so clients in other
        // sessions / privilege levels (for debugging) can connect.
        // SDDL "D:(A;;GA;;;WD)" = allow GENERIC_ALL to Everyone.
        PSECURITY_DESCRIPTOR psd = NULL;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &psd, NULL)) {
            std::cout << "ConvertStringSecurityDescriptorToSecurityDescriptorW failed: " << GetLastError() << "\n";
            psd = NULL;
        }
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = psd;
        sa.bInheritHandle = FALSE;

        bool readySignaled = false;
        while (g_running.load()) {
            HANDLE hPipe = CreateNamedPipeW(
                fullPipe.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                8192, 8192,
                0,
                psd ? &sa : NULL // pass SECURITY_ATTRIBUTES when we successfully created a descriptor
            );
            if (hPipe == INVALID_HANDLE_VALUE) {
                std::cout << "CreateNamedPipeW failed: " << GetLastError() << "\n";
                Sleep(200);
                continue;
            }

            // Signal readiness once when we have successfully created the first pipe instance.
            if (!readySignaled) {
                HANDLE hEvent = CreateEventW(NULL, TRUE, TRUE, READY_EVENT_NAME);
                if (hEvent) CloseHandle(hEvent);
                readySignaled = true;
            }

            // Use overlapped ConnectNamedPipe so we can also wait on shutdownEvent.
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            BOOL connectedSync = ConnectNamedPipe(hPipe, &ov);
            BOOL connected = FALSE;
            if (connectedSync) {
                connected = TRUE;
            } else {
                DWORD err = GetLastError();
                if (err == ERROR_PIPE_CONNECTED) {
                    connected = TRUE;
                } else if (err == ERROR_IO_PENDING) {
                    // wait for either connection or shutdown
                    HANDLE waitHandles[2] = { ov.hEvent, shutdownEvent };
                    DWORD w = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
                    if (w == WAIT_OBJECT_0) {
                        // overlapped event signaled
                        DWORD bytes = 0;
                        DWORD gle = 0;
                        if (GetOverlappedResult(hPipe, &ov, &bytes, FALSE)) {
                            connected = TRUE;
                        }
                    } else {
                        // shutdown signaled, cancel and close pipe
                        CancelIoEx(hPipe, &ov);
                        CloseHandle(hPipe);
                        CloseHandle(ov.hEvent);
                        continue;
                    }
                } else {
                    // other error
                    CloseHandle(hPipe);
                    CloseHandle(ov.hEvent);
                    continue;
                }
            }
            if (ov.hEvent) CloseHandle(ov.hEvent);
            if (!connected) { CloseHandle(hPipe); continue; }

            std::string request;
            if (!ReadAllFromPipe(hPipe, request)) {
                // nothing read
                DisconnectNamedPipe(hPipe);
                CloseHandle(hPipe);
                continue;
            }

            // parse verb and payload (verb\npayload)
            std::string verb, payload;
            auto nl = request.find('\n');
            if (nl != std::string::npos) {
                verb = request.substr(0, nl);
                payload = request.substr(nl+1);
            } else {
                verb = request;
                payload.clear();
            }

            ControllerResult cres{false, "unknown", "{\"error\":\"bad request\"}"};
            bool shouldShutdownFromPipe = false;

            if (verb == "create") {
                auto kv = ParseKvForm(payload);
                vdc::VirtualDisplayConfig cfg;
                if (kv.count("deviceName")) {
                    std::string dn = PercentDecode(kv["deviceName"]);
                    std::wstring wdn = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(dn);
                    cfg.deviceName = wdn;
                }
                if (kv.count("width")) cfg.width = std::stoi(kv["width"]);
                if (kv.count("height")) cfg.height = std::stoi(kv["height"]);
                if (kv.count("refresh")) cfg.refreshRateMilliHz = std::stoi(kv["refresh"]);
                if (kv.count("hdr")) cfg.hdr = kv["hdr"] != "0";
                if (kv.count("primary")) cfg.primary = kv["primary"] != "0";
                if (kv.count("adapter")) {
                    try { cfg.adapterLuid = std::stoull(kv["adapter"]); } catch(...) {}
                }
                std::optional<GUID> guidOpt;
                if (kv.count("guid")) {
                    auto g = vdc::StringToGuid(kv["guid"]);
                    if (g) guidOpt = *g;
                }
                { std::lock_guard<std::mutex> lk(controllerMutex); cres = controller.CreateDisplay(cfg, guidOpt); }
            }
            else if (verb == "remove") {
                auto kv = ParseKvForm(payload);
                if (kv.count("guid")) {
                    auto g = vdc::StringToGuid(kv["guid"]);
                    if (g) {
                        // perform remove under lock and check count while locked
                        bool shouldShutdown = false;
                        {
                            std::lock_guard<std::mutex> lk(controllerMutex);
                            cres = controller.RemoveDisplay(*g);
                            if (cres.success && controller.CountDisplays() == 0) {
                                shouldShutdown = true;
                            }
                        }
                        if (shouldShutdown) {
                            // reply first, then request shutdown
                            WriteAllToPipe(hPipe, cres.json);
                            FlushFileBuffers(hPipe);
                            DisconnectNamedPipe(hPipe);
                            CloseHandle(hPipe);
                            g_running.store(false);
                            PostMessageW(hwnd, WM_CLOSE, 0, 0);
                            break;
                        }
                    } else {
                        cres = { false, "invalid guid", "{\"error\":\"invalid guid\"}" };
                    }
                } else {
                    cres = { false, "missing guid", "{\"error\":\"missing guid\"}" };
                }
            }
            else if (verb == "mode") {
                auto kv = ParseKvForm(payload);
                if (kv.count("guid") && kv.count("w") && kv.count("h") && kv.count("refresh") && kv.count("iso")) {
                    auto g = vdc::StringToGuid(kv["guid"]);
                    if (g) {
                        int w = std::stoi(kv["w"]);
                        int h = std::stoi(kv["h"]);
                        int refresh = std::stoi(kv["refresh"]);
                        bool iso = kv["iso"] == "1";
                        { std::lock_guard<std::mutex> lk(controllerMutex); cres = controller.SetMode(*g, w, h, refresh, iso); }
                    } else cres = { false, "invalid guid", "{\"error\":\"invalid guid\"}" };
                } else cres = { false, "missing params", "{\"error\":\"missing params\"}" };
            }
            else if (verb == "primary") {
                auto kv = ParseKvForm(payload);
                if (kv.count("guid")) {
                    auto g = vdc::StringToGuid(kv["guid"]);
                    if (g)
                        { std::lock_guard<std::mutex> lk(controllerMutex); cres = controller.SetPrimary(*g); }
                    else cres = { false, "invalid guid", "{\"error\":\"invalid guid\"}" };
                } else cres = { false, "missing guid", "{\"error\":\"missing guid\"}" };
            }
            else if (verb == "hdr") {
                auto kv = ParseKvForm(payload);
                if (kv.count("guid") && kv.count("enable")) {
                    auto g = vdc::StringToGuid(kv["guid"]);
                    if (g)
                        { std::lock_guard<std::mutex> lk(controllerMutex); cres = controller.SetHdr(*g, kv["enable"] == "1"); }
                    else cres = { false, "invalid guid", "{\"error\":\"invalid guid\"}" };
                } else cres = { false, "missing params", "{\"error\":\"missing params\"}" };
            }
            else if (verb == "query") {
                auto kv = ParseKvForm(payload);
                if (kv.count("guid")) {
                    auto g = vdc::StringToGuid(kv["guid"]);
                    if (g)
                        { std::lock_guard<std::mutex> lk(controllerMutex); cres = controller.Query(*g); }
                    else cres = { false, "invalid guid", "{\"error\":\"invalid guid\"}" };
                } else cres = { false, "missing guid", "{\"error\":\"missing guid\"}" };
            }
            else if (verb == "exit") {
                // graceful shutdown requested by external client
                cres = { true, "ok", "{\"success\":true}" };
                shouldShutdownFromPipe = true;
            }
            else {
                cres = { false, "unknown verb", "{\"error\":\"unknown verb\"}" };
            }

            // write response
            WriteAllToPipe(hPipe, cres.json);
            FlushFileBuffers(hPipe);
            // If client requested shutdown, reply then trigger server shutdown and break loop
            if (shouldShutdownFromPipe) {
                DisconnectNamedPipe(hPipe);
                CloseHandle(hPipe);
                g_running.store(false);
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                break;
            }

            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        } // while g_running

        if (psd) LocalFree(psd);
    }); // serverThread

    // Message loop (blocks until PostQuitMessage)
    MSG msg{};
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup tray icon
    Shell_NotifyIconW(NIM_DELETE, &nid);

    // Signal shutdown event to wake any ConnectNamedPipe overlapped waits.
    if (shutdownEvent) {
        SetEvent(shutdownEvent);
        CloseHandle(shutdownEvent);
    }

    // Ensure server thread stops
    g_running.store(false);
    if (serverThread.joinable()) {
        // Wait up to 5s for the server thread to exit after we signaled shutdownEvent.
        HANDLE th = serverThread.native_handle();
        DWORD wait = WaitForSingleObject(th, 5000); // 5s
        if (wait == WAIT_OBJECT_0) {
            serverThread.join();
        } else {
            std::cout << "Server thread did not exit in time; detaching and continuing shutdown.\n";
            serverThread.detach();
        }
    }

    // reset static context pointers
    g_ctx.controller = nullptr;
    g_ctx.controllerMutex = nullptr;
    g_ctx.menuMap = nullptr;

    return 0;
}
