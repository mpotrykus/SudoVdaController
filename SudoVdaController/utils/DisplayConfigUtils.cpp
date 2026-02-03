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
#include "../models/VirtualDisplay.h"
#include <algorithm>





struct DeviceName {
    std::wstring name;
    std::wstring path;
};

struct DisplayConfigResult {
    bool success = true;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    UINT32 pathCount = 0;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    UINT32 modeCount = 0;
};




std::string GetReturnCode(long code)
{
    switch (code)
    {
    case ERROR_SUCCESS: return "ERROR_SUCCESS";
    case ERROR_INVALID_PARAMETER: return "ERROR_INVALID_PARAMETER";
    case ERROR_NOT_SUPPORTED: return "ERROR_NOT_SUPPORTED";
    case ERROR_ACCESS_DENIED: return "ERROR_ACCESS_DENIED";
    case ERROR_GEN_FAILURE: return "ERROR_GEN_FAILURE";
    case ERROR_BAD_CONFIGURATION: return "ERROR_BAD_CONFIGURATION";
    case ERROR_INSUFFICIENT_BUFFER: return "ERROR_INSUFFICIENT_BUFFER";
    }
    return "UNKNOWN";
}


std::optional<std::string> GetEdidHexForDeviceInstanceId(const std::wstring& deviceInstanceId) {
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
        for (auto& c : rep) if (c == L'#') c = L'\\';
        if (rep != s) candidates.push_back(rep);
    }


    // Also try removing trailing "#{GUID}" portion and creating _0 suffix variant
    for (size_t i = 0; i < candidates.size(); ++i) {
        const std::wstring& s = candidates[i];
        size_t pos = s.find(L"#{");
        if (pos != std::wstring::npos) {
            std::wstring before = s.substr(0, pos);
            // replace '#' -> '\\'
            std::wstring rep = before;
            for (auto& c : rep) if (c == L'#') c = L'\\';
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
    for (const auto& cand : candidates) {
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
        for (auto& c : res) c = (char)tolower((unsigned char)c);
        return res;
    }
    return std::nullopt;
}

DeviceName GetTargetDeviceName(DISPLAYCONFIG_PATH_INFO const& path)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {};
    targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    targetName.header.size = sizeof(DISPLAYCONFIG_TARGET_DEVICE_NAME);
    targetName.header.adapterId = path.targetInfo.adapterId;
    targetName.header.id = path.targetInfo.id;
    auto ret = DisplayConfigGetDeviceInfo(&targetName.header);
    DeviceName name{};
    if (ret == ERROR_SUCCESS)
    {
        name.name = targetName.monitorFriendlyDeviceName;
        name.path = targetName.monitorDevicePath;

        if (name.name.empty())
        {
            if (path.targetInfo.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL)
            {
                name.name = L"<internal>";
            }
        }
    }
    return name;
}

std::wstring GetGdiDeviceName(DISPLAYCONFIG_PATH_INFO const& path)
{
    DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
    sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    sourceName.header.size = sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME);
    sourceName.header.adapterId = path.sourceInfo.adapterId;
    sourceName.header.id = path.sourceInfo.id;
    auto ret = DisplayConfigGetDeviceInfo(&sourceName.header);
    return (ret == ERROR_SUCCESS)
        ? std::wstring{ sourceName.viewGdiDeviceName }
    : std::wstring{};
}

