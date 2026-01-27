#pragma once
#include <string>
#include <optional>
#include <vector>
#include "../models/DisplayConfig.h"

namespace vdc {

class ConfigStore {
public:
    ConfigStore();
    ~ConfigStore();

    void Load();
    void Save();
    void Clear();

    std::optional<DisplayConfig> GetByNameAndMode(const std::wstring& name, int width, int height, int refresh) const;
    std::optional<DisplayConfig> GetByDisplayId(const std::string& displayId) const;
    std::vector<Topology> GetCombinedTopology(DisplayConfig* config, bool disabledWins) const;

    void SetTopologyMergePolicyDisabledWins(bool disabledWins);
    bool GetTopologyMergePolicyDisabledWins();

    bool SaveDisplayConfig(std::string displayName, const DisplayConfig& displayConfig);

private:
    DisplayMap displayMap_;
    std::string path_;
};

}
