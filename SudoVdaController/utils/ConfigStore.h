#pragma once
#include <string>
#include <map>
#include <optional>
#include <windows.h>
#include <vector>
#include <unordered_set>

namespace vdc {

struct StoredMapping {
    std::string guid; // string form
    int width = 0;
    int height = 0;
    int refresh = 0; // milliHz
    unsigned long long adapterLuid = 0;
    std::string deviceName; // friendly name
    std::string monitorDevicePath; // persisted monitor device path (\?\...)
    std::string edid; // EDID as hex string
    std::string wmiKey; // WMI fallback identifier (manufacturer-product-serial)
    std::map<std::string,bool> topology; // optional per-mapping topology (gdi -> enabled)
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
    // Find a stored mapping by any of the provided identifiers (edid hex, wmiKey, monitorDevicePath)
    std::optional<StoredMapping> FindMappingByIdentifiers(const std::unordered_set<std::string>& ids) const;
    // Update per-mapping topology for a given GDI name (tries to resolve to mapping by friendly name)
    void UpdateTopologyForGdi(const GUID guid, const std::wstring& gdiName, bool enabled);
    // Replace the stored mapping topology for the mapping that matches guid with the provided combined map
    void UpdateMappingTopologyFromCombined(const GUID guid, const std::map<std::string,bool>& combined);

    // Return combined topology merged from global topology_ and all per-mapping topology entries.
    // Merge policy: if disabledWins is true, any "false" from any source makes the result false.
    std::map<std::string,bool> GetCombinedTopology(bool disabledWins = true) const;

    // Configure default merge policy used when applying combined topology
    void SetTopologyMergePolicyDisabledWins(bool disabledWins);
    bool GetTopologyMergePolicyDisabledWins() const;

    // Topology persistence: mark whether a GDI device name is enabled (attached to desktop)
    void SetTopologyEntry(const std::wstring& gdiName, bool enabled);
    std::map<std::string,bool> GetTopologyMap() const;
    // Clear all stored mappings and topology and persist
    void Clear();

private:
    std::map<std::string, std::vector<StoredMapping>> map_; // key is UTF8 name -> list of mappings
    std::map<std::string, bool> topology_; // GDI name -> enabled
    bool mergeDisabledWins_ = true;
    std::string path_;
};

}
