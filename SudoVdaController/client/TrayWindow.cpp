#include "../pch.h"
#include "TrayWindow.h"

#include "../utils/Logger.h"
#include "TrayMenuBuilder.h"
#include "TrayActions.h"
#include "../resource.h"

#include <shellapi.h>

namespace vdc {

    // Forward declaration of the global context instance defined in TrayServer.cpp
    extern TrayContext g_ctx;

    namespace {

        constexpr UINT WM_TRAYICON = WM_APP + 1;
        constexpr wchar_t WINDOW_CLASS_NAME[] = L"SudoVdaTrayWindowClass";

        // Forward declaration of global context (owned by TrayServer.cpp)
        // (do not declare it inside the anonymous namespace - that would give it internal linkage)

        // Window procedure for the tray window
        LRESULT CALLBACK TrayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
            switch (msg) {

            case WM_TRAYICON: {
                if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                    // Build and show the dynamic popup menu
                    ShowTrayMenu(hWnd, &g_ctx);
                    return 0;
                }
                break;
            }

            case WM_COMMAND: {
                UINT cmd = LOWORD(wParam);
                HandleTrayCommand(hWnd, cmd, &g_ctx);
                return 0;
            }

            case WM_CLOSE:
                DestroyWindow(hWnd);
                return 0;

            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            }

            return DefWindowProcW(hWnd, msg, wParam, lParam);
        }

        // Registers the window class if not already registered
        bool RegisterTrayWindowClass(HINSTANCE hInst) {
            static bool registered = false;
            if (registered) return true;

            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = TrayWndProc;
            wc.hInstance = hInst;
            wc.lpszClassName = WINDOW_CLASS_NAME;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

            if (!RegisterClassExW(&wc)) {
                LOG_ERROR("Failed to register tray window class.");
                return false;
            }

            registered = true;
            return true;
        }

        // Adds the tray icon
        bool AddTrayIcon(HWND hWnd) {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hWnd;
            nid.uID = 1;
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            nid.uCallbackMessage = WM_TRAYICON;

            HICON hIcon = nullptr;
            bool ownedIcon = false;
            HINSTANCE hInst = GetModuleHandleW(nullptr);

            hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_MAINICON));

            nid.hIcon = hIcon;
            wcscpy_s(nid.szTip, L"SudoVDA Controller");

            if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
                LOG_ERROR("Failed to add tray icon.");
                if (ownedIcon && hIcon) DestroyIcon(hIcon);
                return false;
            }

            // The shell makes its own copy of the icon. Destroy our owned icon handle if we created it.
            if (ownedIcon && hIcon) {
                DestroyIcon(hIcon);
            }

            return true;
        }

        // Removes the tray icon
        void RemoveTrayIcon(HWND hWnd) {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hWnd;
            nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);
        }

    } // anonymous namespace

    // Forward declaration of the global context instance defined in TrayServer.cpp
    extern TrayContext g_ctx;

    // Public API: Create the tray window
    HWND CreateTrayWindow(TrayContext* ctx) {
        HINSTANCE hInst = GetModuleHandleW(nullptr);

        if (!RegisterTrayWindowClass(hInst))
            return nullptr;

        HWND hWnd = CreateWindowExW(
            0,
            WINDOW_CLASS_NAME,
            L"SudoVDA Tray",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            300, 200,
            nullptr,
            nullptr,
            hInst,
            nullptr
        );

        if (!hWnd) {
            LOG_ERROR("Failed to create tray window.");
            return nullptr;
        }

        // Hide the window — tray apps don't show a UI
        ShowWindow(hWnd, SW_HIDE);

        if (!AddTrayIcon(hWnd)) {
            DestroyWindow(hWnd);
            return nullptr;
        }

        return hWnd;
    }

    // Public API: Destroy tray window + icon
    void DestroyTrayWindow(HWND hWnd) {
        if (!hWnd) return;
        RemoveTrayIcon(hWnd);
        DestroyWindow(hWnd);
    }

} // namespace vdc
