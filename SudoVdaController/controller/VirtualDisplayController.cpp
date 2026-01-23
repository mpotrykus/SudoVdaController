#include "../pch.h"
#include "VirtualDisplayController.h"
#include "../utils/JsonUtils.h"
#include "../utils/GuidUtils.h"
#include "VdaSession.h"
#include "VirtualDisplaySession.h"
#include "../utils/DisplayConfigUtils.h"
#include "../utils/HdrUtils.h"
#include "../utils/ConfigStore.h"


#include <thread>
#include <chrono>
#include <iostream>
#include <locale>
#include <codecvt>
#include <set>

using namespace vdc;
using namespace vdisplay;

VirtualDisplayController::VirtualDisplayController() {
    vda_ = std::make_unique<VdaSession>();
    vda_->Open();
    // load config store
    try { configStore_ = std::make_unique<ConfigStore>(); } catch(...) {}
}

VirtualDisplayController::~VirtualDisplayController() = default;

ControllerResult VirtualDisplayController::CreateDisplay(const VirtualDisplayConfig& cfg, const std::optional<GUID>& guidOpt) {
    GUID g = guidOpt.value_or(GenerateGuid());
    // If caller didn't provide a friendly name, use the driver's default friendly name
    // so menus can show a reasonable label. The driver typically assigns "SudoMakerVDD".
    VirtualDisplayConfig cfgCopy = cfg;
    if (cfgCopy.deviceName.empty()) cfgCopy.deviceName = L"SudoMakerVDD";

    // If we have a stored mapping for this friendly name+mode and the caller did not supply a GUID,
    // prefer to reuse the stored GUID for that exact combo so the driver receives the stable identity.
    try {
        if (guidOpt == std::nullopt && configStore_) {
            // Try exact friendly name+mode match first
            auto mappedOpt = configStore_->GetByNameAndMode(cfgCopy.deviceName, cfgCopy.width, cfgCopy.height, cfgCopy.refreshRateMilliHz);
            if (mappedOpt.has_value() && !mappedOpt->guid.empty()) {
                auto maybeG = vdc::StringToGuid(mappedOpt->guid);
                if (maybeG.has_value()) { g = *maybeG; }
            } else {
                // Otherwise attempt identifier-based match: EDID -> WMI -> monitorDevicePath
                try {
                    std::unordered_set<std::string> ids;
                    // get driver-returned deviceName from the (not yet created) device; use cfgCopy.deviceName? use current system
                    // attempt to query DisplayConfig for the GDI name that will correspond to the new device; use deviceName after creation
                    // We will try to query using existing system GDI names matching the friendly name
                    // collect candidates by enumerating current DisplayConfig paths and looking for monitorDevicePath/friendly
                    UINT32 pathCount=0, modeCount=0;
                    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS && pathCount>0) {
                        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
                        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
                        if (QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) == ERROR_SUCCESS) {
                            for (UINT32 i=0;i<pathCount;++i) {
                                DISPLAYCONFIG_SOURCE_DEVICE_NAME src{}; src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME; src.header.size = sizeof(src);
                                src.header.adapterId = paths[i].sourceInfo.adapterId; src.header.id = paths[i].sourceInfo.id;
                                if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
                                std::wstring gdi = src.viewGdiDeviceName;
                                std::string gdiUtf;
                                try { gdiUtf = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(gdi); } catch(...) { gdiUtf = ""; }
                                for (auto &c: gdiUtf) c = (char)tolower((unsigned char)c);
                                // If friendly name matches requested friendly name, collect identifiers
                                try {
                                    auto fn = vdc::DisplayConfigUtils::GetMonitorFriendlyNameForGdiName(gdi);
                                    std::string fnUtf;
                                    try { fnUtf = fn ? std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(*fn) : std::string(); } catch(...) { fnUtf = ""; }
                                    for (auto &c: fnUtf) c = (char)tolower((unsigned char)c);
                                    std::string targetFriendly;
                                    try { targetFriendly = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(cfgCopy.deviceName); } catch(...) { targetFriendly = ""; }
                                    for (auto &c: targetFriendly) c = (char)tolower((unsigned char)c);
                                    if (!targetFriendly.empty() && fnUtf == targetFriendly) {
                                        DISPLAYCONFIG_TARGET_DEVICE_NAME tname{}; tname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME; tname.header.size = sizeof(tname);
                                        tname.header.adapterId = paths[i].targetInfo.adapterId; tname.header.id = paths[i].targetInfo.id;
                                        if (DisplayConfigGetDeviceInfo(&tname.header) == ERROR_SUCCESS) {
                                            if (tname.monitorDevicePath && tname.monitorDevicePath[0]) {
                                                std::string p;
                                                try { p = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(std::wstring(tname.monitorDevicePath)); } catch(...) { p = ""; }
                                                for (auto &c: p) c = (char)tolower((unsigned char)c);
                                                ids.insert(p);
                                                const std::string pref = "\\\\?\\";
                                                if (p.rfind(pref,0) == 0) ids.insert(p.substr(pref.size()));
                                                try {
                                                    std::wstring inst = std::wstring(tname.monitorDevicePath);
                                                    const std::wstring ipref = L"\\\\?\\";
                                                    if (inst.rfind(ipref,0) == 0) inst = inst.substr(ipref.size());
                                                    auto ed = vdc::DisplayConfigUtils::GetEdidHexForDeviceInstanceId(inst);
                                                    if (ed) { std::string e = *ed; for (auto &c: e) c = (char)tolower((unsigned char)c); ids.insert(e); }
                                                    auto wk = vdc::DisplayConfigUtils::GetWmiKeyForDeviceInstanceId(inst);
                                                    if (wk) { std::string w = *wk; for (auto &c: w) c = (char)tolower((unsigned char)c); ids.insert(w); }
                                                } catch(...) {}
                                            }
                                        }
                                    }
                                } catch(...) {}
                            }
                        }
                    }
                    if (!ids.empty()) {
                        auto byId = configStore_->FindMappingByIdentifiers(ids);
                        if (byId) {
                            auto maybeG = vdc::StringToGuid(byId->guid);
                            if (maybeG.has_value()) g = *maybeG;
                        }
                    }
                } catch(...) {}
            }
        }
    } catch(...) {}

    // Ask VDA to add display (stub behavior)
    auto deviceName = vda_->AddVirtualDisplay(g, cfgCopy);
    JsonBuilder jb;
    if (!deviceName) {
        jb.Add("error", "failed to add virtual display");
        return { false, "failed", jb.Build() };
    }

    // Create the session object
    // Store the session. Use the driver-returned deviceName as the GDI name and
    // keep the friendly name from cfgCopy so the UI can display it.
    auto session = std::make_unique<vdisplay::VirtualDisplaySession>(g, *deviceName, cfgCopy);

    if (cfg.hdr && !session->SetHdr(true)) {
        std::string devUtf8 = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(*deviceName);
        std::cerr << "[Controller] WARNING: failed to enable HDR on device: " << devUtf8 << "\n";
        jb.Add("warning", "failed to enable hdr on device");
    }

    if (cfg.primary && !session->SetPrimary()) {
        std::string devUtf8 = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(*deviceName);
        std::cerr << "[Controller] WARNING: failed to set as primary on device: " << devUtf8 << "\n";
        jb.Add("warning", "failed to set device as primary");
    }

    // Store session
    sessions_.emplace(g, std::move(session));
    // Persist mapping: record deviceName -> guid and current mode (if available)
    try {
        if (configStore_) {
            StoredMapping m;
            m.guid = vdc::GuidToString(g);
            m.deviceName = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(cfgCopy.deviceName);
            auto mode = sessions_[g]->GetCurrentMode();
            if (mode) {
                m.width = mode->width;
                m.height = mode->height;
                m.refresh = mode->refreshRateMilliHz;
            } 

                    // Attempt to capture persistent identifiers: monitorDevicePath and EDID when available
                    try {
                        auto mdp = vdc::DisplayConfigUtils::GetMonitorDevicePathForGdiName(*deviceName);
                        if (mdp) m.monitorDevicePath = *mdp;
                        // If we have a monitor device path, try to get EDID from registry
                        if (m.monitorDevicePath.empty()) {
                            // try target device instance id via QueryDisplayConfig target info
                            // best-effort: iterate display paths to find matching source and read target.monitorDevicePath
                            UINT32 pCount=0, mCount=0;
                            if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pCount, &mCount) == ERROR_SUCCESS) {
                                std::vector<DISPLAYCONFIG_PATH_INFO> paths(pCount);
                                std::vector<DISPLAYCONFIG_MODE_INFO> modes(mCount);
                                if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pCount, paths.data(), &mCount, modes.data(), nullptr) == ERROR_SUCCESS) {
                                    for (UINT32 pi=0; pi<pCount; ++pi) {
                                        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
                                        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                                        src.header.size = sizeof(src);
                                        src.header.adapterId = paths[pi].sourceInfo.adapterId;
                                        src.header.id = paths[pi].sourceInfo.id;
                                        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
                                        if (std::wstring(src.viewGdiDeviceName) != *deviceName) continue;
                                        DISPLAYCONFIG_TARGET_DEVICE_NAME tname{};
                                        tname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                                        tname.header.size = sizeof(tname);
                                        tname.header.adapterId = paths[pi].targetInfo.adapterId;
                                        tname.header.id = paths[pi].targetInfo.id;
                                        if (DisplayConfigGetDeviceInfo(&tname.header) == ERROR_SUCCESS) {
                                            if (tname.monitorDevicePath && tname.monitorDevicePath[0]) m.monitorDevicePath = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(std::wstring(tname.monitorDevicePath));
                                            if (m.monitorDevicePath.empty() && tname.monitorFriendlyDeviceName && tname.monitorFriendlyDeviceName[0]) m.deviceName = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(std::wstring(tname.monitorFriendlyDeviceName));
                                        }
                                    }
                                }
                            }
                        }
                        // Try EDID read via device instance id if monitorDevicePath contains a device instance id
                        if (!m.monitorDevicePath.empty()) {
                            std::wstring inst = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(m.monitorDevicePath);
                            const std::wstring prefix = L"\\\\?\\";
                            if (inst.rfind(prefix,0) == 0) inst = inst.substr(prefix.size());
                            auto ed = vdc::DisplayConfigUtils::GetEdidHexForDeviceInstanceId(inst);
                            if (ed) m.edid = *ed;
                        }
                    // If EDID still empty, try fallback: enumerate display devices to find monitor DeviceID and read EDID
                    if (m.edid.empty()) {
                        DISPLAY_DEVICEW adapter{}; adapter.cb = sizeof(adapter);
                        for (DWORD ai = 0; EnumDisplayDevicesW(NULL, ai, &adapter, 0); ++ai) {
                            std::wstring adapterName = adapter.DeviceName ? adapter.DeviceName : L"";
                            if (adapterName != *deviceName) { adapter.cb = sizeof(adapter); continue; }
                            // enumerate monitors on this adapter
                            DISPLAY_DEVICEW mon{}; mon.cb = sizeof(mon);
                            for (DWORD mi = 0; EnumDisplayDevicesW(adapterName.c_str(), mi, &mon, 0); ++mi) {
                                std::wstring devId = mon.DeviceID ? mon.DeviceID : L"";
                                if (devId.empty()) continue;
                                // convert DeviceID like "MONITOR\\..." into instance id form
                                // try directly
                                auto ed = vdc::DisplayConfigUtils::GetEdidHexForDeviceInstanceId(devId);
                                if (!ed) {
                                    // try replacing '#' with '\\' as alternate form
                                    std::wstring alt = devId;
                                    for (auto &c : alt) if (c == L'#') c = L'\\';
                                    ed = vdc::DisplayConfigUtils::GetEdidHexForDeviceInstanceId(alt);
                                }
                                if (ed) { m.edid = *ed; break; }
                                mon.cb = sizeof(mon);
                            }
                            adapter.cb = sizeof(adapter);
                            if (!m.edid.empty()) break;
                        }
                    }
                    } catch(...) {}

            // We store under the friendly name key but only overwrite if guid differs for this exact mode
            auto existing = configStore_->GetByNameAndMode(cfgCopy.deviceName, cfgCopy.width, cfgCopy.height, cfgCopy.refreshRateMilliHz);
            if (!existing.has_value()) {
                configStore_->SetMapping(cfgCopy.deviceName, m);
            }
            // Always ensure the mapping GUID is present in the topology so newly-created virtual displays
            // can be discovered even when an existing mapping already existed for the same friendly+mode.
            try {
                std::wstring guidW = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(m.guid);
                configStore_->SetTopologyEntry(guidW, true);
            } catch(...) {}

            // Persist WMI key if available as fallback identifier
            try {
                if (!m.monitorDevicePath.empty()) {
                    std::wstring inst = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(m.monitorDevicePath);
                    const std::wstring prefix = L"\\\\?\\";
                    if (inst.rfind(prefix,0) == 0) inst = inst.substr(prefix.size());
                    auto wk = vdc::DisplayConfigUtils::GetWmiKeyForDeviceInstanceId(inst);
                    if (wk) m.wmiKey = *wk;
                }
            } catch(...) {}

            // Also mark topology in the mapping: set this mapping to enable the newly created device
            // and disable other currently-active GDIs so restoring this mapping will leave only it enabled.
            try {
                // Build desired mapping-only topology: enable the newly-created device, disable all other active GDI names
                std::map<std::string,bool> desired;
                UINT32 pathCount = 0, modeCount = 0;
                if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS && pathCount > 0) {
                    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
                    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
                    if (QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) == ERROR_SUCCESS) {
                        for (UINT32 i = 0; i < pathCount; ++i) {
                            DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
                            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                            src.header.size = sizeof(src);
                            src.header.adapterId = paths[i].sourceInfo.adapterId;
                            src.header.id = paths[i].sourceInfo.id;
                            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
                            std::wstring gdi = src.viewGdiDeviceName;
                            std::string gdiUtf;
                            try { gdiUtf = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(gdi); } catch(...) { gdiUtf = ""; }
                            for (auto &c : gdiUtf) c = (char)tolower((unsigned char)c);
                            // default: disable others
                            desired[gdiUtf] = false;
                        }
                    }
                }
                // ensure the created device is enabled (add if missing)
                std::string createdUtf;
                try { createdUtf = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(*deviceName); } catch(...) { createdUtf = ""; }
                for (auto &c : createdUtf) c = (char)tolower((unsigned char)c);
                desired[createdUtf] = true;

                // Do not persist the "desired" map yet; apply merged topology first and persist only on success.
                // Apply merged topology (global + per-mapping) to respect other persisted entries.
                // Log the merged topology for diagnostics.
                const int maxAttempts = 10;
                const std::chrono::milliseconds waitInterval(200);
                for (int a = 0; a < maxAttempts; ++a) {
                    try {
                        auto toApply = configStore_->GetCombinedTopology(configStore_->GetTopologyMergePolicyDisabledWins());
                        // diagnostic: print toApply contents
                        std::cerr << "[Controller] Applying merged topology:";
                        for (const auto &kv : toApply) std::cerr << " " << kv.first << ":" << (kv.second?"1":"0");
                        std::cerr << "\n";
                        if (vdc::DisplayConfigUtils::ApplyTopologyFromStore(toApply)) {
                            // Persist merged topology back into mapping so future recreates restore same state
                            try { configStore_->UpdateMappingTopologyFromCombined(g, toApply); } catch(...) {}
                            break;
                        }
                    } catch(...) {}
                    std::this_thread::sleep_for(waitInterval);
                }
            } catch(...) {}
        }
    } catch(...) {}
    jb.Add("guid", GuidToString(g));
    jb.Add("device", std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(*deviceName));
    jb.AddRaw("success", "true");
    return { true, "created", jb.Build() };
}

