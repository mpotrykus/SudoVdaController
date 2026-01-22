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
#include <sddl.h>
#include <iostream>
#include "../Utils/JsonUtils.h"

using namespace vdc;

namespace {
    constexpr UINT WM_TRAYICON = WM_APP + 1;
    constexpr wchar_t WINDOW_CLASS_NAME[] = L"SudoVdaTrayWindowClass";
    constexpr UINT MENU_BASE_ID = 2000;
    constexpr UINT MENU_EXIT_ID = 1001;

    static std::atomic<bool> g_running{ false };

    enum class DisplayAction : int {
        None = 0,
        Details = 1,
        Remove = 2
    };

    struct MenuItem {
        GUID guid;
        DisplayAction action = DisplayAction::None;
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

                        // Details
                        AppendMenuW(hSub, MF_STRING, id, L"Details");
                        ctx->menuMap->emplace(id, MenuItem{ p.first, DisplayAction::Details });
                        ++id;

                        // Remove
                        AppendMenuW(hSub, MF_STRING, id, L"Remove");
                        ctx->menuMap->emplace(id, MenuItem{ p.first, DisplayAction::Remove });
                        ++id;

                        // Append popup submenu for this display with the label
                        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSub, label.c_str());
                    }
                    // Separator then Exit (append so displays appear above)
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
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
            else {
                cres = { false, "unknown verb", "{\"error\":\"unknown verb\"}" };
            }

            // write response
            WriteAllToPipe(hPipe, cres.json);
            FlushFileBuffers(hPipe);
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
