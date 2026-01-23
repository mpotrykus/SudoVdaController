#include "ConfigStore.h"
#include "JsonUtils.h"
#include <filesystem>
#include <fstream>
#include <codecvt>
#include <ShlObj.h>
#include "DisplayConfigUtils.h"
#include "GuidUtils.h"
#include <chrono>
#include <ctime>
#include <cstdlib>

static void LogStore(const std::string &msg) {
    try {
        char* tmp = nullptr; size_t len = 0;
        errno_t err = _dupenv_s(&tmp, &len, "TEMP");
        std::string dir;
        if (err == 0 && tmp != nullptr) { dir = std::string(tmp); free(tmp); }
        else dir = ".";
        std::string path = dir + "\\sudo_vda_store.log";
        std::ofstream ofs(path, std::ios::app);
        if (!ofs) return;
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char timebuf[64] = {};
        ctime_s(timebuf, sizeof(timebuf), &t);
        ofs << timebuf << ": " << msg << "\n";
    } catch(...) {}


}

static bool IsGuidString(const std::string &s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i==8 || i==13 || i==18 || i==23) { if (s[i] != '-') return false; }
        else {
            char c = s[i]; bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!ok) return false;
        }
    }
    return true;
}

static bool IsGdiLike(const std::string &k) {
    if (k.empty()) return false;
    std::string lower = k; for (auto &c : lower) c = (char)tolower((unsigned char)c);
    if (lower.rfind("\\\\.", 0) == 0) return true;
    if (lower.rfind("\\\\?\\", 0) == 0) return true;
    if (lower.find("display") != std::string::npos) return true;
    return false;
}

static bool IsEdidHex(const std::string &s) {
    if (s.size() < 16) return false;
    std::string lower = s; for (auto &c : lower) c = (char)tolower((unsigned char)c);
    return lower.rfind("00ffffffffffff00", 0) == 0;
}

std::optional<vdc::StoredMapping> vdc::ConfigStore::FindMappingByIdentifiers(const std::unordered_set<std::string>& ids) const {
    for (const auto &kv : map_) {
        for (const auto &m : kv.second) {
            // check edid
            if (!m.edid.empty()) {
                std::string e = m.edid; for (auto &c : e) c = (char)tolower((unsigned char)c);
                if (ids.find(e) != ids.end()) return m;
            }
            // check wmiKey
            if (!m.wmiKey.empty()) {
                std::string w = m.wmiKey; for (auto &c : w) c = (char)tolower((unsigned char)c);
                if (ids.find(w) != ids.end()) return m;
            }
            // check stored GUID
            if (!m.guid.empty()) {
                std::string g = m.guid; for (auto &c : g) c = (char)tolower((unsigned char)c);
                if (ids.find(g) != ids.end()) return m;
            }
            // check monitorDevicePath
            if (!m.monitorDevicePath.empty()) {
                std::string p = m.monitorDevicePath; for (auto &c : p) c = (char)tolower((unsigned char)c);
                if (ids.find(p) != ids.end()) return m;
                // also try variant without \\?\\ prefix
                const std::string prefix = "\\\\?\\";
                if (p.rfind(prefix,0) == 0) {
                    std::string p2 = p.substr(prefix.size());
                    if (ids.find(p2) != ids.end()) return m;
                }
            }
        }
    }
    return std::nullopt;
}

void vdc::ConfigStore::UpdateMappingTopologyFromCombined(const GUID guid, const std::map<std::string,bool>& combined) {
    GUID emptyGuid{};
    if (memcmp(&guid, &emptyGuid, sizeof(GUID)) != 0) {
        std::string guidStr = vdc::GuidToString(guid);
        std::string guidLower = guidStr; for (auto &c : guidLower) c = (char)tolower((unsigned char)c);
        for (auto it = map_.begin(); it != map_.end(); ++it) {
            auto &kv = *it;
            for (auto mit = kv.second.begin(); mit != kv.second.end(); ++mit) {
                auto &m = *mit;
                if (!m.guid.empty()) {
                    std::string mg = m.guid; for (auto &c : mg) c = (char)tolower((unsigned char)c);
                    if (mg == guidLower) {
                        // normalize combined keys to lowercase and replace mapping topology
                        std::map<std::string,bool> norm;
                        for (const auto &tk : combined) {
                            std::string k = tk.first; for (auto &c : k) c = (char)tolower((unsigned char)c);
                            norm[k] = tk.second;
                        }
                        m.topology.swap(norm);
                        Save();
                        return;
                    }
                }
            }
        }
    }
}