bool IsEnabled(DISPLAYCONFIG_PATH_INFO const& path)
{
    return (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
}

void SetEnabled(DISPLAYCONFIG_PATH_INFO& path, bool enable)
{
    if (enable) {
        path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
        return;
	}

    path.flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
}

void FilterPaths(std::vector<DISPLAYCONFIG_PATH_INFO>& paths)
{
    // Remove all paths without available target
    paths.erase(std::remove_if(paths.begin(), paths.end(), [](DISPLAYCONFIG_PATH_INFO const& p) { return !p.targetInfo.targetAvailable; }), paths.end());

    // All enabled paths first
    std::sort(paths.begin(), paths.end(),
        [](DISPLAYCONFIG_PATH_INFO const& a, DISPLAYCONFIG_PATH_INFO const& b)
        {
            bool ae = IsEnabled(a);
            bool be = IsEnabled(b);
            if (ae == be)
            {
                return a.sourceInfo.id < b.sourceInfo.id;
            }
            return ae > be;
        }
    );

    // Remove all alternative disabled paths where there are enabled paths
    std::unordered_set<std::wstring> enabledDisplays;
    std::unordered_set<std::wstring> enabledTargetDevices;
    for (DISPLAYCONFIG_PATH_INFO const& p : paths)
    {
        if (!IsEnabled(p)) continue;
        std::wstring srcName = GetGdiDeviceName(p);
        enabledDisplays.insert(srcName);
        DeviceName tarName = GetTargetDeviceName(p);
        enabledTargetDevices.insert(tarName.path);
    }
    paths.erase(std::remove_if(paths.begin(), paths.end(),
        [&enabledDisplays, &enabledTargetDevices](DISPLAYCONFIG_PATH_INFO const& p)
        {
            if (IsEnabled(p)) return false;

            std::wstring srcName = GetGdiDeviceName(p);
            if (enabledDisplays.find(srcName) != enabledDisplays.end()) return true;

            // these could be interesting for cloning. I don't care for now
            DeviceName tarName = GetTargetDeviceName(p);
            if (enabledTargetDevices.find(tarName.path) != enabledTargetDevices.end()) return true;

            return false;
        }
    ), paths.end());

    // If multiple disabled paths only differ in source 'id' (EVERYTHING else identical), then only keep the one with the smallest id
    std::unordered_set<DISPLAYCONFIG_PATH_INFO const*> toDelete;
    auto end = paths.end();
    for (auto it = paths.begin(); it != end; ++it)
    {
        DISPLAYCONFIG_PATH_INFO a = *it; // copy!
        a.sourceInfo.id = 0;
        if (IsEnabled(*it)) continue;
        for (auto it2 = paths.begin(); it2 != it; ++it2)
        {
            DISPLAYCONFIG_PATH_INFO b = *it2; // copy!
            b.sourceInfo.id = 0;
            if (memcmp(&a, &b, sizeof(DISPLAYCONFIG_PATH_INFO)) == 0)
            {
                toDelete.insert(&(*it));
                break;
            }
        }
    }
    if (toDelete.size() > 0) {
        paths.erase(std::remove_if(paths.begin(), paths.end(),
            [&toDelete](DISPLAYCONFIG_PATH_INFO const& p)
            {
                if (IsEnabled(p)) return false;
                return toDelete.find(&p) != toDelete.end();
            }
        ), paths.end());
    }
}

DISPLAYCONFIG_PATH_INFO* FindPath(std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::wstring const& id)
{
    // helper: normalize utf8 string (lowercase, remove whitespace)
    auto normalizeUtf = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            if (!std::isspace(c)) out.push_back(static_cast<char>(std::tolower(c)));
        }
        return out;
    };

    // convert needle (wstring) -> normalized utf8
    std::string needleUtf;
    try {
        needleUtf = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(id);
    }
    catch (...) { needleUtf.clear(); }
    needleUtf = normalizeUtf(needleUtf);

    for (DISPLAYCONFIG_PATH_INFO& path : paths)
    {
        // 1) Try to match EDID (preferred)
        DISPLAYCONFIG_TARGET_DEVICE_NAME tname{};
        tname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tname.header.size = sizeof(tname);
        tname.header.adapterId = path.targetInfo.adapterId;
        tname.header.id = path.targetInfo.id;

        if (DisplayConfigGetDeviceInfo(&tname.header) == ERROR_SUCCESS && tname.monitorDevicePath && tname.monitorDevicePath[0])
        {
            try {
                std::wstring inst = std::wstring(tname.monitorDevicePath);
                const std::wstring prefix = L"\\\\?\\";
                if (inst.rfind(prefix, 0) == 0) inst = inst.substr(prefix.size());

                auto edOpt = GetEdidHexForDeviceInstanceId(inst);
                if (edOpt) {
                    std::string ed = *edOpt;
                    // normalize returned edid hex to lowercase/no-spaces
                    for (auto &c : ed) c = static_cast<char>(std::tolower((unsigned char)c));
                    ed.erase(std::remove_if(ed.begin(), ed.end(), ::isspace), ed.end());
                    if (!ed.empty() && ed == needleUtf) return &path;
                }
            }
            catch (...) {}
        }

        // 2) Fallback: match source GDI name
        std::wstring srcName = GetGdiDeviceName(path);
        std::string srcUtf;
        try { srcUtf = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(srcName); } catch (...) { srcUtf.clear(); }
        if (normalizeUtf(srcUtf) == needleUtf) return &path;

        // 3) Fallback: match target friendly name or monitorDevicePath
        DeviceName tar = GetTargetDeviceName(path);
        std::string tarNameUtf, tarPathUtf;
        try { tarNameUtf = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(tar.name); } catch (...) { tarNameUtf.clear(); }
        try { tarPathUtf = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(tar.path); } catch (...) { tarPathUtf.clear(); }

        if (normalizeUtf(tarNameUtf) == needleUtf) return &path;
        if (normalizeUtf(tarPathUtf) == needleUtf) return &path;
    }

    return nullptr;
}