ControllerResult VirtualDisplayController::RemoveDisplay(const GUID& guid) {
    JsonBuilder jb;
    auto it = sessions_.find(guid);
    if (it == sessions_.end()) {
        jb.Add("error", "not found");
        return { false, "not found", jb.Build() };
    }
    // Before removing, record topology: mark the physical GDI name as enabled/disabled state
    try {
        if (configStore_) {
            auto dev = it->second->GetDeviceName();
            // when removing a virtual display, mark its GDI device as disabled (update mapping)
            configStore_->UpdateTopologyForGdi(guid, dev, false);
        }
    } catch(...) {}

    if (!vda_->RemoveVirtualDisplay(guid)) {
        jb.Add("error", "driver remove failed");
        return { false, "driver remove failed", jb.Build() };
    }
    sessions_.erase(it);
    jb.AddRaw("success", "true");
    return { true, "removed", jb.Build() };
}

ControllerResult VirtualDisplayController::SetMode(const GUID& guid, int w, int h, int refreshMilliHz, bool isolatedLayout) {
    JsonBuilder jb;
    auto it = sessions_.find(guid);
    if (it == sessions_.end()) { jb.Add("error", "not found"); return { false, "not found", jb.Build() }; }
    bool ok = it->second->SetMode(w, h, refreshMilliHz, isolatedLayout);
    jb.AddRaw("success", ok ? "true" : "false");
    return { ok, ok ? "ok" : "failed", jb.Build() };
}

