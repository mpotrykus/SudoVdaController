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

} // namespace vdc
