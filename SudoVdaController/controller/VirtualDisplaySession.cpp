#include "../pch.h"
#include "VirtualDisplaySession.h"
#include "../utils/DisplayConfigUtils.h"
#include "../utils/HdrUtils.h"
#include "../models/VirtualDisplayConfig.h"

using namespace vdisplay;
using namespace vdc;

VirtualDisplaySession::VirtualDisplaySession(const GUID& guid, const std::wstring& deviceName, const VirtualDisplayConfig& config)
    : guid_(guid), deviceName_(deviceName), config_(config) {
}

VirtualDisplaySession::~VirtualDisplaySession() = default;

const GUID& VirtualDisplaySession::GetGuid() const { return guid_; }
const std::wstring& VirtualDisplaySession::GetDeviceName() const { return deviceName_; }
const VirtualDisplayConfig& VirtualDisplaySession::GetConfig() const { return config_; }

bool VirtualDisplaySession::SetMode(int width, int height, int refreshRateMilliHz, bool isolatedLayout) {
    return DisplayConfigUtils::ApplyModeForDevice(deviceName_, width, height, refreshRateMilliHz, isolatedLayout);
}

bool VirtualDisplaySession::SetPrimary() {
    return DisplayConfigUtils::MakeDevicePrimary(deviceName_);
}

bool VirtualDisplaySession::SetHdr(bool enable) {
    return HdrUtils::SetHdrState(deviceName_, enable);
}

bool VirtualDisplaySession::IsHdrEnabled() const {
    return HdrUtils::IsHdrEnabled(deviceName_);
}

std::optional<DisplayMode> VirtualDisplaySession::GetCurrentMode() const {
    return DisplayConfigUtils::GetCurrentModeForDevice(deviceName_);
}
