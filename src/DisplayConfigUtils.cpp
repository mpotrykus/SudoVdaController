#include "pch.h"
#include "DisplayConfigUtils.h"

#include <windows.h>
#include <vector>
#include <string>
#include <iostream>

using namespace vdc;
using namespace vdisplay;

std::optional<DisplayMode> DisplayConfigUtils::GetCurrentModeForDevice(const std::wstring& deviceName) {
    if (deviceName.empty()) return std::nullopt;

    DEVMODEW devMode{};
    devMode.dmSize = sizeof(devMode);

    if (!EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &devMode)) {
        return std::nullopt;
    }

    // dmDisplayFrequency is in Hz (integer). Convert to milliHz.
    int refreshMilliHz = (devMode.dmDisplayFrequency > 0) ? (devMode.dmDisplayFrequency * 1000) : 60000;
    return DisplayMode{static_cast<int>(devMode.dmPelsWidth), static_cast<int>(devMode.dmPelsHeight), refreshMilliHz};
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

bool DisplayConfigUtils::MakeDevicePrimary(const std::wstring& deviceName) {
    if (deviceName.empty()) return false;

    // Get the DEVMODE of the device we want as primary (to get its offset)
    DEVMODEW primaryDevMode{};
    primaryDevMode.dmSize = sizeof(primaryDevMode);
    if (!EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &primaryDevMode)) {
        return false;
    }

    int offset_x = primaryDevMode.dmPosition.x;
    int offset_y = primaryDevMode.dmPosition.y;

    // Iterate all display devices and adjust their positions relative to the new primary
    DISPLAY_DEVICEW displayDevice{};
    displayDevice.cb = sizeof(displayDevice);

    int device_index = 0;
    std::vector<std::pair<std::wstring, DEVMODEW>> staged;

    // Collect and adjust modes first
    while (EnumDisplayDevicesW(nullptr, device_index, &displayDevice, 0)) {
        device_index++;

        // Only consider active devices
        if (!(displayDevice.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
            displayDevice.cb = sizeof(displayDevice);
            continue;
        }

        DEVMODEW devMode{};
        devMode.dmSize = sizeof(devMode);
        if (!EnumDisplaySettingsW(displayDevice.DeviceName, ENUM_CURRENT_SETTINGS, &devMode)) {
            displayDevice.cb = sizeof(displayDevice);
            continue;
        }

        // Adjust positions: shift everything so chosen device becomes 0,0
        devMode.dmPosition.x -= offset_x;
        devMode.dmPosition.y -= offset_y;
        devMode.dmFields = devMode.dmFields | DM_POSITION;

        staged.emplace_back(std::wstring(displayDevice.DeviceName), devMode);

        displayDevice.cb = sizeof(displayDevice);
    }

    // Apply all non-primary updates first with CDS_UPDATEREGISTRY | CDS_NORESET
    for (auto &p : staged) {
        const std::wstring &name = p.first;
        DEVMODEW &dm = p.second;

        // If this is the device to be primary, skip here, we'll apply with CDS_SET_PRIMARY below.
        if (name == deviceName) continue;

        LONG res = ChangeDisplaySettingsExW(name.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        if (res != DISP_CHANGE_SUCCESSFUL) {
            return false;
        }
    }

    // Now set the chosen device as primary at (0,0)
    DEVMODEW primaryDm{};
    primaryDm.dmSize = sizeof(primaryDm);
    if (!EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &primaryDm)) {
        return false;
    }
    primaryDm.dmPosition.x = 0;
    primaryDm.dmPosition.y = 0;
    primaryDm.dmFields = primaryDm.dmFields | DM_POSITION;

    LONG res = ChangeDisplaySettingsExW(deviceName.c_str(), &primaryDm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET | CDS_SET_PRIMARY, nullptr);
    if (res != DISP_CHANGE_SUCCESSFUL) {
        return false;
    }

    // Commit all staged changes
    res = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    return res == DISP_CHANGE_SUCCESSFUL;
}
