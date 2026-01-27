#include "../pch.h"
#include "SunshineConfig.h"
#include "../utils/StringUtils.h"
#include <cstdlib>

vdc::VirtualDisplay BuildSunshineConfig() {
    auto getenv_first = [&](std::initializer_list<const char*> names)
        -> std::optional<std::string>
        {
            for (const char* n : names) {
                const char* v = std::getenv(n);
                if (v && *v) return std::string(v);
            }
            return std::nullopt;
        };

    vdc::VirtualDisplay cfg;

    if (auto dn = getenv_first({ "SUNSHINE_APP_NAME","APOLLO_APP_NAME","APOLLO_CLIENT_NAME" })) {
        cfg.deviceName = vdc::StringToWString(*dn);
    }
    if (auto w = getenv_first({ "SUNSHINE_CLIENT_WIDTH","APOLLO_CLIENT_WIDTH" })) {
        try { cfg.width = std::stoi(*w); }
        catch (...) {}
    }
    if (auto h = getenv_first({ "SUNSHINE_CLIENT_HEIGHT","APOLLO_CLIENT_HEIGHT" })) {
        try { cfg.height = std::stoi(*h); }
        catch (...) {}
    }
    if (auto f = getenv_first({ "SUNSHINE_CLIENT_FPS","APOLLO_CLIENT_FPS" })) {
        try {
            if (f->find('.') != std::string::npos) {
                double hz = std::stod(*f);
                cfg.refreshRateMilliHz = static_cast<int>(hz * 1000.0 + 0.5);
            }
            else {
                cfg.refreshRateMilliHz = std::stoi(*f) * 1000;
            }
        }
        catch (...) {}
    }
    if (auto hdr = getenv_first({ "SUNSHINE_CLIENT_HDR","APOLLO_CLIENT_HDR" })) {
        std::string v = *hdr;
        for (auto& c : v) c = (char)tolower((unsigned char)c);
        cfg.hdr = (v == "1" || v == "true" || v == "yes");
    }

    return cfg;
}
