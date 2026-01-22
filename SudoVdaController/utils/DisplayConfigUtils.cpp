#include "../pch.h"
#include "DisplayConfigUtils.h"

#include <windows.h>
#include <vector>
#include <string>
#include <iostream>
#include <cstdint>
#include <sstream>
#include <locale>
#include <codecvt>

// Add it RIGHT HERE
#ifndef SDC_SET_PRIMARY
#define SDC_SET_PRIMARY 0x00000010
#endif

// Optional: other missing flags
#ifndef SDC_TOPOLOGY_INTERNAL
#define SDC_TOPOLOGY_INTERNAL 0x00000001
#endif

#ifndef SDC_TOPOLOGY_CLONE
#define SDC_TOPOLOGY_CLONE 0x00000002
#endif

#ifndef SDC_TOPOLOGY_EXTEND
#define SDC_TOPOLOGY_EXTEND 0x00000004
#endif

#ifndef SDC_TOPOLOGY_EXTERNAL
#define SDC_TOPOLOGY_EXTERNAL 0x00000008
#endif

using namespace vdc;
using namespace vdisplay;

std::optional<DisplayMode> DisplayConfigUtils::GetCurrentModeForDevice(const std::wstring& deviceName) {
    if (deviceName.empty()) return std::nullopt;

    // Try QueryDisplayConfig first to obtain exact rational refresh rate (supports fractional)
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> pathArray(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modeArray(modeCount);

        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, pathArray.data(), &modeCount, modeArray.data(), nullptr) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < pathCount; ++i) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
                sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                sourceName.header.size = sizeof(sourceName);
                sourceName.header.adapterId = pathArray[i].sourceInfo.adapterId;
                sourceName.header.id = pathArray[i].sourceInfo.id;

                if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
                    continue;
                }

                if (std::wstring_view(sourceName.viewGdiDeviceName) == deviceName) {
                    auto* sourceInfo = &pathArray[i].sourceInfo;
                    auto* targetInfo = &pathArray[i].targetInfo;

                    // find the matching source mode in modeArray to get width/height
                    for (UINT32 j = 0; j < modeCount; ++j) {
                        if (modeArray[j].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
                            modeArray[j].adapterId.HighPart == sourceInfo->adapterId.HighPart &&
                            modeArray[j].adapterId.LowPart == sourceInfo->adapterId.LowPart &&
                            modeArray[j].id == sourceInfo->id) {

                            auto* sourceMode = &modeArray[j].sourceMode;

                            // targetInfo->refreshRate is a DISPLAYCONFIG_RATIONAL (Numerator/Denominator)
                            uint64_t numer = 0;
                            uint64_t denom = 1;
#if defined(_MSC_VER)
                            // field name differs by SDK, try common names
                            numer = targetInfo->refreshRate.Numerator;
                            denom = targetInfo->refreshRate.Denominator ? targetInfo->refreshRate.Denominator : 1;
#else
                            numer = targetInfo->refreshRate.Numerator;
                            denom = targetInfo->refreshRate.Denominator ? targetInfo->refreshRate.Denominator : 1;
#endif

                            // milliHz = (numerator / denominator) * 1000
                            uint64_t refreshMilliHz = (numer * 1000ULL) / denom;

                            return DisplayMode{
                                static_cast<int>(sourceMode->width),
                                static_cast<int>(sourceMode->height),
                                static_cast<int>(refreshMilliHz)
                            };
                        }
                    }
                }
            }
        }
    }

    // Fallback: EnumDisplaySettingsW returns integer Hz only (fractional precision lost)
    DEVMODEW devMode{};
    devMode.dmSize = sizeof(devMode);

    if (!EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &devMode)) {
        return std::nullopt;
    }

    int refreshMilliHz = (devMode.dmDisplayFrequency > 0) ? (devMode.dmDisplayFrequency * 1000) : 60000;
    return DisplayMode{
        static_cast<int>(devMode.dmPelsWidth),
        static_cast<int>(devMode.dmPelsHeight),
        refreshMilliHz
    };
}