ControllerResult VirtualDisplayController::SetPrimary(const GUID& guid) {
    JsonBuilder jb;
    auto it = sessions_.find(guid);
    if (it == sessions_.end()) { jb.Add("error", "not found"); return { false, "not found", jb.Build() }; }
    bool ok = it->second->SetPrimary();
    jb.AddRaw("success", ok ? "true" : "false");
    return { ok, ok ? "ok" : "failed", jb.Build() };
}

ControllerResult VirtualDisplayController::SetHdr(const GUID& guid, bool enable) {
    JsonBuilder jb;
    auto it = sessions_.find(guid);
    if (it == sessions_.end()) { jb.Add("error", "not found"); return { false, "not found", jb.Build() }; }
    bool ok = it->second->SetHdr(enable);
    jb.AddRaw("success", ok ? "true" : "false");
    return { ok, ok ? "ok" : "failed", jb.Build() };
}

ControllerResult VirtualDisplayController::Query(const GUID& guid) {
    JsonBuilder jb;
    auto it = sessions_.find(guid);
    if (it == sessions_.end()) { jb.Add("error", "not found"); return { false, "not found", jb.Build() }; }
    auto mode = it->second->GetCurrentMode();
    jb.Add("guid", GuidToString(guid));
    // include the friendly name supplied at creation (if any)
    try {
        std::string friendly = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(it->second->GetConfig().deviceName);
        jb.Add("name", friendly);
    } catch(...) {
        // ignore conversion errors
    }
    if (mode) {
        jb.AddRaw("width", std::to_string(mode->width));
        jb.AddRaw("height", std::to_string(mode->height));
        jb.AddRaw("refreshMilliHz", std::to_string(mode->refreshRateMilliHz));
    }
    else {
        jb.Add("mode", "unknown");
    }
    jb.AddRaw("hdr", it->second->IsHdrEnabled() ? "true" : "false");
    return { true, "ok", jb.Build() };
}

