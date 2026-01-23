#pragma once
#include <string>
#include <map>
#include <optional>
#include <windows.h>
#include <vector>

namespace vdc {

struct StoredMapping {
    std::string guid; // string form
    int width = 0;
    int height = 0;
    int refresh = 0; // milliHz
    unsigned long long adapterLuid = 0;
    std::string deviceName; // friendly name
};

class ConfigStore {
public:
    ConfigStore();
    ~ConfigStore();

    void Load();
    void Save();

    // Lookup by friendly name + optional adapter; deprecated - use GetByNameAndMode
    // (removed: only exact name+mode matches are supported)
    // Lookup by friendly name + exact mode combo
    std::optional<StoredMapping> GetByNameAndMode(const std::wstring& name, int width, int height, int refresh) const;

    // Set mapping for a given name (overwrites existing)
    void SetMapping(const std::wstring& name, const StoredMapping& m);

    // Topology persistence: mark whether a GDI device name is enabled (attached to desktop)
    void SetTopologyEntry(const std::wstring& gdiName, bool enabled);
    std::map<std::string,bool> GetTopologyMap() const;

private:
    std::map<std::string, std::vector<StoredMapping>> map_; // key is UTF8 name -> list of mappings
    std::map<std::string, bool> topology_; // GDI name -> enabled
    std::string path_;
};

}
