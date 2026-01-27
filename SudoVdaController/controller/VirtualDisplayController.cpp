#include "../pch.h"
#include "VirtualDisplayController.h"
#include "../utils/JsonUtils.h"
#include "../utils/GuidUtils.h"
#include "../utils/DisplayConfigUtils.h"
#include "../utils/HdrUtils.h"
#include "../utils/ConfigStore.h"


#include <iostream>
#include <locale>
#include <codecvt>
#include <thread>
#include "../utils/StringUtils.h"
#include "../utils/ContainerUtils.h"
#include "../utils/Logger.h"

using namespace vdc;
using namespace vdisplay;

VirtualDisplayController::VirtualDisplayController() {
    virtualDisplayService_ = std::make_unique<VirtualDisplayService>();
    virtualDisplayService_->Open();
    configStore_ = std::make_unique<ConfigStore>();
}

VirtualDisplayController::~VirtualDisplayController() = default;

bool VirtualDisplayController::CreateDisplay(const VirtualDisplay& cfg, const std::optional<GUID>& guidOpt) {

    try {
        VirtualDisplay virtualDisplay = cfg;
	    DisplayConfig displayConfig = FindExistingDisplayConfigOrGenerate(virtualDisplay, guidOpt);
	    GUID displayId = StringToGuid(displayConfig.displayId).value();
        float fpsHz = static_cast<float>(virtualDisplay.refreshRateMilliHz) / 1000.0f;

        auto gdiName = virtualDisplayService_->createVirtualDisplay(GuidToString(displayId).c_str(), WStringToString(virtualDisplay.deviceName).c_str(),
                                                                    virtualDisplay.width, virtualDisplay.height, fpsHz, displayId);

        if (!gdiName) {
            LOG_ERROR("Failed to add virtual display");
            return false;
        }

        virtualDisplay.gdiName = *gdiName;
        auto session = std::make_unique<VirtualDisplay>(virtualDisplay);
        virtualDisplays_.emplace(displayId, std::move(session));

        

        if (cfg.hdr) {
            std::string devNameCopy = WStringToString(virtualDisplay.deviceName);
            std::thread([this, displayId, devNameCopy]() {
                auto res = SetHdr(displayId, true);
                if (res) {
                    LOG_INFO("Succesfully enabled HDR on device '%s'", devNameCopy);
                } else {
                    LOG_ERROR("Failed to enable HDR on device '%s'", devNameCopy);
                }
            }).detach();
        }

        if (cfg.primary) {
            std::string devNameCopy = WStringToString(virtualDisplay.deviceName);
            std::thread([this, displayId, devNameCopy]() {
                auto res = SetPrimary(displayId);
                if (res) {
                    LOG_INFO("Succesfully set '%s' as primary device", devNameCopy);
                } else {
                    LOG_ERROR("Failed to set '%s' as primary device", devNameCopy);
                }
            }).detach();
        }

        if (configStore_) {
            DisplayConfig cfgCopy = displayConfig;
            std::string devNameCopy = WStringToString(virtualDisplay.deviceName);
            std::thread([this, displayId, virtualDisplay = std::move(virtualDisplay), cfgCopy, devNameCopy]() mutable {
                auto res = AddNewDisplayToConfigStore(displayId, virtualDisplay, cfgCopy);
                if (res) {
                    std::string message = "Successfully saved device to config for '" + devNameCopy + "'";
                    LOG_INFO(message.c_str());
                }
                else {
                    std::string message = "Failed to save device to config for '" + devNameCopy + "'";
                    LOG_ERROR(message.c_str());
                }
            }).detach();
        }

        std::string createdName = WStringToString(cfg.deviceName);
        std::string createdGdi = WStringToString(gdiName.value());
        std::string createdId = displayConfig.displayId;
        std::string successMsg = "Created '" + createdName + "' (" + createdGdi + ") with id: " + createdId;
        LOG_SUCCESS(successMsg.c_str());
        return true;
    }
    catch (const std::exception& ex) {
        LOG_ERROR("Failed to create display: %s", ex.what());
        return false;
    }
}

