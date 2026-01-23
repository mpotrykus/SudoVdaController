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
#include <setupapi.h>
#include <cfgmgr32.h>
#pragma comment(lib, "setupapi.lib")
// WMI
#include <comdef.h>
#include <wbemidl.h>
#pragma comment(lib, "wbemuuid.lib")
// Physical monitor API
#include <PhysicalMonitorEnumerationAPI.h>
#pragma comment(lib, "Dxva2.lib")

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
// For ApplyTopologyFromStore
#include <map>

// Helper: read MonitorFriendlyDeviceName from registry for a device instance id
#include <optional>

// Implementation of ApplyTopologyFromStore is placed after helper functions to avoid mixing
// inside the middle of GetMonitorFriendlyNameFromDeviceInstanceId implementation.

std::optional<std::wstring> DisplayConfigUtils::GetMonitorFriendlyNameFromDeviceInstanceId(const std::wstring& deviceInstanceId) {
    if (deviceInstanceId.empty()) return std::nullopt;

    // Try WMI WmiMonitorID -> UserFriendlyName
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool coInit = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) coInit = false;
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
        CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, NULL, EOAC_NONE, NULL);
        IWbemLocator *pLoc = NULL;
        if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID *)&pLoc))) {
            IWbemServices *pSvc = NULL;
            if (SUCCEEDED(pLoc->ConnectServer(_bstr_t(L"ROOT\\WMI"), NULL, NULL, 0, NULL, 0, 0, &pSvc))) {
                CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                    RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

                IEnumWbemClassObject* pEnumerator = NULL;
                HRESULT qhr = pSvc->ExecQuery(_bstr_t(L"WQL"), _bstr_t(L"SELECT InstanceName, UserFriendlyName FROM WmiMonitorID"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator);
                if (SUCCEEDED(qhr) && pEnumerator) {
                    IWbemClassObject *pObj = NULL;
                    while (pEnumerator->Next(WBEM_INFINITE, 1, &pObj, NULL) == S_OK) {
                        VARIANT varInst; VariantInit(&varInst);
                        if (SUCCEEDED(pObj->Get(L"InstanceName", 0, &varInst, NULL, NULL)) && varInst.vt == VT_BSTR) {
                            std::wstring inst = varInst.bstrVal ? std::wstring(varInst.bstrVal) : L"";
                            std::wstring a = inst; std::wstring b = deviceInstanceId;
                            for (auto &c : a) c = towlower(c);
                            for (auto &c : b) c = towlower(c);
                            if (!b.empty() && (a.find(b) != std::wstring::npos || b.find(a) != std::wstring::npos)) {
                                VARIANT varName; VariantInit(&varName);
                                if (SUCCEEDED(pObj->Get(L"UserFriendlyName", 0, &varName, NULL, NULL)) && (varName.vt & VT_ARRAY)) {
                                    SAFEARRAY* psa = varName.parray;
                                    if (psa) {
                                        LONG lBound=0, uBound=0;
                                        SafeArrayGetLBound(psa, 1, &lBound);
                                        SafeArrayGetUBound(psa, 1, &uBound);
                                        UINT count = (UINT)(uBound - lBound + 1);
                                        USHORT *data = NULL;
                                        if (SUCCEEDED(SafeArrayAccessData(psa, (void**)&data))) {
                                            std::wstring name;
                                            for (UINT i = 0; i < count; ++i) { USHORT ch = data[i]; if (ch == 0) break; name.push_back((wchar_t)ch); }
                                            SafeArrayUnaccessData(psa);
                                            VariantClear(&varName);
                                            VariantClear(&varInst);
                                            pObj->Release(); pEnumerator->Release(); pSvc->Release(); pLoc->Release();
                                            if (coInit) CoUninitialize();
                                            return name;
                                        }


                                    }
                                }
                                VariantClear(&varName);
                            }
                        }
                        VariantClear(&varInst);
                        pObj->Release();
                    }
                    pEnumerator->Release();
                }
                pSvc->Release();
            }
            pLoc->Release();
        }
        if (coInit) CoUninitialize();
    }

    // SetupAPI: try to open device by instance id
    HDEVINFO devs = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devs != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA devInfo{}; devInfo.cbSize = sizeof(devInfo);
        if (SetupDiOpenDeviceInfoW(devs, deviceInstanceId.c_str(), NULL, 0, &devInfo)) {
            DWORD req = 0; wchar_t buf[512];
            if (SetupDiGetDeviceRegistryPropertyW(devs, &devInfo, SPDRP_FRIENDLYNAME, NULL, (PBYTE)buf, sizeof(buf), &req)) { SetupDiDestroyDeviceInfoList(devs); return std::wstring(buf); }
            if (SetupDiGetDeviceRegistryPropertyW(devs, &devInfo, SPDRP_DEVICEDESC, NULL, (PBYTE)buf, sizeof(buf), &req)) { SetupDiDestroyDeviceInfoList(devs); return std::wstring(buf); }
        } else {
            // enumerate and match instance id loosely
            DWORD idx = 0; while (SetupDiEnumDeviceInfo(devs, idx++, &devInfo)) {
                wchar_t instBuf[512]; if (SetupDiGetDeviceInstanceIdW(devs, &devInfo, instBuf, (DWORD)sizeof(instBuf)/sizeof(wchar_t), NULL)) {
                    std::wstring instId = instBuf; std::wstring a = instId, b = deviceInstanceId; for (auto &c : a) c = towlower(c); for (auto &c : b) c = towlower(c);
                    if (a.find(b) != std::wstring::npos || b.find(a) != std::wstring::npos) {
                        DWORD req2 = 0; wchar_t buf2[512];
                        if (SetupDiGetDeviceRegistryPropertyW(devs, &devInfo, SPDRP_FRIENDLYNAME, NULL, (PBYTE)buf2, sizeof(buf2), &req2)) { SetupDiDestroyDeviceInfoList(devs); return std::wstring(buf2); }
                        if (SetupDiGetDeviceRegistryPropertyW(devs, &devInfo, SPDRP_DEVICEDESC, NULL, (PBYTE)buf2, sizeof(buf2), &req2)) { SetupDiDestroyDeviceInfoList(devs); return std::wstring(buf2); }
                    }
                }
            }
        }
        SetupDiDestroyDeviceInfoList(devs);
    }

    // Registry EDID fallback
    HKEY hKey = NULL;
    std::wstring sub = L"SYSTEM\\CurrentControlSet\\Enum\\" + deviceInstanceId + L"\\Device Parameters";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) return std::nullopt;
    DWORD type = 0; wchar_t buf[512]; DWORD cb = sizeof(buf);
    if (RegQueryValueExW(hKey, L"FriendlyName", NULL, &type, reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS && type == REG_SZ) { RegCloseKey(hKey); return std::wstring(buf); }
    cb = 0; if (RegQueryValueExW(hKey, L"EDID", NULL, &type, NULL, &cb) != ERROR_SUCCESS || type != REG_BINARY) { RegCloseKey(hKey); return std::nullopt; }
    std::vector<BYTE> edid(cb); if (RegQueryValueExW(hKey, L"EDID", NULL, &type, edid.data(), &cb) != ERROR_SUCCESS) { RegCloseKey(hKey); return std::nullopt; }
    RegCloseKey(hKey);
    if (edid.size() < 128) return std::nullopt;
    const size_t bases[4] = {54,72,90,108};
    for (int d=0; d<4; ++d) {
        size_t off = bases[d]; if (off + 18 <= edid.size()) {
            if (edid[off] == 0x00 && edid[off+3] == 0xFC) {
                std::wstring name; for (size_t k = off+5; k < off+18; ++k) { BYTE c = edid[k]; if (c==0x0A || c==0x00) break; name.push_back((wchar_t)c); }
                if (!name.empty()) return name;
            }
        }
    }

    return std::nullopt;
}

