#include "../pch.h"
#include "VerbHandlers.h"
#include "SunshineConfig.h"
#include "../client/TrayClient.h"
#include "../controller/VirtualDisplayController.h"
#include "../utils/StringUtils.h"
#include "../utils/GuidUtils.h"
#include "../utils/Logger.h"

#include <iostream>
#include <map>
#include <codecvt>
#include <locale>

int HandleVerb(const vdc::CliArgs& cli) {

    if (cli.verb == "create") {
        vdc::VirtualDisplay cfg;
        std::optional<GUID> guidOpt;

        // parse flags
        for (size_t i = 0; i < cli.args.size(); ++i) {
            std::string a = cli.args[i];

            if (a.rfind("--", 0) == 0) {
                std::string key = a;
                std::string val;

                auto eq = key.find('=');
                if (eq != std::string::npos) {
                    val = key.substr(eq + 1);
                    key = key.substr(0, eq);
                }
                else if (i + 1 < cli.args.size() && cli.args[i + 1].rfind("--", 0) != 0) {
                    val = cli.args[++i];
                }

                if (key == "--width" || key == "--w") {
                    try { cfg.width = std::stoi(val); }
                    catch (...) {}
                }
                else if (key == "--height" || key == "--h") {
                    try { cfg.height = std::stoi(val); }
                    catch (...) {}
                }
                else if (key == "--refresh" || key == "--r") {
                    try {
                        if (val.find('.') != std::string::npos) {
                            double hz = std::stod(val);
                            cfg.refreshRateMilliHz = static_cast<int>(hz * 1000.0 + 0.5);
                        }
                        else {
                            long long v = std::stoll(val);
                            if (v < 20) v = 20;
                            cfg.refreshRateMilliHz = (v < 1000) ? int(v * 1000) : int(v);
                        }
                    }
                    catch (...) {}
                }
                else if (key == "--hdr") {
                    cfg.hdr = true;
                }
                else if (key == "--primary") {
                    cfg.primary = true;
                }
                else if (key == "--no-hdr") {
                    cfg.hdr = false;
                }
                else if (key == "--adapter") {
                    try {
                        if (val.rfind("0x", 0) == 0)
                            cfg.adapterLuid = std::stoull(val, nullptr, 16);
                        else
                            cfg.adapterLuid = std::stoull(val);
                    }
                    catch (...) {}
                }
                else if (key == "--guid") {
                    auto g = vdc::StringToGuid(val);
                    if (g) guidOpt = *g;
                }
            }
            else {
                cfg.deviceName = vdc::StringToWString(a);
            }
        }

        std::map<std::string, std::string> kv;
        kv["deviceName"] = vdc::PercentEncodeUtf8(cfg.deviceName);
        kv["width"] = std::to_string(cfg.width);
        kv["height"] = std::to_string(cfg.height);
        kv["refresh"] = std::to_string(cfg.refreshRateMilliHz);
        kv["hdr"] = cfg.hdr ? "1" : "0";
        kv["primary"] = cfg.primary ? "1" : "0";
        if (cfg.adapterLuid) kv["adapter"] = std::to_string(cfg.adapterLuid);
        if (guidOpt) kv["guid"] = vdc::GuidToString(*guidOpt);

        auto res = SendVerb("create", kv);
        std::cout << res.second << std::endl;
        return 0;
    }

    if (cli.verb == "remove" && cli.args.size() >= 1) {
        std::map<std::string, std::string> kv{ {"guid", cli.args[0]} };

        auto res = SendVerb("remove", kv);
        std::cout << res.second << std::endl;

        if (!res.first) {
            auto g = vdc::StringToGuid(cli.args[0]);
            if (!g) { std::cout << "{\"error\":\"invalid guid\"}\n"; return 2; }
            vdc::VirtualDisplayController local;
            return local.RemoveDisplay(*g) ? 0 : 1;
        }

        return res.first ? 0 : 1;
    }

    if (cli.verb == "mode" && cli.args.size() >= 5) {
        std::map<std::string, std::string> kv{
            {"guid", cli.args[0]},
            {"w", cli.args[1]},
            {"h", cli.args[2]},
            {"refresh", cli.args[3]},
            {"iso", cli.args[4]}
        };

        auto res = SendVerb("mode", kv);
        std::cout << res.second << std::endl;
        return res.first ? 0 : 1;
    }

    if (cli.verb == "primary" && cli.args.size() >= 1) {
        std::map<std::string, std::string> kv{ {"guid", cli.args[0]} };
        auto res = SendVerb("primary", kv);
        std::cout << res.second << std::endl;
        return res.first ? 0 : 1;
    }

    if (cli.verb == "hdr" && cli.args.size() >= 2) {
        std::map<std::string, std::string> kv{
            {"guid", cli.args[0]},
            {"enable", cli.args[1]}
        };

        auto res = SendVerb("hdr", kv);
        std::cout << res.second << std::endl;
        return res.first ? 0 : 1;
    }

    if (cli.verb == "query" && cli.args.size() >= 1) {
        std::map<std::string, std::string> kv{ {"guid", cli.args[0]} };
        auto res = SendVerb("query", kv);
        std::cout << res.second << std::endl;
        return res.first ? 0 : 1;
    }

    if (cli.verb == "kill") {
        auto res = SendVerb("exit", {});
        std::cout << res.second << std::endl;
        return res.first ? 0 : 1;
    }

    if (cli.verb == "sunshine") {
        vdc::VirtualDisplay cfg = BuildSunshineConfig();

        std::map<std::string, std::string> kv;
        kv["deviceName"] = vdc::PercentEncodeUtf8(cfg.deviceName);
        kv["width"] = std::to_string(cfg.width);
        kv["height"] = std::to_string(cfg.height);
        kv["refresh"] = std::to_string(cfg.refreshRateMilliHz);
        kv["hdr"] = cfg.hdr ? "1" : "0";
        kv["primary"] = "1";

        auto res = SendVerb("create", kv);
        std::cout << res.second << std::endl;
        return 0;
    }

    LOG_ERROR("Unknown verb");
    return 2;
}

