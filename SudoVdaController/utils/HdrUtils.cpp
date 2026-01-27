#include "../pch.h"
#include "HdrUtils.h"

#include <windows.h>
#include <vector>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using namespace vdc;
using Microsoft::WRL::ComPtr;

// Helper: find adapter LUID and target id for a given GDI device name
static bool findDisplayIds(const wchar_t* displayName, LUID& adapterId, uint32_t& targetId) {
    // Try active paths first (fastest, common case)
    for (int mode = 0; mode < 2; ++mode) {
        const UINT32 flags = (mode == 0) ? QDC_ONLY_ACTIVE_PATHS : QDC_ALL_PATHS;
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount) != ERROR_SUCCESS) {
            continue;
        }

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(flags, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
            continue;
        }

        for (UINT32 i = 0; i < pathCount; ++i) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
            sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof(sourceName);
            sourceName.header.adapterId = paths[i].sourceInfo.adapterId;
            sourceName.header.id = paths[i].sourceInfo.id;

            if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
                continue;
            }

            if (std::wstring_view(sourceName.viewGdiDeviceName) == std::wstring_view(displayName)) {
                adapterId = paths[i].sourceInfo.adapterId;
                targetId = paths[i].targetInfo.id;
                return true;
            }
        }
    }

    return false;
}

// Helper: query HDR state for a GDI device name via DXGI (uses IDXGIOutput6 when available)
static bool getDisplayHDRByName(const wchar_t* displayName) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    for (UINT adapterIdx = 0;; ++adapterIdx) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapters1(adapterIdx, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;

        for (UINT outputIdx = 0;; ++outputIdx) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(outputIdx, output.GetAddressOf());
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr) || !output) continue;

            DXGI_OUTPUT_DESC desc = {};
            if (FAILED(output->GetDesc(&desc))) continue;

            MONITORINFOEXW mi = {};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(desc.Monitor, &mi)) continue;

            if (std::wstring_view(mi.szDevice) == std::wstring_view(displayName)) {
                ComPtr<IDXGIOutput6> output6;
                if (SUCCEEDED(output.As(&output6)) && output6) {
                    DXGI_OUTPUT_DESC1 desc1 = {};
                    if (SUCCEEDED(output6->GetDesc1(&desc1))) {
                        return desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    }
                }
                // Matched the output but cannot query IDXGIOutput6 -> consider HDR disabled
                return false;
            }
        }
    }

    return false;
}

bool HdrUtils::SetHdrState(const std::wstring& deviceName, bool enable) {
    if (deviceName.empty()) {
        // If no device specified, try to use primary display name via EnumDisplayDevices
        DISPLAY_DEVICEW displayDevice{};
        displayDevice.cb = sizeof(displayDevice);
        int idx = 0;
        while (EnumDisplayDevicesW(NULL, idx++, &displayDevice, 0)) {
            if (displayDevice.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
                return SetHdrState(displayDevice.DeviceName, enable);
            }
        }
        return false;
    }

    LUID adapterId = {};
    uint32_t targetId = 0;
    if (!findDisplayIds(deviceName.c_str(), adapterId, targetId)) {
        return false;
    }

    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE setHdrInfo = {};
    setHdrInfo.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    setHdrInfo.header.size = sizeof(setHdrInfo);
    setHdrInfo.header.adapterId = adapterId;
    setHdrInfo.header.id = targetId;
    setHdrInfo.enableAdvancedColor = enable ? TRUE : FALSE;

    return DisplayConfigSetDeviceInfo(&setHdrInfo.header) == ERROR_SUCCESS;
}

bool HdrUtils::IsHdrEnabled(const std::wstring& deviceName) {
    if (deviceName.empty()) {
        // find primary
        DISPLAY_DEVICEW displayDevice{};
        displayDevice.cb = sizeof(displayDevice);
        int idx = 0;
        while (EnumDisplayDevicesW(NULL, idx++, &displayDevice, 0)) {
            if (displayDevice.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
                return getDisplayHDRByName(displayDevice.DeviceName);
            }
        }
        return false;
    }

    return getDisplayHDRByName(deviceName.c_str());
}
