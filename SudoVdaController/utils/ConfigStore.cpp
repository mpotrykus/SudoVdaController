#include "ConfigStore.h"
#include "JsonUtils.h"
#include <filesystem>
#include <fstream>
#include <codecvt>
#include <ShlObj.h>
#include "DisplayConfigUtils.h"

using namespace vdc;

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (sz <= 0) return std::string();
    std::string out; out.resize(sz);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], sz, NULL, NULL);
    return out;
}

std::optional<StoredMapping> ConfigStore::GetByNameAndMode(const std::wstring& name, int width, int height, int refresh) const {
    std::string k = ToUtf8(name);
    auto it = map_.find(k);
    if (it != map_.end()) {
        for (const auto &entry : it->second) {
            if (entry.width == width && entry.height == height && entry.refresh == refresh) return entry;
        }
    }
    for (const auto& kv : map_) {
        if (_stricmp(kv.first.c_str(), k.c_str()) == 0) {
            for (const auto &entry : kv.second) {
                if (entry.width == width && entry.height == height && entry.refresh == refresh) return entry;
            }
        }
    }
    return std::nullopt;
}
static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (sz <= 0) return std::wstring();
    std::wstring out; out.resize(sz);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], sz);
    return out;
}

ConfigStore::ConfigStore() {
    // build path in %APPDATA%\SudoVdaController\monitor_map.json
    char buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, buf))) {
        std::string dir = std::string(buf) + "\\SudoVdaController";
        std::filesystem::create_directories(dir);
        path_ = dir + "\\monitor_map.json";
    } else {
        path_ = "monitor_map.json";
    }
    Load();
}

ConfigStore::~ConfigStore() {
    Save();
}