using namespace vdc;

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (sz <= 0) return std::string();
    std::string out; out.resize(sz);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], sz, NULL, NULL);
    return out;
}

void ConfigStore::Clear() {
    map_.clear();
    topology_.clear();
    Save();
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
                                    auto mdp = findVal("monitorDevicePath"); if (!mdp.empty()) m.monitorDevicePath = mdp;
                                    auto ed = findVal("edid"); if (!ed.empty()) m.edid = ed;
                                    auto wk = findVal("wmiKey"); if (!wk.empty()) m.wmiKey = wk;
                                    // parse optional topology object inside this mapping
                                    auto topoPos = inner.find("\"topology\"");
                                    if (topoPos != std::string::npos) {
                                        auto bpos = inner.find('{', topoPos);
                                        if (bpos != std::string::npos) {
                                            int depth = 0; size_t t = bpos;
                                            for (; t < inner.size(); ++t) {
                                                if (inner[t] == '{') ++depth;
                                                else if (inner[t] == '}') { --depth; if (depth == 0) break; }
                                            }
                                            if (t < inner.size()) {
                                                size_t start = bpos+1; size_t end = t;
                                                size_t kk = start;
                                                while (kk < end) {
                                                    // parse "key": (true|false)
                                                    while (kk < end && isspace((unsigned char)inner[kk])) ++kk;
                                                    if (kk >= end) break;
                                                    if (inner[kk] == ',') { ++kk; continue; }
                                                    if (inner[kk] == '"') {
                                                        ++kk; size_t q = inner.find('"', kk); if (q == std::string::npos || q >= end) break;
                                                        std::string tk = inner.substr(kk, q - kk);
                                                        kk = q+1;
                                                        // find ':'
                                                        auto colon = inner.find(':', kk); if (colon == std::string::npos || colon >= end) break;
                                                        kk = colon+1; while (kk < end && isspace((unsigned char)inner[kk])) ++kk;
                                                        bool val = false;
                                                        if (inner.compare(kk,4,"true") == 0) { val = true; kk += 4; }
                                                        else if (inner.compare(kk,5,"false") == 0) { val = false; kk += 5; }
                                                        std::string tkutf = tk; for (auto &c: tkutf) c = (char)tolower((unsigned char)c);
                                                        m.topology[tkutf] = val;
                                                    } else break;
                                                }
                                            }
                                        }
                                    }
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
                            // log entry
                            try { LogStore(std::string("Loaded topology entry: ") + gdi + (val?":true":":false")); } catch(...) {}
                        } else { ++k; }
                    }
                }
            }
            // Normalize topology keys to lowercase UTF-8 for consistent matching
            if (!topology_.empty()) {
                std::map<std::string,bool> norm;
                for (const auto &kv : topology_) {
                    std::string k = kv.first;
                    for (auto &c : k) c = (char)tolower((unsigned char)c);
                    norm[k] = kv.second;
                }
                topology_.swap(norm);
            }

            // Debug: log resolved identifiers for each topology GDI key (monitorDevicePath, EDID, WMI, friendly)
            // Also log current DisplayConfig paths so we can compare what the system currently exposes
            try {
                UINT32 pCount = 0, mCount = 0;
                if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pCount, &mCount) == ERROR_SUCCESS && pCount > 0) {
                    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pCount);
                    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mCount);
                    if (QueryDisplayConfig(QDC_ALL_PATHS, &pCount, paths.data(), &mCount, modes.data(), nullptr) == ERROR_SUCCESS) {
                        for (UINT32 i = 0; i < pCount; ++i) {
                            std::string srcUtf, mdpUtf, friendlyUtf;
                            DISPLAYCONFIG_SOURCE_DEVICE_NAME src{}; src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME; src.header.size = sizeof(src);
                            src.header.adapterId = paths[i].sourceInfo.adapterId; src.header.id = paths[i].sourceInfo.id;
                            if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS) { try { srcUtf = ToUtf8(std::wstring(src.viewGdiDeviceName)); } catch(...) { srcUtf = ""; } }
                            DISPLAYCONFIG_TARGET_DEVICE_NAME tname{}; tname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME; tname.header.size = sizeof(tname);
                            tname.header.adapterId = paths[i].targetInfo.adapterId; tname.header.id = paths[i].targetInfo.id;
                            if (DisplayConfigGetDeviceInfo(&tname.header) == ERROR_SUCCESS) {
                                try { if (tname.monitorDevicePath && tname.monitorDevicePath[0]) mdpUtf = ToUtf8(std::wstring(tname.monitorDevicePath)); } catch(...) { mdpUtf = ""; }
                                try { if (tname.monitorFriendlyDeviceName && tname.monitorFriendlyDeviceName[0]) friendlyUtf = ToUtf8(std::wstring(tname.monitorFriendlyDeviceName)); } catch(...) { friendlyUtf = ""; }
                            }
                            try { LogStore(std::string("DisplayConfig path: src='") + srcUtf + "' mdp='" + mdpUtf + "' friendly='" + friendlyUtf + "'"); } catch(...) {}
                        }
                    }
                }
            } catch(...) {}
            // helper: find monitorDevicePath by comparing lowercase GDI names from DisplayConfig
            auto findMdpForLowerGdi = [&](const std::string &lowerGdi)->std::optional<std::string> {
                const UINT32 queries[2] = { QDC_ONLY_ACTIVE_PATHS, QDC_ALL_PATHS };
                for (UINT qi = 0; qi < _countof(queries); ++qi) {
                    UINT32 qflag = queries[qi];
                    UINT32 pathCount = 0, modeCount = 0;
                    if (GetDisplayConfigBufferSizes(qflag, &pathCount, &modeCount) != ERROR_SUCCESS) continue;
                    if (pathCount == 0) continue;
                    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
                    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
                    if (QueryDisplayConfig(qflag, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) continue;
                    for (UINT32 i = 0; i < pathCount; ++i) {
                        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
                        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                        src.header.size = sizeof(src);
                        src.header.adapterId = paths[i].sourceInfo.adapterId;
                        src.header.id = paths[i].sourceInfo.id;
                        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
                        std::string srcUtf = ToUtf8(std::wstring(src.viewGdiDeviceName));
                        for (auto &c : srcUtf) c = (char)tolower((unsigned char)c);
                        if (srcUtf != lowerGdi) continue;
                        DISPLAYCONFIG_TARGET_DEVICE_NAME tname{};
                        tname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                        tname.header.size = sizeof(tname);
                        tname.header.adapterId = paths[i].targetInfo.adapterId;
                        tname.header.id = paths[i].targetInfo.id;
                        if (DisplayConfigGetDeviceInfo(&tname.header) == ERROR_SUCCESS) {
                            if (tname.monitorDevicePath && tname.monitorDevicePath[0]) return ToUtf8(std::wstring(tname.monitorDevicePath));
                        }
                    }
                }
                return std::nullopt;
            };

            for (const auto &kv : topology_) {
                const std::string key = kv.first;
                try {
                    std::string lower = key;
                    for (auto &c : lower) c = (char)tolower((unsigned char)c);
                    std::string mdpStr, edStr, wkStr, friendlyStr;
                    auto mdp = findMdpForLowerGdi(lower);
                    if (mdp && !mdp->empty()) {
                        mdpStr = *mdp;
                        try {
                            std::wstring inst = FromUtf8(mdpStr);
                            const std::wstring prefix = L"\\\\?\\";
                            if (inst.rfind(prefix,0) == 0) inst = inst.substr(prefix.size());
                            auto ed = vdc::DisplayConfigUtils::GetEdidHexForDeviceInstanceId(inst);
                            if (ed) edStr = *ed;
                            auto wk = vdc::DisplayConfigUtils::GetWmiKeyForDeviceInstanceId(inst);
                            if (wk) wkStr = *wk;
                        } catch(...) {}
                    } else {
                        // try friendly name lookup (may still fail)
                        try {
                            std::wstring gdi = FromUtf8(key);
                            auto fn = vdc::DisplayConfigUtils::GetMonitorFriendlyNameForGdiName(gdi);
                            if (fn) friendlyStr = ToUtf8(*fn);
                        } catch(...) {}
                    }
                    std::string log = "Topology debug: key='" + key + "' mdp='" + mdpStr + "' edid='" + edStr + "' wmi='" + wkStr + "' friendly='" + friendlyStr + "'";
                    try { LogStore(log); } catch(...) {}
                } catch(...) {}
            }
            // Force-migrate GDI-style topology keys to stable identifiers and persist if changed.
            bool persistedChanges = false;
            if (!topology_.empty()) {
                std::map<std::string,bool> migrated;
                for (const auto &kv : topology_) {
                    std::string key = kv.first;
                    bool val = kv.second;
                    std::string lower = key; for (auto &c : lower) c = (char)tolower((unsigned char)c);
                    // resolve via DisplayConfig
                    auto mdp = findMdpForLowerGdi(lower);
                    std::string resolved = key;
                    if (mdp && !mdp->empty()) {
                        try {
                            std::wstring inst = FromUtf8(*mdp);
                            const std::wstring prefix = L"\\\\?\\";
                            if (inst.rfind(prefix,0) == 0) inst = inst.substr(prefix.size());
                            auto ed = vdc::DisplayConfigUtils::GetEdidHexForDeviceInstanceId(inst);
                            if (ed && !ed->empty()) {
                                resolved = *ed;
                            } else {
                                auto wk = vdc::DisplayConfigUtils::GetWmiKeyForDeviceInstanceId(inst);
                                if (wk && !wk->empty()) resolved = *wk;
                                else {
                                    // as a fallback, try to find an existing mapping that references this monitorDevicePath
                                    std::unordered_set<std::string> ids;
                                    std::string p = *mdp; for (auto &c : p) c = (char)tolower((unsigned char)c);
                                    ids.insert(p);
                                    const std::string pref = "\\\\?\\";
                                    if (p.rfind(pref,0) == 0) ids.insert(p.substr(pref.size()));
                                    auto by = FindMappingByIdentifiers(ids);
                                    if (by) resolved = by->guid;
                                }
                            }
                        } catch(...) {}
                    }
                    // Only replace keys when we resolved a concrete identifier (EDID or WMI)
                    if (resolved != key) {
                        for (auto &c : resolved) c = (char)tolower((unsigned char)c);
                        migrated[resolved] = val;
                        persistedChanges = true;
                        try { LogStore(std::string("Persist migrate topology: \"") + key + "\" -> \"" + resolved + "\""); } catch(...) {}
                    } else {
                        migrated[key] = val;
                    }
                }
                topology_.swap(migrated);
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

            // Migrate GDI-style topology keys (

            // Also migrate per-mapping topology entries
            for (auto &kv : map_) {
                for (auto &m : kv.second) {
                    if (m.topology.empty()) continue;
                    std::map<std::string,bool> newTopo;
                    for (const auto &tk : m.topology) {
                        const std::string tkey = tk.first;
                        bool tval = tk.second;
                        bool isGdi = (tkey.rfind("\\\\.", 0) == 0) || (tkey.rfind("\\\\?\\", 0) == 0) || (tkey.find("display") != std::string::npos);
                        if (!isGdi) { newTopo[tkey] = tval; continue; }
                        try {
                            std::wstring gdi = FromUtf8(tkey);
                            auto mdp = vdc::DisplayConfigUtils::GetMonitorDevicePathForGdiName(gdi);
                            std::string resolved = tkey;
                            if (mdp && !mdp->empty()) {
                                try {
                                    std::wstring inst = FromUtf8(*mdp);
                                    const std::wstring prefix = L"\\\\?\\";
                                    if (inst.rfind(prefix,0) == 0) inst = inst.substr(prefix.size());
                                    auto ed = vdc::DisplayConfigUtils::GetEdidHexForDeviceInstanceId(inst);
                                    if (ed && !ed->empty()) {
                                        resolved = *ed;
                                    } else {
                                        auto wk = vdc::DisplayConfigUtils::GetWmiKeyForDeviceInstanceId(inst);
                                        if (wk && !wk->empty()) resolved = *wk;
                                    }
                                } catch(...) {}
                            }
                            if (resolved != tkey) {
                                for (auto &c : resolved) c = (char)tolower((unsigned char)c);
                                newTopo[resolved] = tval;
                                try { LogStore(std::string("Mapping '") + kv.first + "': topology key '" + tkey + "' -> '" + resolved + "'"); } catch(...) {}
                            } else {
                                newTopo[tkey] = tval;
                            }
                        } catch(...) {
                            newTopo[tkey] = tval;
                        }
                    }
                    // detect changes
                    if (m.topology != newTopo) {
                        m.topology.swap(newTopo);
                        persistedChanges = true;
                    }
                }
            }
            if (persistedChanges) {
                try { Save(); LogStore(std::string("ConfigStore: persisted migrated topology/ids to ") + path_); } catch(...) {}
            }
            // (no automatic addition of GUID keys on load — mappings will add GUID topology when created)
            // Read optional merge policy flag from file
            auto mpPos = json.find("\"mergeDisabledWins\"");
            if (mpPos != std::string::npos) {
                auto colon = json.find(':', mpPos);
                if (colon != std::string::npos) {
                    size_t kk = colon + 1; vdc::JsonBuilder::SkipWs(json, kk);
                    if (json.compare(kk, 4, "true") == 0) mergeDisabledWins_ = true;
                    else if (json.compare(kk, 5, "false") == 0) mergeDisabledWins_ = false;
                }
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
                if (!entry.monitorDevicePath.empty()) ofs << "        ,\"monitorDevicePath\": \"" << entry.monitorDevicePath << "\"\n";
                if (!entry.edid.empty()) ofs << "        ,\"edid\": \"" << entry.edid << "\"\n";
                if (!entry.wmiKey.empty()) ofs << "        ,\"wmiKey\": \"" << entry.wmiKey << "\"\n";
                // emit optional topology object
                if (!entry.topology.empty()) {
                    ofs << "        ,\"topology\": {\n";
                    bool ft = true;
                    for (const auto &tk : entry.topology) {
                        if (!ft) ofs << ",\n";
                        ft = false;
                        ofs << "          \"" << tk.first << "\": " << (tk.second ? "true" : "false");
                    }
                    ofs << "\n        }\n";
                }
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
        // merge policy
        ofs << "\n  },\n  \"mergeDisabledWins\": " << (mergeDisabledWins_ ? "true" : "false") << "\n}\n";
    } catch (...) {}
}

std::map<std::string,bool> ConfigStore::GetCombinedTopology(bool disabledWins) const {
    std::map<std::string,bool> out = topology_; // start with global
    // overlay per-mapping topology
    for (const auto &kv : map_) {
        for (const auto &m : kv.second) {
            for (const auto &tk : m.topology) {
                auto k = tk.first;
                std::string lk = k; for (auto &c : lk) c = (char)tolower((unsigned char)c);
                bool v = tk.second;
                auto it = out.find(lk);
                if (it == out.end()) out[lk] = v;
                else {
                    if (disabledWins) {
                        if (!v) it->second = false;
                    } else {
                        // enabled-wins: if any true, mark true
                        if (v) it->second = true;
                    }
                }
            }
        }
    }
    return out;
}

void ConfigStore::SetTopologyMergePolicyDisabledWins(bool disabledWins) {
    mergeDisabledWins_ = disabledWins;
    Save();
}

bool ConfigStore::GetTopologyMergePolicyDisabledWins() const { return mergeDisabledWins_; }

void ConfigStore::SetMapping(const std::wstring& name, const StoredMapping& m) {
    std::string k = ToUtf8(name);
    // Append mapping if not already present for same mode
    auto &vec = map_[k];
    bool found = false;
    for (auto &e : vec) {
        if (e.width == m.width && e.height == m.height && e.refresh == m.refresh) {
            // merge fields but preserve any existing per-mapping topology unless the new mapping provides entries
            found = true;
            e.guid = m.guid;
            e.width = m.width;
            e.height = m.height;
            e.refresh = m.refresh;
            e.adapterLuid = m.adapterLuid;
            e.deviceName = m.deviceName;
            // merge topology: incoming keys override existing ones; preserve others
            for (const auto &tk : m.topology) e.topology[tk.first] = tk.second;
            break;
        }
    }
    if (!found) vec.push_back(m);
    Save();
}

void ConfigStore::UpdateTopologyForGdi(const GUID guid, const std::wstring& gdiName, bool enabled) {
    // Try to find mapping corresponding to this gdiName and update its stored topology
    // Try to find mapping corresponding to this gdiName and update its stored topology
    std::string gdiUtf = ToUtf8(gdiName);
    std::string gdiLower = gdiUtf; 
    for (auto &c : gdiLower) c = (char)tolower((unsigned char)c);
    // If a GUID was provided, prefer updating the mapping that contains that GUID
    GUID emptyGuid{};
    if (memcmp(&guid, &emptyGuid, sizeof(GUID)) != 0) {
        std::string guidStr = vdc::GuidToString(guid);
        std::string guidLower = guidStr; for (auto &c : guidLower) c = (char)tolower((unsigned char)c);
        for (auto &kv : map_) {
            for (auto &m : kv.second) {
                if (!m.guid.empty()) {
                    std::string mg = m.guid; for (auto &c : mg) c = (char)tolower((unsigned char)c);
                    if (mg == guidLower) {
                        // normalize existing topology keys to lowercase and merge
                        std::map<std::string,bool> norm;
                        for (const auto &tk : m.topology) {
                            std::string k = tk.first; for (auto &c : k) c = (char)tolower((unsigned char)c);
                            norm[k] = tk.second;
                        }
                        norm[gdiLower] = enabled;
                        m.topology.swap(norm);
                        Save();
                        return;
                    }
                }
            }
        }
    }

    // Fallback: try to match by friendly name key (case-insensitive)
    //for (auto &kv : map_) {
    //    for (auto &m : kv.second) {
    //        if (_stricmp(kv.first.c_str(), m.deviceName.c_str()) == 0) {
    //            // normalize existing topology keys to lowercase and merge
    //            std::map<std::string,bool> norm;
    //            for (const auto &tk : m.topology) {
    //                std::string k = tk.first; for (auto &c : k) c = (char)tolower((unsigned char)c);
    //                norm[k] = tk.second;
    //            }
    //            norm[gdiLower] = enabled;
    //            m.topology.swap(norm);
    //            Save();
    //            return;
    //        }
    //    }
    //}

    // If no mapping found, write to global topology
    SetTopologyEntry(gdiName, enabled);
}
    // Fallback: try to match by friendly name key (case-insensitive)

void ConfigStore::SetTopologyEntry(const std::wstring& gdiName, bool enabled) {
    // Normalize GDI key to UTF8 lowercase for stable storage
    std::string key = ToUtf8(gdiName);
    for (auto &c : key) c = (char)tolower((unsigned char)c);
    try { LogStore(std::string("SetTopologyEntry: ") + key + (enabled?":true":":false")); } catch(...) {}
    topology_[key] = enabled;
    // Also persist the monitor friendly name (if available) as a secondary key (lowercase)
    // Do not store friendly-name aliases here; Load() will migrate legacy friendly-name keys to GDI names.
    Save();
    try { LogStore(std::string("ConfigStore::Save() called from SetTopologyEntry -> ") + path_); } catch(...) {}
}

std::map<std::string,bool> ConfigStore::GetTopologyMap() const {
    return topology_;
}
