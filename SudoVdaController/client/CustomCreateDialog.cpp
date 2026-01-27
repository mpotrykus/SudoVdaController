#include "../pch.h"
#include "CustomCreateDialog.h"

#include <string>
#include <codecvt>
#include <locale>

namespace vdc {

    namespace {

        // Control IDs
        constexpr int IDC_NAME = 1001;
        constexpr int IDC_WIDTH = 1002;
        constexpr int IDC_HEIGHT = 1003;
        constexpr int IDC_REFRESH = 1004;
        constexpr int IDC_HDR = 1005;
        constexpr int IDC_CREATE = 1006;
        constexpr int IDC_CANCEL = 1007;

        struct CustomDialogState {
            HWND dlg = nullptr;
            HWND eName = nullptr;
            HWND eWidth = nullptr;
            HWND eHeight = nullptr;
            HWND eRefresh = nullptr;
            HWND hdr = nullptr;

            VirtualDisplay out;
            bool ok = false;
        };

        LRESULT CALLBACK CustomCreateDlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
            CustomDialogState* st = reinterpret_cast<CustomDialogState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

            switch (msg) {

            case WM_COMMAND: {
                int id = LOWORD(wParam);
                if (!st) break;

                if (id == IDC_CREATE) {
                    wchar_t buf[256];

                    // Name
                    GetWindowTextW(st->eName, buf, _countof(buf));
                    st->out.deviceName = buf;

                    // Width
                    GetWindowTextW(st->eWidth, buf, _countof(buf));
                    try { st->out.width = std::stoi(std::wstring(buf)); }
                    catch (...) {}

                    // Height
                    GetWindowTextW(st->eHeight, buf, _countof(buf));
                    try { st->out.height = std::stoi(std::wstring(buf)); }
                    catch (...) {}

                    // Refresh
                    GetWindowTextW(st->eRefresh, buf, _countof(buf));
                    try {
                        std::wstring s(buf);
                        if (s.find(L'.') != std::wstring::npos) {
                            double hz = std::stod(s);
                            st->out.refreshRateMilliHz = static_cast<int>(hz * 1000.0 + 0.5);
                        }
                        else {
                            long long v = std::stoll(s);
                            if (v < 1000)
                                st->out.refreshRateMilliHz = static_cast<int>(v * 1000);
                            else
                                st->out.refreshRateMilliHz = static_cast<int>(v);
                        }
                    }
                    catch (...) {}

                    // HDR
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

    } // anonymous namespace

    // -----------------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------------
    bool ShowCustomCreateDialog(HWND parent, VirtualDisplay& outCfg) {
        auto* st = new CustomDialogState();

        const int W = 380;
        const int H = 220;

        st->dlg = CreateWindowExW(
            0,
            L"Static",
            L"Create Display",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT,
            W, H,
            parent,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr
        );

        if (!st->dlg) {
            delete st;
            return false;
        }

        // Layout
        int x = 12, y = 12;
        int labelW = 80, editW = 260, h = 22, gap = 6;

        CreateWindowExW(0, L"Static", L"Name:", WS_CHILD | WS_VISIBLE,
            x, y, labelW, h, st->dlg, nullptr, nullptr, nullptr);

        st->eName = CreateWindowExW(0, L"Edit", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | WS_TABSTOP,
            x + labelW, y, editW, h, st->dlg, (HMENU)IDC_NAME, nullptr, nullptr);

        y += h + gap;

        CreateWindowExW(0, L"Static", L"Width:", WS_CHILD | WS_VISIBLE,
            x, y, labelW, h, st->dlg, nullptr, nullptr, nullptr);

        st->eWidth = CreateWindowExW(0, L"Edit", L"1920", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | WS_TABSTOP,
            x + labelW, y, 100, h, st->dlg, (HMENU)IDC_WIDTH, nullptr, nullptr);

        CreateWindowExW(0, L"Static", L"Height:", WS_CHILD | WS_VISIBLE,
            x + labelW + 110, y, 60, h, st->dlg, nullptr, nullptr, nullptr);

        st->eHeight = CreateWindowExW(0, L"Edit", L"1080", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | WS_TABSTOP,
            x + labelW + 170, y, 90, h, st->dlg, (HMENU)IDC_HEIGHT, nullptr, nullptr);

        y += h + gap;

        CreateWindowExW(0, L"Static", L"Refresh:", WS_CHILD | WS_VISIBLE,
            x, y, labelW, h, st->dlg, nullptr, nullptr, nullptr);

        st->eRefresh = CreateWindowExW(0, L"Edit", L"60", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | WS_TABSTOP,
            x + labelW, y, 100, h, st->dlg, (HMENU)IDC_REFRESH, nullptr, nullptr);

        st->hdr = CreateWindowExW(0, L"Button", L"HDR",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            x + labelW + 110, y, 80, h,
            st->dlg, (HMENU)IDC_HDR, nullptr, nullptr);

        // Buttons
        CreateWindowExW(0, L"Button", L"Create",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            W - 200, H - 75, 80, 26,
            st->dlg, (HMENU)IDC_CREATE, nullptr, nullptr);

        CreateWindowExW(0, L"Button", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            W - 110, H - 75, 80, 26,
            st->dlg, (HMENU)IDC_CANCEL, nullptr, nullptr);

        // Subclass
        SetWindowLongPtrW(st->dlg, GWLP_USERDATA, (LONG_PTR)st);
        SetWindowLongPtrW(st->dlg, GWLP_WNDPROC, (LONG_PTR)CustomCreateDlgProc);

        // Center on screen
        int sx = GetSystemMetrics(SM_CXSCREEN);
        int sy = GetSystemMetrics(SM_CYSCREEN);
        int px = sx / 2 - W / 2;
        int py = sy / 2 - H / 2;
        if (px < 0) px = 0;
        if (py < 0) py = 0;

        SetWindowPos(st->dlg, nullptr, px, py, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

        EnableWindow(parent, FALSE);
        ShowWindow(st->dlg, SW_SHOW);
        SetFocus(st->eName);

        // Modal loop
        MSG msg{};
        while (IsWindow(st->dlg)) {
            if (!GetMessageW(&msg, nullptr, 0, 0))
                break;

            if (!IsDialogMessageW(st->dlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        bool ok = st->ok;
        if (ok)
            outCfg = st->out;

        EnableWindow(parent, TRUE);
        delete st;

        return ok;
    }

} // namespace vdc