void PrintHelp() {
    std::cout << "Usage: SudoVdaController <verb> [args]\n\n";

    std::cout << "Verbs:\n";
    std::cout << "  create [name]                        Create a virtual display (optional UTF-8 name)\n";
    std::cout << "      Flags for create:\n";
    std::cout << "        --width  N, --w N              Width in pixels (default 1920)\n";
    std::cout << "        --height N, --h N              Height in pixels (default 1080)\n";
    std::cout << "        --refresh N or --refresh N.N   Refresh rate; integer=milliHz, float=Hz\n";
    std::cout << "        --hdr                          Enable HDR\n";
    std::cout << "        --primary                      Make display primary\n";
    std::cout << "        --adapter <id>                 Adapter LUID (decimal or 0xhex)\n";
    std::cout << "  remove <guid>                        Remove a virtual display\n";
    std::cout << "  mode <guid> <w> <h> <refresh> <iso>  Set mode (iso: 1=isolated, 0=not)\n";
    std::cout << "  primary <guid>                       Make display primary\n";
    std::cout << "  hdr <guid> <0|1>                     Disable/enable HDR\n";
    std::cout << "  query <guid>                         Query display state\n";
    std::cout << "  sunshine                             Auto-capture Sunshine/Apollo env vars\n\n";

    std::cout << "Options:\n";
    std::cout << "  --help, -h                           Show this help message\n\n";

    std::cout << "Examples:\n";
    std::cout << "  SudoVdaController create \"My Display\" --width=2560 --height=1440 --refresh=119.98\n";
    std::cout << "  SudoVdaController create --width 2560 --height 1440 --refresh 119980 --stay\n";
    std::cout << "  SudoVdaController remove 01234567-89ab-cdef-0123-456789abcdef\n\n";
}
