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
#include <unordered_map>
#include <unordered_set>
#include <cctype>
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
#include "StringUtils.h"
#include <set>
#include "../models/DisplayConfig.h"
#include "Logger.h"

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

// Helper wrappers implemented here to avoid requiring ToUtf8 visibility in header.
static std::string ToUtf8Local(const std::wstring& w) {
    if (w.empty()) return std::string();
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (sz <= 0) return std::string();
    std::string out; out.resize(sz);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], sz, NULL, NULL);
    return out;
}

std::optional<std::string> DisplayConfigUtils::GetMonitorDevicePathForGdiName(const std::wstring& gdiName) {
    if (gdiName.empty()) return std::nullopt;
    const UINT32 queries[2] = { QDC_ONLY_ACTIVE_PATHS, QDC_ALL_PATHS };
    for (UINT qi = 0; qi < _countof(queries); ++qi) {
        UINT32 qflag = queries[qi];
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(qflag, &pathCount, &modeCount) != ERROR_SUCCESS) continue;
        if (pathCount == 0) continue;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(qflag, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) continue;
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
                if (tname.monitorDevicePath && tname.monitorDevicePath[0]) return ToUtf8Local(std::wstring(tname.monitorDevicePath));
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> DisplayConfigUtils::GetEdidHexForDeviceInstanceId(const std::wstring& deviceInstanceId) {
    if (deviceInstanceId.empty()) return std::nullopt;

    // Try several candidate instance id strings to account for variations in monitorDevicePath
    // e.g. "\\?\DISPLAY#SAM0E5E#5&29e13dfe&0&UID4353#{...}" -> registry uses "DISPLAY\\SAM0E5E\\5&29e13dfe&0&UID4353_0"
    std::vector<std::wstring> candidates;
    candidates.push_back(deviceInstanceId);
    const std::wstring prefix = L"\\\\?\\";
    if (deviceInstanceId.rfind(prefix, 0) == 0) candidates.push_back(deviceInstanceId.substr(prefix.size()));

    // For each base candidate, add variant with '#' -> '\\'
    size_t startIdx = 0;
    for (size_t i = startIdx; i < candidates.size(); ++i) {
        std::wstring s = candidates[i];
        std::wstring rep = s;
        for (auto &c : rep) if (c == L'#') c = L'\\';
        if (rep != s) candidates.push_back(rep);
    }

    // Also try removing trailing "#{GUID}" portion and creating _0 suffix variant
    for (size_t i = 0; i < candidates.size(); ++i) {
        const std::wstring &s = candidates[i];
        size_t pos = s.find(L"#{");
        if (pos != std::wstring::npos) {
            std::wstring before = s.substr(0, pos);
            // replace '#' -> '\\'
            std::wstring rep = before;
            for (auto &c : rep) if (c == L'#') c = L'\\';
            if (rep != s) candidates.push_back(rep);
            // append _0 to last component
            std::wstring suffix = rep;
            // remove trailing backslash if present
            if (!suffix.empty() && suffix.back() == L'\\') suffix.pop_back();
            suffix += L"_0";
            candidates.push_back(suffix);
        }
    }

    // Try each candidate to read EDID
    for (const auto &cand : candidates) {
        HKEY hKey = NULL;
        std::wstring sub = L"SYSTEM\\CurrentControlSet\\Enum\\" + cand + L"\\Device Parameters";
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) continue;
        DWORD type = 0; DWORD cb = 0;
        if (RegQueryValueExW(hKey, L"EDID", NULL, &type, NULL, &cb) != ERROR_SUCCESS || type != REG_BINARY) { RegCloseKey(hKey); continue; }
        std::vector<BYTE> edid(cb);
        if (RegQueryValueExW(hKey, L"EDID", NULL, &type, edid.data(), &cb) != ERROR_SUCCESS) { RegCloseKey(hKey); continue; }
        RegCloseKey(hKey);
        if (edid.empty()) continue;
        std::ostringstream oss;
        oss << std::hex;
        for (BYTE b : edid) {
            oss.width(2); oss.fill('0'); oss << (int)b;
        }
        std::string res = oss.str();
        // normalize to lowercase
        for (auto &c : res) c = (char)tolower((unsigned char)c);
        return res;
    }
    return std::nullopt;
}

