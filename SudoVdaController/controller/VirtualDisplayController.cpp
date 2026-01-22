#include "../pch.h"
#include "VirtualDisplayController.h"
#include "../utils/JsonUtils.h"
#include "../utils/GuidUtils.h"
#include "VdaSession.h"
#include "VirtualDisplaySession.h"
#include "../utils/DisplayConfigUtils.h"
#include "../utils/HdrUtils.h"

#include <thread>
#include <chrono>
#include <iostream>
#include <locale>
#include <codecvt>

using namespace vdc;
using namespace vdisplay;

VirtualDisplayController::VirtualDisplayController() {
    vda_ = std::make_unique<VdaSession>();
    vda_->Open();
}

VirtualDisplayController::~VirtualDisplayController() = default;

ControllerResult VirtualDisplayController::CreateDisplay(const VirtualDisplayConfig& cfg, const std::optional<GUID>& guidOpt) {
    GUID g = guidOpt.value_or(GenerateGuid());
    // Ask VDA to add display (stub behavior)
    auto deviceName = vda_->AddVirtualDisplay(g, cfg);
    JsonBuilder jb;
    if (!deviceName) {
        jb.Add("error", "failed to add virtual display");
        return { false, "failed", jb.Build() };
    }

    // Create the session object
    auto session = std::make_unique<vdisplay::VirtualDisplaySession>(g, *deviceName, cfg);

    // If the config requested HDR, try to enable it now.
    if (cfg.hdr && !session->SetHdr(true)) {
        std::string devUtf8 = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(*deviceName);
        std::cerr << "[Controller] WARNING: failed to enable HDR on device: " << devUtf8 << "\n";
        jb.Add("warning", "failed to enable hdr on device");
    }

    // Store session
    sessions_.emplace(g, std::move(session));
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
    for (const auto& kv : sessions_) {
        out.emplace_back(kv.first, kv.second->GetDeviceName());
    }
    return out;
}