DisplayConfigResult GetDisplayConfig() {
	DisplayConfigResult result;

    UINT32 pathCount = 0, modeCount = 0;
    HRESULT hr = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount);
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    hr = QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);

    if (hr != ERROR_SUCCESS) {
		result.success = false;
        return result;
    }

    FilterPaths(paths);
    
	result.modes = std::move(modes);
    result.modeCount = modeCount;
	result.paths = std::move(paths);
	result.pathCount = pathCount;

    return result;
}

// Diagnostic: print basic path fields before calling SetDisplayConfig
void DumpPathsForDebug(const std::vector<DISPLAYCONFIG_PATH_INFO>& paths)
{
    for (size_t i = 0; i < paths.size(); ++i) {
        const auto& p = paths[i];
        char buf[512];
        snprintf(buf, sizeof(buf),
            "path[%zu]: srcA=%08x%08x srcId=%u tgtA=%08x%08x tgtId=%u flags=0x%x srcModeIdx=%d tgtModeIdx=%d avail=%d",
            i,
            static_cast<unsigned int>(p.sourceInfo.adapterId.HighPart),
            static_cast<unsigned int>(p.sourceInfo.adapterId.LowPart),
            static_cast<unsigned int>(p.sourceInfo.id),
            static_cast<unsigned int>(p.targetInfo.adapterId.HighPart),
            static_cast<unsigned int>(p.targetInfo.adapterId.LowPart),
            static_cast<unsigned int>(p.targetInfo.id),
            p.flags,
            static_cast<int>(p.sourceInfo.modeInfoIdx),
            static_cast<int>(p.targetInfo.modeInfoIdx),
            p.targetInfo.targetAvailable);
        LOG_INFO(buf);
    }
}

long Apply(std::vector<DISPLAYCONFIG_PATH_INFO>& paths)
{
    // clear modeInfoIdx fields, as the API will be requested to newly align the mode info data
    for (DISPLAYCONFIG_PATH_INFO& path : paths)
    {
        path.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    }

	DumpPathsForDebug(paths);

    // validate
    long result = SetDisplayConfig(static_cast<uint32_t>(paths.size()), paths.data(), 0, nullptr, SDC_VALIDATE | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES);
    if (result != ERROR_SUCCESS)
    {
        LOG_ERROR("Topology validation check failed");
        return result;
    }

    return SetDisplayConfig(static_cast<uint32_t>(paths.size()), paths.data(), 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES);
}