bool VirtualDisplayController::RemoveDisplay(const GUID& guid) {

    try {
        auto virtualDisplay = virtualDisplays_.find(guid);
        if (virtualDisplay == virtualDisplays_.end()) {
            throw std::exception("The display could not be found");
        }

        if (configStore_) {
            auto displayConfigOpt = configStore_->GetByDisplayId(GuidToString(guid));
            if (displayConfigOpt.has_value()) {
                auto displayConfig = UpdateConfigTopogology(displayConfigOpt.value(), true);
                configStore_->SaveDisplayConfig(WStringToString(virtualDisplay->second->deviceName), displayConfig);
            }
            else {
                LOG_WARN("No display config found in store for removed display: %s", GuidToString(guid));
            }
        }

        if (!virtualDisplayService_->removeVirtualDisplay(guid)) {
            throw std::exception("Driver failed to remove display");
        }

        virtualDisplays_.erase(virtualDisplay);
        
        return true;
    } catch (const std::exception& ex) {
		std::string guidStr = GuidToString(guid);
        LOG_ERROR("Failed to remove display with id (%s): %s", guidStr, ex.what());
        return false;
	}
}

bool VirtualDisplayController::SetMode(const GUID& guid, int w, int h, int refreshMilliHz, bool isolatedLayout) {
    try {
        auto virtualDisplay = virtualDisplays_.find(guid);

        if (virtualDisplay == virtualDisplays_.end()) { 
            throw std::exception("The display could not be found"); 
        }

        if (!DisplayConfigUtils::ApplyModeForDevice(virtualDisplay->second->gdiName, w, h, refreshMilliHz, isolatedLayout)) {
            throw std::exception("Failed to set mode on display");
		}

        return true;
    } catch (const std::exception& ex) {
		std::string guidStr = GuidToString(guid);
        LOG_ERROR("Failed to set mode with id (%s) : %s", guidStr, ex.what());
        return false;
    }
}

bool VirtualDisplayController::SetPrimary(const GUID& guid) {
    try {
        auto virtualDisplay = virtualDisplays_.find(guid);

        if (virtualDisplay == virtualDisplays_.end()) {
            throw std::exception("The display could not be found");
        }

        if (!DisplayConfigUtils::MakeDevicePrimary(virtualDisplay->second->gdiName)) {
			throw std::exception("Failed to set display as primary");
        }

        return true;
    } catch (const std::exception& ex) {
		std::string guidStr = GuidToString(guid);
        LOG_ERROR("Failed to set primary with id (%s) : %s", guidStr, ex.what());
        return false;
    }
}

bool VirtualDisplayController::SetHdr(const GUID& guid, bool enable) {
    try {
        auto virtualDisplay = virtualDisplays_.find(guid);

        if (virtualDisplay == virtualDisplays_.end()) {
            throw std::exception("The display could not be found");
        }
        
        if (!HdrUtils::SetHdrState(virtualDisplay->second->gdiName, enable)) {
			throw std::exception("Failed to set HDR state on display");
        }

        return true;
    } catch (const std::exception& ex) {
		std::string guidStr = GuidToString(guid);
		std::string colorSpace = enable ? "HDR" : "SDR";
        LOG_ERROR("Failed to set %s with id (%s) : %s", colorSpace, guidStr, ex.what());
        return false;
    }
}

bool VirtualDisplayController::Query(const GUID& guid) {
    try {
        auto virtualDisplay = virtualDisplays_.find(guid);

        if (virtualDisplay == virtualDisplays_.end()) {
            throw std::exception("The display could not be found");
        }

        auto mode = DisplayConfigUtils::GetCurrentModeForDevice(virtualDisplay->second->gdiName);
        if (!mode) {
            LOG_WARN("Mode returned 'unknown'");
            return false;
        }

        std::string friendly = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(virtualDisplay->second->deviceName);
	    auto colorSpace = HdrUtils::IsHdrEnabled(virtualDisplay->second->deviceName) ? "HDR" : "SDR";
        LOG_INFO("Mode Found:");
        LOG_INFO("%s (%s)", friendly, GuidToString(guid));
        LOG_INFO("%sx%s@%s %s", std::to_string(mode->width), std::to_string(mode->height), std::to_string(mode->refreshRateMilliHz), colorSpace);

        return true;
    }
    catch (const std::exception& ex) {
		std::string guidStr = GuidToString(guid);
        LOG_ERROR("Query failed with id (%s) : %s", guidStr, ex.what());
        return false;
    }
}

