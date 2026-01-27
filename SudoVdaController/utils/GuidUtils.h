#pragma once
#include <string>
#include <optional>
#include <windows.h>

namespace vdc {

    inline GUID GenerateGuid() {
        GUID g;
        CoCreateGuid(&g);
        return g;
    }

    // Comparator for GUID to allow use in ordered containers
    struct GuidLess {
        bool operator()(const GUID& a, const GUID& b) const noexcept {
            if (a.Data1 != b.Data1) return a.Data1 < b.Data1;
            if (a.Data2 != b.Data2) return a.Data2 < b.Data2;
            if (a.Data3 != b.Data3) return a.Data3 < b.Data3;
            for (int i = 0; i < 8; ++i) {
                if (a.Data4[i] != b.Data4[i]) return a.Data4[i] < b.Data4[i];
            }
            return false;
        }
    };

    inline std::string GuidToString(const GUID& g) {
        char buf[64]{};
        snprintf(buf, sizeof(buf),
            "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            g.Data1, g.Data2, g.Data3,
            g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
            g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
        return std::string(buf);
    }

    inline std::optional<GUID> StringToGuid(const std::string& s) {
        // minimal parser, not fully robust
        GUID g{};
        if (sscanf_s(s.c_str(),
            "%8lX-%4hX-%4hX-%2hhX%2hhX-%2hhX%2hhX%2hhX%2hhX%2hhX%2hhX",
            &g.Data1, &g.Data2, &g.Data3,
            &g.Data4[0], &g.Data4[1],
            &g.Data4[2], &g.Data4[3], &g.Data4[4], &g.Data4[5], &g.Data4[6], &g.Data4[7]) >= 3) {
            return g;
        }
        return std::nullopt;
    }

    inline bool IsGuid(const std::string value) {
        std::string s = value;
        // trim
        auto trim = [](std::string& str) {
            size_t a = 0;
            while (a < str.size() && std::isspace(static_cast<unsigned char>(str[a]))) ++a;
            size_t b = str.size();
            while (b > a && std::isspace(static_cast<unsigned char>(str[b - 1]))) --b;
            str = str.substr(a, b - a);
            };
        trim(s);

        // optional surrounding { } or ( )
        if (s.size() >= 2 && ((s.front() == '{' && s.back() == '}') || (s.front() == '(' && s.back() == ')')))
            s = s.substr(1, s.size() - 2);

        // optional urn:uuid: prefix (case-insensitive)
        const std::string urn = "urn:uuid:";
        if (s.size() > urn.size()) {
            bool match = true;
            for (size_t i = 0; i < urn.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(s[i])) != urn[i]) { match = false; break; }
            }
            if (match) s = s.substr(urn.size());
        }

        if (s.size() != 36) return false;
        for (size_t i = 0; i < s.size(); ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (s[i] != '-') return false;
            }
            else {
                unsigned char c = static_cast<unsigned char>(s[i]);
                bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                if (!ok) return false;
            }
        }
        return true;
    }

} // namespace vdc

// Provide global operator< for GUID so it can be used as a key in ordered
// containers (std::map, std::set) without requiring an explicit comparator.
inline bool operator<(const GUID& a, const GUID& b) noexcept {
    if (a.Data1 != b.Data1) return a.Data1 < b.Data1;
    if (a.Data2 != b.Data2) return a.Data2 < b.Data2;
    if (a.Data3 != b.Data3) return a.Data3 < b.Data3;
    for (int i = 0; i < 8; ++i) {
        if (a.Data4[i] != b.Data4[i]) return a.Data4[i] < b.Data4[i];
    }
    return false;
}