std::optional<std::string> DisplayConfigUtils::GetWmiKeyForDeviceInstanceId(const std::wstring& deviceInstanceId) {
    if (deviceInstanceId.empty()) return std::nullopt;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool coInit = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) coInit = false;
    if (!(SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)) return std::nullopt;

    std::optional<std::string> out;
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, NULL, EOAC_NONE, NULL);
    IWbemLocator *pLoc = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID *)&pLoc))) {
        IWbemServices *pSvc = NULL;
        if (SUCCEEDED(pLoc->ConnectServer(_bstr_t(L"ROOT\\WMI"), NULL, NULL, 0, NULL, 0, 0, &pSvc))) {
            CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

                // build input candidates to increase chance of matching InstanceName formats
                std::vector<std::wstring> candidates;
                candidates.push_back(deviceInstanceId);
                const std::wstring prefix = L"\\\\?\\";
                if (deviceInstanceId.rfind(prefix, 0) == 0) candidates.push_back(deviceInstanceId.substr(prefix.size()));
                // variants: replace '#' -> '\\'
                size_t ci = 0;
                for (; ci < candidates.size(); ++ci) {
                    std::wstring s = candidates[ci];
                    std::wstring rep = s;
                    for (auto &c : rep) if (c == L'#') c = L'\\';
                    if (rep != s) candidates.push_back(rep);
                }
                // remove trailing GUID {..} and add _0 index variant
                for (size_t i = 0; i < candidates.size(); ++i) {
                    const std::wstring &s = candidates[i];
                    size_t pos = s.find(L"#{");
                    if (pos == std::wstring::npos) pos = s.find(L"\\{");
                    if (pos != std::wstring::npos) {
                        std::wstring before = s.substr(0, pos);
                        if (before != s) candidates.push_back(before);
                        std::wstring idx = before;
                        if (!idx.empty() && idx.back() == L'\\') idx.pop_back();
                        idx += L"_0";
                        candidates.push_back(idx);
                    }
                }

                IEnumWbemClassObject* pEnumerator = NULL;
            HRESULT qhr = pSvc->ExecQuery(_bstr_t(L"WQL"), _bstr_t(L"SELECT InstanceName, ManufacturerName, ProductCodeID, SerialNumberID FROM WmiMonitorID"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator);
            if (SUCCEEDED(qhr) && pEnumerator) {
                IWbemClassObject *pObj = NULL;
                while (pEnumerator->Next(WBEM_INFINITE, 1, &pObj, NULL) == S_OK) {
                    VARIANT varInst; VariantInit(&varInst);
                    if (SUCCEEDED(pObj->Get(L"InstanceName", 0, &varInst, NULL, NULL)) && varInst.vt == VT_BSTR) {
                            std::wstring inst = varInst.bstrVal ? std::wstring(varInst.bstrVal) : L"";
                            // normalize and compare against all candidates
                            auto normalize = [](const std::wstring &s)->std::wstring {
                                std::wstring out = s;
                                for (auto &c : out) c = towlower(c);
                                return out;
                            };
                            std::wstring instNorm = normalize(inst);
                            bool match = false;
                            for (const auto &cand : candidates) {
                                std::wstring candNorm = normalize(cand);
                                if (instNorm.find(candNorm) != std::wstring::npos || candNorm.find(instNorm) != std::wstring::npos) { match = true; break; }
                            }
                            if (match) {
                            // Manufacturer
                            std::string manStr;
                            VARIANT varMan; VariantInit(&varMan);
                            if (SUCCEEDED(pObj->Get(L"ManufacturerName", 0, &varMan, NULL, NULL)) && (varMan.vt & VT_ARRAY)) {
                                SAFEARRAY* psa = varMan.parray; USHORT *data = NULL; LONG l=0,u=0; SafeArrayGetLBound(psa,1,&l); SafeArrayGetUBound(psa,1,&u); SafeArrayAccessData(psa,(void**)&data);
                                std::wstring man; for (LONG ii=0; ii<=u-l; ++ii) { USHORT ch = data[ii]; if (ch==0) break; man.push_back((wchar_t)ch); }
                                SafeArrayUnaccessData(psa); manStr = ToUtf8Local(man);
                            }
                            // Product code: prefer ASCII characters when available (many monitors expose ASCII product string)
                            std::string prodStr;
                            VARIANT varProd; VariantInit(&varProd);
                            if (SUCCEEDED(pObj->Get(L"ProductCodeID", 0, &varProd, NULL, NULL)) && (varProd.vt & VT_ARRAY)) {
                                SAFEARRAY* psa = varProd.parray; BYTE *pdata = NULL; LONG l=0,u=0; SafeArrayGetLBound(psa,1,&l); SafeArrayGetUBound(psa,1,&u); SafeArrayAccessData(psa,(void**)&pdata);
                                std::wstring prodW;
                                for (LONG ii=0; ii<=u-l; ++ii) { BYTE b = pdata[ii]; if (b == 0) break; prodW.push_back((wchar_t)b); }
                                SafeArrayUnaccessData(psa);
                                prodStr = ToUtf8Local(prodW);
                                // if empty, fall back to numeric list
                                if (prodStr.empty()) {
                                    std::ostringstream oss; oss << std::dec;
                                    for (LONG ii=0; ii<=u-l; ++ii) { if (ii) oss << ","; oss << (int)pdata[ii]; }
                                    prodStr = oss.str();
                                }
                            }
                            // Serial
                            std::string serStr;
                            VARIANT varSer; VariantInit(&varSer);
                            if (SUCCEEDED(pObj->Get(L"SerialNumberID", 0, &varSer, NULL, NULL)) && (varSer.vt & VT_ARRAY)) {
                                SAFEARRAY* psa = varSer.parray; USHORT *data2 = NULL; LONG l2=0,u2=0; SafeArrayGetLBound(psa,1,&l2); SafeArrayGetUBound(psa,1,&u2); SafeArrayAccessData(psa,(void**)&data2);
                                std::wstring ser; for (LONG ii=0; ii<=u2-l2; ++ii) { USHORT ch = data2[ii]; if (ch==0) break; ser.push_back((wchar_t)ch); }
                                SafeArrayUnaccessData(psa); serStr = ToUtf8Local(ser);
                            }
                            std::string key = manStr + "-" + prodStr + "-" + serStr;
                            for (auto &c : key) c = (char)tolower((unsigned char)c);
                            out = key;
                            VariantClear(&varMan); VariantClear(&varProd); VariantClear(&varSer);
                            VariantClear(&varInst);
                            pObj->Release();
                            break;
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
    return out;
}

// Implementation of ApplyTopologyFromStore
bool DisplayConfigUtils::ApplyTopologyFromStore(const std::map<std::string, bool>& topologyMap) {
    if (topologyMap.empty()) return true;

    auto normalize = [](std::string s) {
        // drop \\?\ prefix and lowercase
        if (s.rfind("\\\\?\\", 0) == 0) s = s.substr(4);
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        return s;
        };

    // build normalized lookup
    std::unordered_map<std::string, bool> topoLower;
    topoLower.reserve(topologyMap.size());
    for (const auto& kv : topologyMap) {
        topoLower[normalize(kv.first)] = kv.second;
    }

    // Query only active paths
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
        return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
        return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> newPaths;
    newPaths.reserve(pathCount);

    // For verification/logging only
    std::vector<std::string> disabledGdiNames;

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;

        std::wstring gdiW;
        std::string gdiUtf;
        if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS) {
            gdiW = src.viewGdiDeviceName;
            try {
                gdiUtf = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(gdiW);
            }
            catch (...) {
                gdiUtf.clear();
            }
        }

        // Target identifiers
        std::string targetMdpUtf;
        std::string targetEdidHex;
        std::string friendlyUtf;

        try {
            DISPLAYCONFIG_TARGET_DEVICE_NAME tname{};
            tname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tname.header.size = sizeof(tname);
            tname.header.adapterId = paths[i].targetInfo.adapterId;
            tname.header.id = paths[i].targetInfo.id;

            if (DisplayConfigGetDeviceInfo(&tname.header) == ERROR_SUCCESS) {
                if (tname.monitorDevicePath && tname.monitorDevicePath[0]) {
                    try {
                        targetMdpUtf = std::wstring_convert<std::codecvt_utf8<wchar_t>>()
                            .to_bytes(std::wstring(tname.monitorDevicePath));
                    }
                    catch (...) {
                        targetMdpUtf.clear();
                    }
                }
                if (tname.monitorFriendlyDeviceName && tname.monitorFriendlyDeviceName[0]) {
                    try {
                        friendlyUtf = std::wstring_convert<std::codecvt_utf8<wchar_t>>()
                            .to_bytes(std::wstring(tname.monitorFriendlyDeviceName));
                    }
                    catch (...) {
                        friendlyUtf.clear();
                    }
                }

                // attempt EDID read using device instance id
                if (!targetMdpUtf.empty()) {
                    try {
                        std::wstring inst = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(targetMdpUtf);
                        const std::wstring prefix = L"\\\\?\\";
                        if (inst.rfind(prefix, 0) == 0) {
                            inst = inst.substr(prefix.size());
                        }
                        auto ed = GetEdidHexForDeviceInstanceId(inst);
                        if (ed) {
                            targetEdidHex = *ed;
                            for (auto& c : targetEdidHex) c = (char)tolower((unsigned char)c);
                        }
                    }
                    catch (...) {
                        // ignore EDID failures
                    }
                }
            }
        }
        catch (...) {
            // ignore target info failures
        }

        // Decide enable/disable based on topologyMap
        bool foundRule = false;
        bool desiredEnabled = true;

        auto considerKey = [&](const std::string& key) {
            if (key.empty()) return;
            auto it = topoLower.find(normalize(key));
            if (it != topoLower.end()) {
                foundRule = true;
                desiredEnabled = it->second;
            }
            };

        // Priority: EDID, then monitor device path, then friendly name
        if (!targetEdidHex.empty()) considerKey(targetEdidHex);
        if (!foundRule && !targetMdpUtf.empty()) considerKey(targetMdpUtf);
        if (!foundRule && !friendlyUtf.empty()) considerKey(friendlyUtf);

        bool shouldDisable = (foundRule && !desiredEnabled);

        if (shouldDisable) {
            LOG_INFO("Disabling display %s (%s)...", friendlyUtf.c_str(), gdiUtf.c_str());
        }

        if (shouldDisable) {
            if (!gdiUtf.empty()) {
                std::string gdiNorm = gdiUtf;
                for (auto& c : gdiNorm) c = (char)tolower((unsigned char)c);
                disabledGdiNames.push_back(gdiNorm);
            }
            // Do NOT push this path -> effectively disables it
            continue;
        }

        // Keep this path as-is
        newPaths.push_back(paths[i]);
    }

    // Nothing changed
    if (newPaths.size() == pathCount) {
        return true;
    }

    // Let Windows keep current modes; we only supply which paths are active
    LOG_INFO("DisplayConfigUtils: SetDisplayConfig(topology only) paths=%u", static_cast<unsigned int>(newPaths.size()));

    // Build a compact modes array containing only the mode entries referenced by newPaths
    std::vector<DISPLAYCONFIG_MODE_INFO> suppliedModes;
    suppliedModes.reserve(modeCount);
    std::unordered_map<UINT32, UINT32> modeIndexMap; // old index -> new index

    auto ensureMode = [&](UINT32 oldIdx) -> UINT32 {
        if (oldIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) return DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        auto it = modeIndexMap.find(oldIdx);
        if (it != modeIndexMap.end()) return it->second;
        UINT32 newIdx = static_cast<UINT32>(suppliedModes.size());
        suppliedModes.push_back(modes[oldIdx]);
        modeIndexMap[oldIdx] = newIdx;
        return newIdx;
    };

    // Remap modeInfoIdx in a local copy of the newPaths array
    std::vector<DISPLAYCONFIG_PATH_INFO> pathsForApply = newPaths;
    for (auto &p : pathsForApply) {
        UINT32 sidx = p.sourceInfo.modeInfoIdx;
        UINT32 tidx = p.targetInfo.modeInfoIdx;
        UINT32 newS = ensureMode(sidx);
        UINT32 newT = ensureMode(tidx);
        p.sourceInfo.modeInfoIdx = newS;
        p.targetInfo.modeInfoIdx = newT;
    }

    LONG status = SetDisplayConfig(
        static_cast<UINT32>(pathsForApply.size()), pathsForApply.data(),
        static_cast<UINT32>(suppliedModes.size()), suppliedModes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES
    );

    {
        char msgbuf[512] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, (DWORD)status, 0, msgbuf, (DWORD)sizeof(msgbuf), NULL);
        std::string msg = msgbuf[0] ? std::string(msgbuf) : std::string();
        while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n')) msg.pop_back();
        if (status == 0) {
            LOG_INFO("SetDisplayConfig returned %ld. %s", status, msg.empty() ? "" : msg.c_str());
        } else {
            LOG_ERROR("SetDisplayConfig returned %ld. %s", status, msg.empty() ? "" : msg.c_str());
        }
    }

    if (status != ERROR_SUCCESS && status != ERROR_INVALID_PARAMETER) {
        return false;
    }

    // Optional verification: check that disabled GDIs are gone from active paths
    UINT32 qPathCount = 0, qModeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &qPathCount, &qModeCount) != ERROR_SUCCESS) {
        return true; // we already applied; don't treat verification failure as fatal
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> qPaths(qPathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> qModes(qModeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &qPathCount, qPaths.data(), &qModeCount, qModes.data(), nullptr) != ERROR_SUCCESS) {
        return true;
    }

    for (UINT32 qi = 0; qi < qPathCount; ++qi) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sname{};
        sname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sname.header.size = sizeof(sname);
        sname.header.adapterId = qPaths[qi].sourceInfo.adapterId;
        sname.header.id = qPaths[qi].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sname.header) != ERROR_SUCCESS) continue;

        std::wstring gdiW2 = sname.viewGdiDeviceName;
        std::string gdiUtf2;
        try {
            gdiUtf2 = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(gdiW2);
        }
        catch (...) {
            gdiUtf2.clear();
        }
        std::string gdiNorm2 = gdiUtf2;
        for (auto& c : gdiNorm2) c = (char)tolower((unsigned char)c);

        for (const auto& d : disabledGdiNames) {
            if (d == gdiNorm2) {
                LOG_WARN("Disabled display still present: %s", gdiUtf2.c_str());
                return false;
            }
        }
    }

    return true;
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

