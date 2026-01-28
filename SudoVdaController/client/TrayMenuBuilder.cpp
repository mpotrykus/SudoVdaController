#include "../pch.h"
#include "TrayMenuBuilder.h"

#include "../controller/VirtualDisplayservice.h"
#include "../utils/GuidUtils.h"
#include "../utils/Logger.h"

#include <codecvt>
#include <locale>
#include <string>
#include <vector>
#include "../utils/StringUtils.h"

namespace vdc {

    namespace {

        constexpr UINT MENU_BASE_ID = 2000;
        constexpr UINT MENU_EXIT_ID = 1001;
        constexpr UINT MENU_CLEAR_CONFIG_ID = 1999;

        static std::wstring ExtractGdiName(const std::wstring& label) {
            size_t l = label.rfind(L'(');
            size_t r = label.rfind(L')');
            if (l != std::wstring::npos && r != std::wstring::npos && r > l)
                return label.substr(l + 1, r - l - 1);
            return L"";
        }

    } // anonymous namespace

    // -----------------------------------------------------------------------------
    // Build and show the tray popup menu
    // -----------------------------------------------------------------------------
    void ShowTrayMenu(HWND hWnd, TrayContext* ctx) {
        if (!ctx || !ctx->service || !ctx->serviceMutex || !ctx->menuMap)
            return;

        HMENU hMenu = CreatePopupMenu();
        if (!hMenu)
            return;

        std::lock_guard<std::mutex> lk(*ctx->serviceMutex);

        auto list = ctx->service->ListDisplays();
        ctx->menuMap->clear();

        UINT id = MENU_BASE_ID;

        // -------------------------------------------------------------------------
        // Build per‑display submenus
        // -------------------------------------------------------------------------
        for (const auto& p : list) {
            const GUID& guid = p.first;
            const std::wstring& label = p.second;

            bool isPhysical = (guid == GUID());
            const std::wstring gdiName = ExtractGdiName(label);

            HMENU hSub = CreatePopupMenu();
            if (!hSub) continue;

            // Set Primary
            {
                AppendMenuW(hSub, MF_STRING, id, L"Set Primary");
                MenuItem mi{};
                mi.guid = guid;
                mi.action = DisplayAction::SetPrimary;

                if (isPhysical) {
                    mi.gdiName = gdiName;
                    mi.physicalLabel = label;
                }

                ctx->menuMap->emplace(id, mi);
                ++id;
            }

            // Toggle HDR
            if (ctx->service->DisplaySupportsHdr(gdiName)) {
                MenuItem mi{};
                mi.guid = guid;

                if (isPhysical) {
                    mi.gdiName = gdiName;
                    mi.physicalLabel = label;
                }

                if (ctx->service->IsHdrEnabled(gdiName)) {
                    AppendMenuW(hSub, MF_STRING, id, L"Disable HDR");
                    mi.action = DisplayAction::ToggleHdr;
                }
                else {
                    AppendMenuW(hSub, MF_STRING, id, L"Enable HDR");
                    mi.action = DisplayAction::ToggleHdr;
                }

                ctx->menuMap->emplace(id, mi);
                ++id;
            }

            AppendMenuW(hSub, MF_SEPARATOR, 0, nullptr);

            // Details
            {
                AppendMenuW(hSub, MF_STRING, id, L"Details");
                MenuItem mi{};
                mi.guid = guid;
                mi.action = DisplayAction::Details;

                mi.gdiName = gdiName;
                mi.physicalLabel = label;

                ctx->menuMap->emplace(id, mi);
                ++id;
            }

            // Remove (virtual only)
            if (!isPhysical) {
                AppendMenuW(hSub, MF_STRING, id, L"Remove");
                ctx->menuMap->emplace(id, MenuItem{ guid, DisplayAction::Remove });
                ++id;
            }

            // Attach submenu to main menu
            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSub, label.c_str());
        }

        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

        // -------------------------------------------------------------------------
        // Add Display submenu
        // -------------------------------------------------------------------------
        HMENU hAddRoot = CreatePopupMenu();
        if (hAddRoot) {
            std::vector<std::string> resolutions = {
                "3840x2160", "2560x1440", "1920x1080", "1280x720"
            };

            std::vector<std::string> refreshRates = {
                "240hz", "144hz", "120hz", "119.97hz", "60hz", "59.94hz", "24hz"
            };

            std::vector<std::string> colorSpace = { "HDR", "SDR" };

            for (const auto& res : resolutions) {
                HMENU hRateMenu = CreatePopupMenu();
                if (!hRateMenu) continue;

                std::wstring wRes = StringToWString(res);

                for (const auto& rate : refreshRates) {
                    HMENU hCsMenu = CreatePopupMenu();
                    if (!hCsMenu) continue;

                    std::wstring wRate = StringToWString(rate);

                    for (const auto& cs : colorSpace) {
                        std::wstring wCs = StringToWString(cs);

                        AppendMenuW(hCsMenu, MF_STRING, id, wCs.c_str());

                        VirtualDisplay cfg;

                        // Parse resolution
                        try {
                            auto x = res.find('x');
                            if (x != std::string::npos) {
                                cfg.width = std::stoi(res.substr(0, x));
                                cfg.height = std::stoi(res.substr(x + 1));
                            }
                        }
                        catch (...) {}

                        // Parse refresh rate
                        try {
                            std::string clean;
                            for (char c : rate)
                                if ((c >= '0' && c <= '9') || c == '.')
                                    clean.push_back(c);

                            if (!clean.empty()) {
                                double hz = std::stod(clean);
                                cfg.refreshRateMilliHz = static_cast<int>(hz * 1000.0);
                            }
                        }
                        catch (...) {}

                        cfg.hdr = (cs == "HDR");

                        ctx->menuMap->emplace(id, MenuItem{ GUID(), DisplayAction::Add, cfg });
                        ++id;
                    }

                    AppendMenuW(hRateMenu, MF_POPUP, (UINT_PTR)hCsMenu, wRate.c_str());
                }

                AppendMenuW(hAddRoot, MF_POPUP, (UINT_PTR)hRateMenu, wRes.c_str());
            }

            // Custom option
            AppendMenuW(hAddRoot, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hAddRoot, MF_STRING, id, L"Custom");
            ctx->menuMap->emplace(id, MenuItem{ GUID(), DisplayAction::Custom });
            ++id;

            AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hAddRoot, L"Add display");
        }

        // Clear config
        AppendMenuW(hMenu, MF_STRING, MENU_CLEAR_CONFIG_ID, L"Clear display config");

        // Exit
        AppendMenuW(hMenu, MF_STRING, MENU_EXIT_ID, L"Exit");

        // Show menu
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hWnd);
        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);

        DestroyMenu(hMenu);
    }

} // namespace vdc