size_t VirtualDisplayController::CountDisplays() const {
    return sessions_.size();
}

std::vector<std::pair<GUID, std::wstring>> VirtualDisplayController::ListDisplays() const {
    std::vector<std::pair<GUID, std::wstring>> out;
    out.reserve(sessions_.size());
    // First enumerate physical displays and add them (marked with empty GUID)
    std::set<std::wstring> virtualGdiNames;
    for (const auto& kv : sessions_) {
        virtualGdiNames.insert(kv.second->GetDeviceName());
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
            } catch(...) {}
            // Fallback: try device instance id from EnumDisplayDevices
            if (friendly.empty() && mon.DeviceID) {
                try {
                    auto regName = vdc::DisplayConfigUtils::GetMonitorFriendlyNameFromDeviceInstanceId(mon.DeviceID);
                    if (regName && !regName->empty()) friendly = *regName;
                } catch(...) {}
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
    for (const auto& kv : sessions_) {
        // Show friendly name (if provided in config) followed by the GDI device name in parentheses
        const std::wstring devName = kv.second->GetDeviceName();
        const vdc::VirtualDisplayConfig& cfg = kv.second->GetConfig();
        std::wstring label;
        if (!cfg.deviceName.empty()) label = cfg.deviceName + L" (" + devName + L")";
        else label = devName;
        out.emplace_back(kv.first, label);
    }
    return out;
}
