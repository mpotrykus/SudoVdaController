#include "ConfigStore.h"
#include "JsonUtils.h"
#include "JsonSerializers.h"
#include "StringUtils.h"
#include "../models/DisplayConfig.h"

#include <filesystem>
#include <fstream>
#include <ShlObj.h>
#include <algorithm>
#include <iostream>
#include "ContainerUtils.h"

using namespace vdc;

ConfigStore::ConfigStore() {
    // build path in %APPDATA%\SudoVdaController\monitor_map.json
    char buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, buf))) {
        std::string dir = std::string(buf) + "\\SudoVdaController";
        std::filesystem::create_directories(dir);
        path_ = dir + "\\monitor_map.json";
    }
    else {
        path_ = "monitor_map.json";
    }
    Load();
}

ConfigStore::~ConfigStore() {
    Save();
}

void ConfigStore::Load() {
    displayMap_ = DisplayMap{};
    try {

        std::ifstream ifs(path_);
        if (!ifs) return;
        std::string json((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        displayMap_ = vdc::JsonSerializer<DisplayMap>::FromJson(vdc::JsonBuilder::ParseJson(json));

    }
    catch (...) {}
}

void ConfigStore::Save() {
    try {

        std::ofstream ofs(path_, std::ios::trunc);
        if (!ofs) return;

        auto json = vdc::JsonSerializer<DisplayMap>::ToJson(displayMap_);
        ofs << json.ToString(2) << std::endl;

    }
    catch (...) {}
}

void ConfigStore::Clear() {
    displayMap_ = {};
    Save();
}

std::optional<DisplayConfig> ConfigStore::GetByNameAndMode(const std::wstring& name, int width, int height, int refreshRate) const {
    try {

        std::string nameString = WStringToString(name);
        auto displays = displayMap_.displays;
    
        auto display = std::find_if(
            displays.begin(),
            displays.end(),
            [&](const Display& d) { return d.displayName == nameString; }
        );

        if (display == displays.end()) return std::nullopt;
    
	    auto configs = display->configs;
        auto config = std::find_if(
            configs.begin(),
            configs.end(),
            [&](const DisplayConfig& c) { 
                return c.height == height 
                       && c.width == width
                       && c.refreshRateHz == refreshRate;
            }
        );

	    if (config == configs.end()) return std::nullopt;

        return *config;

    } catch (std::exception &ex) {
		std::cerr << "ConfigStore::GetByNameAndMode failed with exception: " << ex.what() << std::endl;
		return std::nullopt;
    }
}



std::optional<DisplayConfig> ConfigStore::GetByDisplayId(const std::string& displayId) const {
    try {

        for (auto display : displayMap_.displays)
        {
            for (auto config : display.configs)
            {
                if (ToLower(config.displayId) == ToLower(displayId))
                {
                    return config;
				}
            }
        }

        return std::nullopt;

    }
    catch (std::exception& ex) {
        std::cerr << "ConfigStore::GetByNameAndMode failed with exception: " << ex.what() << std::endl;
        return std::nullopt;
    }
}

std::vector<Topology> ConfigStore::GetCombinedTopology(DisplayConfig* config, bool disabledWins) const {
    
	if (config->topology.empty() && displayMap_.globalTopology.empty()) return std::vector<Topology>();
	if (config->topology.empty()) return displayMap_.globalTopology;
	if (displayMap_.globalTopology.empty()) return config->topology;

    std::vector<Topology> topology = displayMap_.globalTopology;

    for (const auto& topo : config->topology) {
        const std::string id = ToLower(topo.displayId);

        auto match = vdc::FindByKey(topology, id, [](const Topology& t) { return t.displayId; });
        if (match == topology.end()) {
            topology.push_back(topo);
            continue;
        }
        
        if (!topo.displayName.empty()) match->displayName = topo.displayName;

        match->enabled = topo.enabled;
    }

    return topology;
}

void ConfigStore::SetTopologyMergePolicyDisabledWins(bool disabledWins) {
    displayMap_.mergeDisabledWins = disabledWins;
    Save();
}

bool ConfigStore::GetTopologyMergePolicyDisabledWins() {
    return displayMap_.mergeDisabledWins;
}

bool ConfigStore::SaveDisplayConfig(std::string displayName, const DisplayConfig& displayConfig) {

   try {
        auto &displays = displayMap_.displays;
        auto displayIt = vdc::FindByKey(displays, displayName, [](const Display& d) { return d.displayName; });

        if (displayIt != displays.end()) {
            auto &configs = displayIt->configs;
            auto configIt = std::find_if(
                configs.begin(),
                configs.end(),
                [&](const DisplayConfig& c) {
                    return c.width == displayConfig.width
                           && c.height == displayConfig.height
                           && c.refreshRateHz == displayConfig.refreshRateHz;
                }
            );

            if (configIt != configs.end()) {
                *configIt = displayConfig;
            } else {
                configs.push_back(displayConfig);
            }

        } else {
            Display d;
            d.displayName = displayName;
            d.configs.push_back(displayConfig);
            displays.push_back(std::move(d));
        }

        Save();
        return true;

    } catch (std::exception &ex) {
        std::cerr << "ConfigStore::SaveDisplayConfig failed with exception: " << ex.what() << std::endl;
		return false;
    } catch (...) {
		std::cerr << "ConfigStore::SaveDisplayConfig failed. Unkown Exception.\n";
        return false;
    }
}