std::vector<std::pair<GUID, std::wstring>> DisplayConfigUtils::ListDisplays(std::vector<std::pair<GUID, std::wstring>> virtualDisplays) const {
    std::vector<std::pair<GUID, std::wstring>> out;
    out.reserve(virtualDisplays.size());
    // First enumerate physical displays and add them (marked with empty GUID)
    std::set<std::wstring> virtualGdiNames;
    for (const auto& kv : virtualDisplays) {
        virtualGdiNames.insert(kv.second);
    }

    // Enumerate adapters and their monitors to get monitor-friendly names (DeviceString on monitor devices)
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof(adapter);
    for (DWORD ai = 0; EnumDisplayDevicesW(nullptr, ai, &adapter, 0); ++ai) {
        // consider active / attached adapters
        if (!(adapter.StateFlags & DISPLAY_DEVICE_ACTIVE) && !(adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
            adapter.cb = sizeof(adapter);
            continue;
        }
        std::wstring adapterName = adapter.DeviceName ? adapter.DeviceName : L"";

        // enumerate monitors for this adapter to obtain monitor friendly names
        DISPLAY_DEVICEW mon{};
        mon.cb = sizeof(mon);
        bool anyMonitor = false;
        for (DWORD mi = 0; EnumDisplayDevicesW(adapter.DeviceName, mi, &mon, 0); ++mi) {
            // only consider attached monitors
            if (!(mon.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && !(mon.StateFlags & DISPLAY_DEVICE_ACTIVE)) { mon.cb = sizeof(mon); continue; }

            // skip if a virtual session already manages this GDI name
            if (virtualGdiNames.find(adapterName) != virtualGdiNames.end()) { mon.cb = sizeof(mon); anyMonitor = true; continue; }

            std::wstring friendly;
            // Prefer lookup by GDI name (uses DisplayConfig -> monitorDevicePath -> WMI/EDID)
            try {
                auto byGdi = vdc::DisplayConfigUtils::GetMonitorFriendlyNameForGdiName(adapterName);
                if (byGdi && !byGdi->empty()) friendly = *byGdi;
            }
            catch (...) {}
            // Fallback: try device instance id from EnumDisplayDevices
            if (friendly.empty() && mon.DeviceID) {
                try {
                    auto regName = vdc::DisplayConfigUtils::GetMonitorFriendlyNameFromDeviceInstanceId(mon.DeviceID);
                    if (regName && !regName->empty()) friendly = *regName;
                }
                catch (...) {}
            }
            if (friendly.empty()) friendly = mon.DeviceString ? mon.DeviceString : adapterName;
            std::wstring label = friendly + L" (" + adapterName + L")";
            out.emplace_back(GUID(), label);
            anyMonitor = true;
            mon.cb = sizeof(mon);
        }

        // If adapter had no monitor entries, fall back to adapter DeviceString
        if (!anyMonitor) {
            if (virtualGdiNames.find(adapterName) == virtualGdiNames.end()) {
                std::wstring friendly = adapter.DeviceString ? adapter.DeviceString : adapterName;
                std::wstring label = friendly + L" (" + adapterName + L")";
                out.emplace_back(GUID(), label);
            }
        }

        adapter.cb = sizeof(adapter);
    }
    for (const auto& kv : virtualDisplays) {
        const std::wstring devName = kv.second;
        std::wstring label;
        if (!kv.second.empty()) label = kv.second + L" (" + devName + L")";
        else label = devName;
        out.emplace_back(kv.first, label);
    }
    return out;
}

std::vector<vdc::Topology> DisplayConfigUtils::GetActiveDisplayTopology(const std::vector<std::pair<GUID, std::wstring>>& virtualDisplays) {
    std::vector<Topology> topologies;

    try {
        // collect active identifiers from only-active paths
        std::unordered_set<std::string> activeIds;
        UINT32 aPathCount = 0, aModeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &aPathCount, &aModeCount) == ERROR_SUCCESS && aPathCount > 0) {
            std::vector<DISPLAYCONFIG_PATH_INFO> aPaths(aPathCount);
            std::vector<DISPLAYCONFIG_MODE_INFO> aModes(aModeCount);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &aPathCount, aPaths.data(), &aModeCount, aModes.data(), nullptr) == ERROR_SUCCESS) {
                for (UINT32 i = 0; i < aPathCount; ++i) {
                    DISPLAYCONFIG_TARGET_DEVICE_NAME tname{};
                    tname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                    tname.header.size = sizeof(tname);
                    tname.header.adapterId = aPaths[i].targetInfo.adapterId;
                    tname.header.id = aPaths[i].targetInfo.id;
                    if (DisplayConfigGetDeviceInfo(&tname.header) != ERROR_SUCCESS) continue;
                    if (tname.monitorDevicePath && tname.monitorDevicePath[0]) {
                        std::string mdp;
                        try { mdp = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(std::wstring(tname.monitorDevicePath)); }
                        catch (...) { mdp = ""; }
                        if (!mdp.empty()) {
                            std::string n = vdc::ToLower(mdp);
                            activeIds.insert(n);
                            const std::string pref = "\\\\?\\";
                            if (n.rfind(pref, 0) == 0) activeIds.insert(n.substr(pref.size()));
                        }
                        try {
                            std::wstring inst = std::wstring(tname.monitorDevicePath);
                            const std::wstring ipref = L"\\\\?\\";
                            if (inst.rfind(ipref, 0) == 0) inst = inst.substr(ipref.size());
                            auto ed = GetEdidHexForDeviceInstanceId(inst);
                            if (ed) {
                                std::string e = *ed; for (auto& c : e) c = (char)tolower((unsigned char)c);
                                activeIds.insert(e);
                            }
                        }
                        catch (...) {}
                    }
                }
            }
        }

        // enumerate all paths and create topology entries
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS && pathCount > 0) {
            std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
            if (QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) == ERROR_SUCCESS) {
                std::unordered_set<std::string> seenIds;
                for (UINT32 i = 0; i < pathCount; ++i) {
                    DISPLAYCONFIG_TARGET_DEVICE_NAME tname{};
                    tname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                    tname.header.size = sizeof(tname);
                    tname.header.adapterId = paths[i].targetInfo.adapterId;
                    tname.header.id = paths[i].targetInfo.id;
                    std::string idStr;
                    std::string friendlyUtf;
                    if (DisplayConfigGetDeviceInfo(&tname.header) == ERROR_SUCCESS) {
                        if (tname.monitorDevicePath && tname.monitorDevicePath[0]) {
                            try { friendlyUtf = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(std::wstring(tname.monitorFriendlyDeviceName ? tname.monitorFriendlyDeviceName : L"")); }
                            catch (...) { friendlyUtf = ""; }
                            std::string mdp;
                            try { mdp = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(std::wstring(tname.monitorDevicePath)); }
                            catch (...) { mdp = ""; }
                            if (!mdp.empty()) {
                                std::string n = vdc::ToLower(mdp);
                                try {
                                    std::wstring inst = std::wstring(tname.monitorDevicePath);
                                    const std::wstring ipref = L"\\\\?\\";
                                    if (inst.rfind(ipref, 0) == 0) inst = inst.substr(ipref.size());
                                    auto ed = GetEdidHexForDeviceInstanceId(inst);
                                    if (ed) {
                                        idStr = *ed;
                                        for (auto& c : idStr) c = (char)tolower((unsigned char)c);
                                    }
                                }
                                catch (...) {}
                                if (idStr.empty()) idStr = n;
                            }
                        }
                    }

                    if (idStr.empty()) continue;
                    if (seenIds.find(idStr) != seenIds.end()) continue;
                    seenIds.insert(idStr);

                    Topology t;
                    t.edid = idStr;
                    t.displayName = friendlyUtf;
                    t.enabled = (activeIds.find(idStr) != activeIds.end());

                    // Try to map to virtual display displayId by comparing the source GDI name
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
                    src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    src.header.size = sizeof(src);
                    src.header.adapterId = paths[i].sourceInfo.adapterId;
                    src.header.id = paths[i].sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS) {
                        std::wstring srcGdi = src.viewGdiDeviceName;
                        if (!srcGdi.empty()) {
                            for (const auto& vd : virtualDisplays) {
                                if (vd.second == srcGdi) {
                                    // found matching virtual display, store its GUID string
                                    t.displayId = vdc::GuidToString(vd.first);
                                    break;
                                }
                            }
                        }
                    }

                    topologies.push_back(t);
                }
            }
        }

        return topologies;
    }
    catch (std::exception& ex) {
        std::cerr << "[DisplayConfigUtils] Exception in GetActiveDisplayTopology: " << ex.what() << "\n";
        return std::vector<Topology>();
    }
    catch (...) {
        std::cerr << "[DisplayConfigUtils] Exception in GetActiveDisplayTopology: Unknown Exception." << "\n";
        return std::vector<Topology>();
    }
}