bool DisplayConfigUtils::ApplyDisplayConfig(std::vector<Topology> requested) {

    auto dcResult = GetDisplayConfig();
    if (!dcResult.success) {
        LOG_ERROR("Failed to get display config");
        return false;
    }

    for (UINT32 i = 0; i < requested.size(); i++)
    {
		auto request = requested[i];

        auto path = FindPath(dcResult.paths, StringToWString(request.edid));
        if (path == nullptr)
        {
            auto msg = "No path was found for " + request.displayName;
            LOG_WARN(msg.c_str());
            continue;
        }

		bool isEnabled = IsEnabled(*path);
        if (request.enabled == isEnabled)
        {
            auto msg = "'" + request.displayName + "' is already " + (request.enabled ? "enabled" : "disabled") + ". Skipping...";
            LOG_WARN(msg.c_str());
            continue;
        }

        std::string msg = std::string(request.enabled ? "Enabling" : "Disabling") + " '" + request.displayName + "'...";
        LOG_WARN(msg.c_str());
        
		SetEnabled(*path, request.enabled);
    }

    auto res = Apply(dcResult.paths);
    if (res != ERROR_SUCCESS)
    {
		auto msg = "Failed to apply display config: " + GetReturnCode(res) + " (" + std::to_string(res) + ")";
        LOG_ERROR(msg.c_str());
		return false;
    }

	return true;
}

































    // Get display config (paths + modes) for a given QDC flag
    static bool GetDisplayConfigSnapshot(UINT32 qdcFlags,
        std::vector<DISPLAYCONFIG_PATH_INFO>& outPaths,
        std::vector<DISPLAYCONFIG_MODE_INFO>& outModes)
    {
        UINT32 pathCount = 0, modeCount = 0;
        LONG err = GetDisplayConfigBufferSizes(qdcFlags, &pathCount, &modeCount);
        if (err != ERROR_SUCCESS) return false;
        outPaths.resize(pathCount);
        outModes.resize(modeCount);
        err = QueryDisplayConfig(qdcFlags, &pathCount, outPaths.data(), &modeCount, outModes.data(), nullptr);
        return err == ERROR_SUCCESS;
    }

    // Find path index whose target EDID (read from registry) matches normalizedEdid (lowercase, no spaces)
    static bool FindPathIndexByEdid(const std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
        const std::vector<DISPLAYCONFIG_MODE_INFO>& /*modes*/,
        const std::string& normalizedEdidLower,
        UINT32& outIndex)
    {
        for (UINT32 i = 0; i < paths.size(); ++i) {
            DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
            targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            targetName.header.size = sizeof(targetName);
            targetName.header.adapterId = paths[i].targetInfo.adapterId;
            targetName.header.id = paths[i].targetInfo.id;

            if (DisplayConfigGetDeviceInfo(&targetName.header) != ERROR_SUCCESS)
                continue;

            if (targetName.monitorDevicePath == nullptr || targetName.monitorDevicePath[0] == L'\0')
                continue;

            try {
                // strip \\?\ prefix before passing to GetEdidHexForDeviceInstanceId
                std::wstring inst = std::wstring(targetName.monitorDevicePath);
                const std::wstring prefix = L"\\\\?\\";
                if (inst.rfind(prefix, 0) == 0) inst = inst.substr(prefix.size());
                auto edOpt = GetEdidHexForDeviceInstanceId(inst);
                if (!edOpt) continue;
                std::string ed = *edOpt;
                for (auto& c : ed) c = (char)tolower((unsigned char)c);
                if (ed == normalizedEdidLower) {
                    outIndex = i;
                    return true;
                }
            }
            catch (...) {
                continue;
            }
        }
        return false;
    }



static bool SameAdapter(const LUID& a, const LUID& b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}