bool DisplayConfigUtils::ApplyModeForDevice(const std::wstring& deviceName, int w, int h, int refreshMilliHz, bool isolatedLayout) {
    if (deviceName.empty()) return false;

    DEVMODEW devMode{};
    devMode.dmSize = sizeof(devMode);

    // Get current mode first to preserve other fields if possible
    if (!EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &devMode)) {
        return false;
    }

    devMode.dmPelsWidth = w;
    devMode.dmPelsHeight = h;

    // Convert milliHz to (integer) Hz for DEVMODE; use at least 1 Hz if odd value
    int hz = refreshMilliHz / 1000;
    if (hz <= 0) hz = 60;
    devMode.dmDisplayFrequency = hz;

    devMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

    // If isolatedLayout is requested, caller may expect we do not reposition displays here.
    // We use CDS_UPDATEREGISTRY | CDS_NORESET to stage change and then call global apply.
    LONG res = ChangeDisplaySettingsExW(deviceName.c_str(), &devMode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (res != DISP_CHANGE_SUCCESSFUL) {
        // Try applying immediately as a fallback
        res = ChangeDisplaySettingsExW(deviceName.c_str(), &devMode, nullptr, CDS_UPDATEREGISTRY, nullptr);
        if (res != DISP_CHANGE_SUCCESSFUL) {
            return false;
        }
    }

    // Commit staged changes
    res = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    return res == DISP_CHANGE_SUCCESSFUL;
}

bool DisplayConfigUtils::MakeDevicePrimary(const std::wstring& deviceName)
{
    if (deviceName.empty()) {
        return false;
    }

    UINT32 pathCount = 0, modeCount = 0;
    LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

    status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
        &modeCount, modes.data(), nullptr);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    // Find the path whose source GDI name matches deviceName
    DISPLAYCONFIG_PATH_INFO* primaryPath = nullptr;

    for (auto& path : paths)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name = {};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = path.sourceInfo.adapterId;
        name.header.id = path.sourceInfo.id;

        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS)
            continue;

        if (std::wstring(name.viewGdiDeviceName) == deviceName) {
            primaryPath = &path;
            break;
        }
    }

    if (!primaryPath) {
        return false;
    }

    // Helper to get a source mode for a given path, using modeInfoIdx safely
    auto getSourceModeForPath = [&](DISPLAYCONFIG_PATH_INFO& path) -> DISPLAYCONFIG_MODE_INFO*
        {
            if (path.sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
                return nullptr;

            if (path.sourceInfo.modeInfoIdx >= modeCount)
                return nullptr;

            DISPLAYCONFIG_MODE_INFO* m = &modes[path.sourceInfo.modeInfoIdx];
            if (m->infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
                return nullptr;

            return m;
        };

    // Set primary display's position to (0,0)
    DISPLAYCONFIG_MODE_INFO* primarySourceMode = getSourceModeForPath(*primaryPath);
    if (!primarySourceMode) {
        return false;
    }

    primarySourceMode->sourceMode.position.x = 0;
    primarySourceMode->sourceMode.position.y = 0;

    int primaryWidth = static_cast<int>(primarySourceMode->sourceMode.width);
    int nextX = primaryWidth;

    // Reposition all other displays to the right of the primary
    for (auto& path : paths)
    {
        if (&path == primaryPath)
            continue;

        DISPLAYCONFIG_MODE_INFO* srcMode = getSourceModeForPath(path);
        if (!srcMode)
            continue; // no source mode → skip

        srcMode->sourceMode.position.x = nextX;
        srcMode->sourceMode.position.y = 0;

        nextX += static_cast<int>(srcMode->sourceMode.width);
    }

    // Apply the new topology atomically
    status = SetDisplayConfig(
        pathCount,
        paths.data(),
        modeCount,
        modes.data(),
        SDC_APPLY |
        SDC_USE_SUPPLIED_DISPLAY_CONFIG |
        SDC_SAVE_TO_DATABASE
    );

    if (status != ERROR_SUCCESS) {
        return false;
    }

    return true;
}