size_t VirtualDisplayController::CountDisplays() const {
    return virtualDisplays_.size();
}

std::vector<std::pair<GUID, std::wstring>> VirtualDisplayController::ListDisplays() const {
    std::vector<std::pair<GUID, std::wstring>> virtualDisplays;
    virtualDisplays.reserve(virtualDisplays_.size());

    for (const auto& virtualDisplay : virtualDisplays_)
    {
		virtualDisplays.push_back({ virtualDisplay.first, virtualDisplay.second->deviceName });
    }

    DisplayConfigUtils utils;
    return utils.ListDisplays(virtualDisplays);
}

bool VirtualDisplayController::AddNewDisplayToConfigStore(GUID displayId,
                                                          VirtualDisplay virtualDisplay,
                                                          DisplayConfig displayConfig)
{
    // Verify mode
    auto mode = DisplayConfigUtils::GetCurrentModeForDevice(virtualDisplay.deviceName);
    if (mode) {
        displayConfig.displayId = vdc::GuidToString(displayId);
        displayConfig.width = mode->width;
        displayConfig.height = mode->height;
        displayConfig.refreshRateHz = mode->refreshRateMilliHz;
    }

    // Apply topology from config store (JSON is source of truth)
    std::map<std::string, bool> topologyMap;
    auto toApply = configStore_->GetCombinedTopology(&displayConfig,
                                                     configStore_->GetTopologyMergePolicyDisabledWins());
    if (!toApply.empty()) {
        std::string merged;
        merged.reserve(256);
        for (const auto& t : toApply) {
            if (!merged.empty()) merged += ' ';
            merged += t.displayName + ":" + (t.enabled ? "1" : "0");

            if (!t.edid.empty()) topologyMap.insert_or_assign(t.edid, t.enabled);
            if (!t.displayId.empty()) topologyMap.insert_or_assign(t.displayId, t.enabled);
            if (!t.displayName.empty()) topologyMap.insert_or_assign(t.displayName, t.enabled);
        }

        std::string message = "Applying merged topology: " + merged;
        LOG_INFO(message.c_str());

        if (!vdc::DisplayConfigUtils::ApplyTopologyFromStore(topologyMap)) {
            LOG_ERROR("Failed to apply merged topology from config store");
            return false;
        }

        LOG_INFO("Succesfully applied merged topology from config store");
        return true;
    }

    // Update device topology snapshot based on what Windows actually has active now
    displayConfig = UpdateConfigTopogology(displayConfig, false);

    // Save
    configStore_->SaveDisplayConfig(WStringToString(virtualDisplay.deviceName), displayConfig);
}


DisplayConfig VirtualDisplayController::FindExistingDisplayConfigOrGenerate(const VirtualDisplay& cfg, const std::optional<GUID>& guidOpt) {

    if (auto mappedOpt = configStore_->GetByNameAndMode(cfg.deviceName, cfg.width, cfg.height, cfg.refreshRateMilliHz); 
        mappedOpt.has_value() && !mappedOpt->displayId.empty()) {
        return mappedOpt.value();
    }

    GUID g = guidOpt.value_or(GenerateGuid());

    DisplayConfig dCfg;
    dCfg.displayId = vdc::GuidToString(g);
    dCfg.width = cfg.width;
    dCfg.height = cfg.height;
    dCfg.refreshRateHz = cfg.refreshRateMilliHz;

    return dCfg;
}

DisplayConfig VirtualDisplayController::UpdateConfigTopogology(DisplayConfig displayConfig, bool overrideEnabled)
{
    auto utils = DisplayConfigUtils();
    std::vector<std::pair<GUID, std::wstring>> vds;
    vds.reserve(virtualDisplays_.size());
    for (const auto& kv : virtualDisplays_) vds.push_back({ kv.first, kv.second->gdiName });
    auto physicalTopologies = utils.GetActiveDisplayTopology(vds);
    for (const auto& phys : physicalTopologies) {
        auto match = vdc::FindByKey(displayConfig.topology, phys.displayId, [](const Topology& t) { return t.displayId; });
        if (match != displayConfig.topology.end()) {
            if (overrideEnabled) {
                match->enabled = phys.enabled;
            }
        }
        else {
            displayConfig.topology.push_back(phys);
        }
    }
    return displayConfig;
}