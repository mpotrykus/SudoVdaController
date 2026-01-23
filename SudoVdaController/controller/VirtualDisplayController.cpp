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
            auto mappedOpt = configStore_->GetByNameAndMode(cfgCopy.deviceName, cfgCopy.width, cfgCopy.height, cfgCopy.refreshRateMilliHz);
            if (mappedOpt.has_value() && !mappedOpt->guid.empty()) {
                auto maybeG = vdc::StringToGuid(mappedOpt->guid);
                if (maybeG.has_value()) {
                    g = *maybeG;
                }
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

    // If we have a config store mapping for this friendly name, apply stored mode
    //try {
    //    if (configStore_) {
    //        auto mappedOpt = configStore_->GetByName(cfgCopy.deviceName);
    //        if (mappedOpt.has_value()) {
    //            auto mapped = mappedOpt.value();
    //            // try to apply stored mode � retry for a short window because the OS may not be ready
    //            if (mapped.width > 0 && mapped.height > 0 && mapped.refresh > 0) {
    //                bool applied = false;
    //                const int maxAttempts = 20; // ~5s at 250ms
    //                for (int a = 0; a < maxAttempts; ++a) {
    //                    if (session->SetMode(mapped.width, mapped.height, mapped.refresh, true)) { applied = true; break; }
    //                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    //                }
    //                if (!applied) {
    //                    std::cerr << "[Controller] Warning: failed to apply stored mode for " << std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(cfgCopy.deviceName) << " after retries\n";
    //                }
    //            }
    //            // If stored GUID available and we didn't receive guidOpt, prefer reusing it
    //            if (guidOpt == std::nullopt && !mapped.guid.empty()) {
    //                auto maybeG = vdc::StringToGuid(mapped.guid);
    //                if (maybeG.has_value()) {
    //                    g = *maybeG;
    //                    session->SetGuid(g);
    //                }
    //            }
    //        }
    //    }
    //} catch(...) {}

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
            // Only set mapping if exact combo (name+mode) not already present. Use composite key name|w|h|r
            std::string composite = m.deviceName + "|" + std::to_string(m.width) + "x" + std::to_string(m.height) + "@" + std::to_string(m.refresh);
            // We store under the friendly name key but only overwrite if guid differs for this exact mode
            auto existing = configStore_->GetByNameAndMode(cfgCopy.deviceName, cfgCopy.width, cfgCopy.height, cfgCopy.refreshRateMilliHz);
            if (!existing.has_value()) {
                configStore_->SetMapping(cfgCopy.deviceName, m);
            }
            // Also mark topology: this GDI device is enabled now. Use the driver-returned GDI device name
            try { configStore_->SetTopologyEntry(*deviceName, true); } catch(...) {}
            // try { configStore_->SetTopologyEntry(cfg.deviceName, true); } catch(...) {}
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
            // when removing a virtual display, mark its GDI device as disabled
            configStore_->SetTopologyEntry(dev, false);
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