void ConfigStore::Load() {
    map_.clear();
    topology_.clear();
    try {
        std::ifstream ifs(path_);
        if (!ifs) return;
        std::string json((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        size_t i = 0; vdc::JsonBuilder::SkipWs(json, i);
        // Expect object { "keys": { name: { guid:..., width:..., height:..., refresh:..., adapter:... } }, "topology": { "GDI": true } }
        if (i < json.size() && json[i] == '{') {
            // crude parse to find keys object
            auto keysPos = json.find("\"keys\"");
            if (keysPos != std::string::npos) {
                auto objPos = json.find('{', keysPos);
                if (objPos != std::string::npos) {
                    size_t j = objPos + 1;
                    // parse simple entries: "name":{...}
                    while (j < json.size()) {
                        vdc::JsonBuilder::SkipWs(json, j);
                        if (j >= json.size() || json[j] == '}') break;
                        if (json[j] == ',') { ++j; continue; }
                        // parse key string
                        if (json[j] == '"') {
                            ++j; size_t k = json.find('"', j); if (k == std::string::npos) break;
                            std::string name = json.substr(j, k - j);
                            j = k + 1;
                            vdc::JsonBuilder::SkipWs(json, j);
                            if (j < json.size() && json[j] == ':') ++j;
                            vdc::JsonBuilder::SkipWs(json, j);
                            // Require array form for mappings. Expect '[' and parse contained objects.
                            if (j < json.size() && json[j] == '[') {
                                size_t endArr = json.find(']', j);
                                if (endArr == std::string::npos) break;
                                size_t p = j + 1;
                                while (p < endArr) {
                                    size_t objStart = json.find('{', p);
                                    if (objStart == std::string::npos || objStart > endArr) break;
                                    size_t objEnd = json.find('}', objStart);
                                    if (objEnd == std::string::npos || objEnd > endArr) break;
                                    std::string inner = json.substr(objStart+1, objEnd - (objStart+1));
                                    StoredMapping m;
                                    m.deviceName = name;
                                    auto findVal = [&](const std::string& key)->std::string {
                                        auto p2 = inner.find('"'+key+'"');
                                        if (p2==std::string::npos) return std::string();
                                        auto colon = inner.find(':', p2);
                                        if (colon==std::string::npos) return std::string();
                                        size_t valStart = colon+1;
                                        while (valStart < inner.size() && isspace((unsigned char)inner[valStart])) ++valStart;
                                        if (valStart>=inner.size()) return std::string();
                                        if (inner[valStart] == '"') {
                                            size_t endq = inner.find('"', valStart+1);
                                            if (endq==std::string::npos) return std::string();
                                            return inner.substr(valStart+1, endq - (valStart+1));
                                        } else {
                                            size_t vend = valStart;
                                            while (vend < inner.size() && (isdigit((unsigned char)inner[vend]) || inner[vend]=='-' )) ++vend;
                                            return inner.substr(valStart, vend-valStart);
                                        }
                                    };
                                    auto g = findVal("guid"); if (!g.empty()) m.guid = g;
                                    auto w = findVal("width"); if (!w.empty()) m.width = std::stoi(w);
                                    auto h = findVal("height"); if (!h.empty()) m.height = std::stoi(h);
                                    auto r = findVal("refresh"); if (!r.empty()) m.refresh = std::stoi(r);
                                    auto a = findVal("adapterLuid"); if (!a.empty()) m.adapterLuid = std::stoull(a);
                                    map_[name].push_back(m);
                                    p = objEnd + 1;
                                }
                                j = endArr + 1;
                            } else {
                                // Skip unknown form (legacy single-object entries are no longer supported)
                                ++j;
                            }
                        } else {
                            ++j;
                        }
                    }
                }
            }
            // topology
            auto topoPos = json.find("\"topology\"");
            if (topoPos != std::string::npos) {
                auto tObj = json.find('{', topoPos);
                if (tObj != std::string::npos) {
                    size_t k = tObj + 1;
                    while (k < json.size()) {
                        vdc::JsonBuilder::SkipWs(json, k);
                        if (k >= json.size() || json[k] == '}') break;
                        if (json[k] == ',') { ++k; continue; }
                        if (json[k] == '"') {
                            ++k; size_t q = json.find('"', k); if (q == std::string::npos) break;
                            std::string gdi = json.substr(k, q - k);
                            k = q + 1; vdc::JsonBuilder::SkipWs(json, k);
                            if (k < json.size() && json[k] == ':') ++k; vdc::JsonBuilder::SkipWs(json, k);
                            // read bool value
                            bool val = false;
                            if (json.compare(k, 4, "true") == 0) { val = true; k += 4; }
                            else if (json.compare(k, 5, "false") == 0) { val = false; k += 5; }
                            topology_[gdi] = val;
                        } else { ++k; }
                    }
                }
            }
            // Migrate topology keys that are friendly names into GDI device names when possible.
            if (!topology_.empty()) {
                std::map<std::string,bool> newTopo;
                for (const auto &kv : topology_) {
                    const std::string key = kv.first;
                    bool val = kv.second;
                    // If key already looks like a GDI name (\\.\DISPLAYn or starts with \\?\), keep as-is
                    if (key.rfind("\\\\.\\", 0) == 0 || key.rfind("\\\\?\\", 0) == 0 || key.find("DISPLAY") != std::string::npos) {
                        newTopo[key] = val;
                        continue;
                    }
                    // Try to resolve friendly name -> GDI by enumerating current adapters
                    std::wstring friendly = FromUtf8(key);
                    bool resolved = false;
                    DISPLAY_DEVICEW adapter{}; adapter.cb = sizeof(adapter);
                    for (DWORD ai = 0; EnumDisplayDevicesW(NULL, ai, &adapter, 0); ++ai) {
                        std::wstring adapterName = adapter.DeviceName ? adapter.DeviceName : L"";
                        try {
                            auto fn = vdc::DisplayConfigUtils::GetMonitorFriendlyNameForGdiName(adapterName);
                            if (fn && *fn == friendly) {
                                std::string gdiUtf = ToUtf8(adapterName);
                                newTopo[gdiUtf] = val;
                                resolved = true;
                                break;
                            }
                        } catch(...) {}
                        adapter.cb = sizeof(adapter);
                    }
                    if (!resolved) {
                        // keep original key if we cannot resolve
                        newTopo[key] = val;
                    }
                }
                topology_.swap(newTopo);
            }
        }
    } catch (...) {}
}

void ConfigStore::Save() {
    try {
        std::ofstream ofs(path_, std::ios::trunc);
        if (!ofs) return;
        ofs << "{\n  \"keys\": {\n";
        bool first = true;
        for (auto& kv : map_) {
            if (!first) ofs << ",\n";
            first = false;
            ofs << "    \"" << kv.first << "\": [\n";
            bool firstEntry = true;
            for (const auto &entry : kv.second) {
                if (!firstEntry) ofs << ",\n";
                firstEntry = false;
                ofs << "      {\n";
                ofs << "        \"guid\": \"" << entry.guid << "\",\n";
                ofs << "        \"width\": " << entry.width << ",\n";
                ofs << "        \"height\": " << entry.height << ",\n";
                ofs << "        \"refresh\": " << entry.refresh << ",\n";
                ofs << "        \"adapterLuid\": " << entry.adapterLuid << "\n";
                ofs << "      }";
            }
            ofs << "\n    ]";
        }
        ofs << "\n  },\n  \"topology\": {\n";
        bool firstTopo = true;
        for (auto &kv : topology_) {
            if (!firstTopo) ofs << ",\n";
            firstTopo = false;
            ofs << "    \"" << kv.first << "\": " << (kv.second ? "true" : "false");
        }
        ofs << "\n  }\n}\n";
    } catch (...) {}
}

// Deprecated: no longer supported. Only exact name+mode matches are used.
// std::optional<StoredMapping> ConfigStore::GetByName(const std::wstring& name, std::optional<unsigned long long> adapterLuid) const { }

void ConfigStore::SetMapping(const std::wstring& name, const StoredMapping& m) {
    std::string k = ToUtf8(name);
    // Append mapping if not already present for same mode
    auto &vec = map_[k];
    bool found = false;
    for (auto &e : vec) {
        if (e.width == m.width && e.height == m.height && e.refresh == m.refresh) { found = true; e = m; break; }
    }
    if (!found) vec.push_back(m);
    Save();
}

void ConfigStore::SetTopologyEntry(const std::wstring& gdiName, bool enabled) {
    std::string k = ToUtf8(gdiName);
    topology_[k] = enabled;
    Save();
}

std::map<std::string,bool> ConfigStore::GetTopologyMap() const {
    return topology_;
}