// Implementation of ApplyTopologyFromStore
bool DisplayConfigUtils::ApplyTopologyFromStore(const std::map<std::string,bool>& topologyMap) {
    if (topologyMap.empty()) return true;

    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return false;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return false;

    std::vector<DISPLAYCONFIG_PATH_INFO> newPaths;
    newPaths.reserve(pathCount);

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) {
            // keep path if we cannot query name
            newPaths.push_back(paths[i]);
            continue;
        }

        // convert to utf8 for map lookup
        std::wstring gdi = src.viewGdiDeviceName;
        std::string gdiUtf;
        try {
            gdiUtf = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(gdi);
        } catch(...) { gdiUtf = ""; }

        auto it = topologyMap.find(gdiUtf);
        if (it != topologyMap.end() && it->second == false) {
            // skip this path to disable this GDI device
            continue;
        }
        newPaths.push_back(paths[i]);
    }

    // If nothing changed, nothing to apply
    if (newPaths.size() == pathCount) return true;

    LONG status = SetDisplayConfig(
        static_cast<UINT32>(newPaths.size()), newPaths.data(),
        modeCount, modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE
    );

    return status == ERROR_SUCCESS;
}

std::optional<std::wstring> DisplayConfigUtils::GetMonitorFriendlyNameForGdiName(const std::wstring& gdiName) {
    if (gdiName.empty()) return std::nullopt;

    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return std::nullopt;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return std::nullopt;

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (std::wstring(src.viewGdiDeviceName) != gdiName) continue;

        DISPLAYCONFIG_TARGET_DEVICE_NAME tname{};
        tname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tname.header.size = sizeof(tname);
        tname.header.adapterId = paths[i].targetInfo.adapterId;
        tname.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tname.header) == ERROR_SUCCESS) {
            if (tname.monitorFriendlyDeviceName && tname.monitorFriendlyDeviceName[0]) {
                return std::wstring(tname.monitorFriendlyDeviceName);
            }
            if (tname.monitorDevicePath && tname.monitorDevicePath[0]) {
                std::wstring inst = tname.monitorDevicePath;
                const std::wstring prefix = L"\\\\?\\";
                if (inst.rfind(prefix, 0) == 0) inst = inst.substr(prefix.size());
                auto reg = GetMonitorFriendlyNameFromDeviceInstanceId(inst);
                if (reg) return reg;
                std::wstring alt = inst;
                for (auto &c : alt) if (c == L'#') c = L'\\';
                reg = GetMonitorFriendlyNameFromDeviceInstanceId(alt);
                if (reg) return reg;
            }
        }
    }

    // Fallback: enumerate monitor device interfaces and match to GDI name
    GUID GUID_DEVINTERFACE_MONITOR = { 0xE6F07B5F, 0xEE97, 0x4a90, { 0xB0, 0x76, 0x33, 0xF5, 0x7B, 0xF4, 0xEA, 0xA7 } };
    HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_MONITOR, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (di != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA ifdata{}; ifdata.cbSize = sizeof(ifdata);
        DWORD idx = 0;
        while (SetupDiEnumDeviceInterfaces(di, NULL, &GUID_DEVINTERFACE_MONITOR, idx++, &ifdata)) {
            DWORD req = 0;
            SetupDiGetDeviceInterfaceDetailW(di, &ifdata, NULL, 0, &req, NULL);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) continue;
            std::vector<BYTE> detail(req);
            PSP_DEVICE_INTERFACE_DETAIL_DATA_W pdetail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detail.data());
            pdetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA devinfo{}; devinfo.cbSize = sizeof(devinfo);
            if (!SetupDiGetDeviceInterfaceDetailW(di, &ifdata, pdetail, req, NULL, &devinfo)) continue;
            std::wstring devicePath = pdetail->DevicePath ? std::wstring(pdetail->DevicePath) : std::wstring();
            if (!devicePath.empty()) {
                std::wstring dp = devicePath; for (auto &c : dp) c = towlower(c);
                std::wstring gn = gdiName; for (auto &c: gn) c = towlower(c);
                if (dp.find(gn) != std::wstring::npos || gn.find(dp) != std::wstring::npos) {
                    wchar_t buf[512]; DWORD cb = sizeof(buf);
                    if (SetupDiGetDeviceRegistryPropertyW(di, &devinfo, SPDRP_FRIENDLYNAME, NULL, (PBYTE)buf, cb, NULL)) { SetupDiDestroyDeviceInfoList(di); return std::wstring(buf); }
                    if (SetupDiGetDeviceRegistryPropertyW(di, &devinfo, SPDRP_DEVICEDESC, NULL, (PBYTE)buf, cb, NULL)) { SetupDiDestroyDeviceInfoList(di); return std::wstring(buf); }
                }
            }
        }
        SetupDiDestroyDeviceInfoList(di);
    }

    // Final fallback: use PhysicalMonitorEnumerationAPI to query physical monitor descriptions
    // Map gdiName (e.g. "\\.\DISPLAY1") to HMONITOR via EnumDisplayMonitors / source GDI name
    struct FindMonCtx { std::wstring gdi; std::optional<std::wstring> result; } ctx{ gdiName, std::nullopt };
    auto enumProc = [](HMONITOR hMon, HDC, LPRECT, LPARAM lparam) -> BOOL {
        FindMonCtx* c = reinterpret_cast<FindMonCtx*>(lparam);
        // get source name for this monitor via DisplayConfig
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return TRUE;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return TRUE;
        wchar_t buf[64];
        // iterate paths and try to match target monitor via monitor handle? fallback: compare adapter/source names
        for (UINT32 i=0;i<pathCount;++i) {
            DISPLAYCONFIG_TARGET_DEVICE_NAME tname{};
            tname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tname.header.size = sizeof(tname);
            tname.header.adapterId = paths[i].targetInfo.adapterId;
            tname.header.id = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&tname.header) != ERROR_SUCCESS) continue;
            // tname.monitorFriendlyDeviceName or monitorDevicePath
            std::wstring inst = tname.monitorDevicePath ? std::wstring(tname.monitorDevicePath) : L"";
            if (!inst.empty()) {
                const std::wstring prefix = L"\\\\?\\";
                if (inst.rfind(prefix,0)==0) inst = inst.substr(prefix.size());
                // try if it contains the gdi name
                if (inst.find(c->gdi) != std::wstring::npos) {
                    // query physical monitor description
                    DWORD count = 0;
                    if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &count) && count > 0) {
                        std::vector<PHYSICAL_MONITOR> pms(count);
                        if (GetPhysicalMonitorsFromHMONITOR(hMon, count, pms.data())) {
                            std::wstring desc = pms[0].szPhysicalMonitorDescription ? std::wstring(pms[0].szPhysicalMonitorDescription) : std::wstring();
                            DestroyPhysicalMonitors(count, pms.data());
                            if (!desc.empty()) { c->result = desc; return FALSE; }
                        }
                    }
                }
            }
        }
        return TRUE;
    };
    EnumDisplayMonitors(NULL, NULL, enumProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.result) return ctx.result;

    return std::nullopt;
}

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