// --- New, refactored EnableDisplayByEdidHex (uses helpers) ---
bool DisplayConfigUtils::SetDisplayEnabled(const std::string& edid, bool enabled)
{
    if (edid.empty()) return false;

    // normalize input to lowercase, remove whitespace
    std::string targetEdid = edid;
    targetEdid.erase(std::remove_if(targetEdid.begin(), targetEdid.end(), ::isspace), targetEdid.end());
    for (auto& c : targetEdid) c = (char)tolower((unsigned char)c);

    // 1) Query all possible paths + modes
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!GetDisplayConfigSnapshot(QDC_ALL_PATHS, paths, modes)) return false;
    if (paths.empty()) return false;

    // 2) Find the specific path index for this EDID
    UINT32 idx = 0;
    if (!FindPathIndexByEdid(paths, modes, targetEdid, idx)) return false;

    // 3) Modify the snapshot to enable/disable the found path
    auto& path = paths[idx];

    if (enabled) {
        path.flags |= DISPLAYCONFIG_PATH_ACTIVE;

        // If there is already a valid source mode index, keep it so Windows can restore layout.
        bool hasValidSourceMode = false;
        if (path.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
            path.sourceInfo.modeInfoIdx < modes.size() &&
            modes[path.sourceInfo.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
            SameAdapter(modes[path.sourceInfo.modeInfoIdx].adapterId, path.sourceInfo.adapterId) &&
            modes[path.sourceInfo.modeInfoIdx].id == path.sourceInfo.id) {
            hasValidSourceMode = true;
        }

        if (!hasValidSourceMode) {
            // extend to the right of the existing desktop
            LONG maxRight = 0;
            for (UINT32 j = 0; j < paths.size(); ++j) {
                if (!(paths[j].flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
                UINT32 midx = paths[j].sourceInfo.modeInfoIdx;
                if (midx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || midx >= modes.size()) continue;
                if (modes[midx].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
                const auto& src = modes[midx].sourceMode;
                LONG right = src.position.x + src.width;
                if (right > maxRight) maxRight = right;
            }

            // Find a source mode for this source and assign a position
            for (UINT32 m = 0; m < modes.size(); ++m) {
                if (modes[m].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
                    SameAdapter(modes[m].adapterId, path.sourceInfo.adapterId) &&
                    modes[m].id == path.sourceInfo.id) {
                    modes[m].sourceMode.position.x = maxRight;
                    modes[m].sourceMode.position.y = 0;
                    path.sourceInfo.modeInfoIdx = m;
                    break;
                }
            }
        }
    }
    else {
        path.flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
    }

    UINT32 pathCount = static_cast<UINT32>(paths.size());
    UINT32 modeCount = static_cast<UINT32>(modes.size());
    UINT32 flags = SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE;
    LONG status = SetDisplayConfig(pathCount, pathCount ? paths.data() : nullptr,
        modeCount, modeCount ? modes.data() : nullptr, flags);
    return status == ERROR_SUCCESS;
}

bool DisplayConfigUtils::IsDisplayEnabled(const std::string& edidHex)
{
    if (edidHex.empty()) {
        LOG_ERROR("IsDisplayEnabled: edidHex was empty");
        return false;
    }

    std::string target = edidHex;
    for (auto &c : target) c = (char)tolower((unsigned char)c);

    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return false;

    if (pathCount == 0) return false;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return false;

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME tname{};
        tname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tname.header.size = sizeof(tname);
        tname.header.adapterId = paths[i].targetInfo.adapterId;
        tname.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tname.header) != ERROR_SUCCESS) continue;

        if (tname.monitorDevicePath && tname.monitorDevicePath[0]) {
            try {
                std::wstring inst = std::wstring(tname.monitorDevicePath);
                const std::wstring prefix = L"\\\\?\\";
                if (inst.rfind(prefix, 0) == 0) inst = inst.substr(prefix.size());
                auto ed = GetEdidHexForDeviceInstanceId(inst);
                if (ed) {
                    std::string e = *ed;
                    for (auto &c : e) c = (char)tolower((unsigned char)c);
                    if (e == target) return true;
                }
            }
            catch (...) {}
        }
    }

    return false;
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
                                    std::wstring inst = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(mdp);
                                    const std::wstring prefix = L"\\\\?\\";
                                    if (inst.rfind(prefix, 0) == 0) inst = inst.substr(prefix.size());
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
                            // store GDI name (UTF-8) in topology
                            try {
                                t.gdiName = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(srcGdi);
                            }
                            catch (...) {
                                t.gdiName.clear();
                            }

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
