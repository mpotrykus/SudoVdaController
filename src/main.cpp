#include "pch.h"
#include <iostream>
#include <codecvt>
#include <locale>
#include <thread>
#include <chrono>
#include "CliParser.h"
#include "VirtualDisplayController.h"
#include "GuidUtils.h"
#include "VirtualDisplayConfig.h"

using namespace vdc;

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    std::cout << "VirtualDisplayController starting...\n";

    // Optionally initialize COM for modules that expect it:
    EnsureComInitialized();

    // New: handle --help / -h / help before parsing and detect --stay
    bool keepAliveRequested = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h" || arg == "help") {
            std::cout << "Usage: VirtualDisplayController <verb> [args]\n";
            std::cout << "Verbs:\n";
            std::cout << "Verbs:\n";
            std::cout << "  create [name]                       Create a virtual display (optional UTF-8 name)\n";
            std::cout << "      --stay                           Keep the process alive so the created display remains present\n";
            std::cout << "  remove <guid>                       Remove a virtual display\n";
            std::cout << "  mode <guid> <w> <h> <refresh> <iso> Set mode (iso: 1=isolated, 0=not)\n";
            std::cout << "  primary <guid>                      Make display primary\n";
            std::cout << "  hdr <guid> <0|1>                    Disable/enable HDR\n";
            std::cout << "  query <guid>                        Query display state\n";
            std::cout << "Options:\n";
            std::cout << "  --help, -h                          Show this help message\n";
            std::cout << "  --stay                              For 'create', keep process alive so device stays present\n";
            std::cout << "Examples:\n";
            std::cout << "  VirtualDisplayController create \"My Display\"\n";
            std::cout << "  VirtualDisplayController create \"My Display\" --stay\n";
            std::cout << "  VirtualDisplayController remove 01234567-89ab-cdef-0123-456789abcdef\n";
            return 0;
        }
        if (arg == "--stay") {
            keepAliveRequested = true;
        }
    }

    auto cli = CliParser::Parse(argc, argv);
    VirtualDisplayController controller;

    if (cli.verb == "create") {
        VirtualDisplayConfig cfg;
        if (!cli.args.empty()) {
            // first arg -> device name (utf8)
            std::wstring device = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(cli.args[0]);
            cfg.deviceName = device;
        }
        auto res = controller.CreateDisplay(cfg);
        std::cout << res.json << std::endl;
        if (!res.success) {
            // Always print error JSON and exit with code 1
            std::cerr << res.json << std::endl;
            return 1;
        }

        // Verify creation by extracting the guid from the response and querying it.
        std::string guidStr;
        const std::string key = "\"guid\":\"";
        auto pos = res.json.find(key);
        if (pos != std::string::npos) {
            auto start = pos + key.size();
            auto end = res.json.find('"', start);
            if (end != std::string::npos && end > start) {
                guidStr = res.json.substr(start, end - start);
            }
        }
        if (guidStr.empty()) {
            std::cerr << "{\"error\":\"create returned no guid to verify\"}\n";
            return 1;
        }

        auto g = vdc::StringToGuid(guidStr);
        if (!g) {
            std::cerr << "{\"error\":\"invalid guid returned from create\"}\n";
            return 1;
        }

        // Retry querying a few times to allow the device to become available.
        const int maxRetries = 10;
        const auto delay = std::chrono::milliseconds(200);
        bool verified = false;
        for (int i = 0; i < maxRetries; ++i) {
            auto qres = controller.Query(*g);
            if (qres.success) {
                std::cout << qres.json << std::endl;
                verified = true;
                break;
            }
            std::this_thread::sleep_for(delay);
        }
        if (!verified) {
            std::cerr << "{\"error\":\"create verification failed: device not found after retries\"}\n";
            return 1;
        }

        // If user requested to keep the process alive, block here while the controller object remains alive.
        if (keepAliveRequested) {
            std::cout << "Keeping process alive to retain the created display. Press Enter to exit (or Ctrl+C).\n";
            // Keep controller in scope and block; Ctrl+C will terminate process and clean up.
            std::string dummy;
            std::getline(std::cin, dummy);
        }

        return 0;
    }

    if (cli.verb == "remove" && cli.args.size() >= 1) {
        auto g = vdc::StringToGuid(cli.args[0]);
        if (!g) { std::cout << "{\"error\":\"invalid guid\"}\n"; return 2; }
        auto res = controller.RemoveDisplay(*g);
        std::cout << res.json << std::endl;
        return res.success ? 0 : 1;
    }

    if (cli.verb == "mode" && cli.args.size() >= 5) {
        auto g = vdc::StringToGuid(cli.args[0]);
        if (!g) { std::cout << "{\"error\":\"invalid guid\"}\n"; return 2; }
        int w = std::stoi(cli.args[1]);
        int h = std::stoi(cli.args[2]);
        int refresh = std::stoi(cli.args[3]);
        bool isolated = cli.args[4] == "1";
        auto res = controller.SetMode(*g, w, h, refresh, isolated);
        std::cout << res.json << std::endl;
        return res.success ? 0 : 1;
    }

    if (cli.verb == "primary" && cli.args.size() >= 1) {
        auto g = vdc::StringToGuid(cli.args[0]);
        if (!g) { std::cout << "{\"error\":\"invalid guid\"}\n"; return 2; }
        auto res = controller.SetPrimary(*g);
        std::cout << res.json << std::endl;
        return res.success ? 0 : 1;
    }

    if (cli.verb == "hdr" && cli.args.size() >= 2) {
        auto g = vdc::StringToGuid(cli.args[0]);
        if (!g) { std::cout << "{\"error\":\"invalid guid\"}\n"; return 2; }
        bool enable = cli.args[1] == "1";
        auto res = controller.SetHdr(*g, enable);
        std::cout << res.json << std::endl;
        return res.success ? 0 : 1;
    }

    if (cli.verb == "query" && cli.args.size() >= 1) {
        auto g = vdc::StringToGuid(cli.args[0]);
        if (!g) { std::cout << "{\"error\":\"invalid guid\"}\n"; return 2; }
        auto res = controller.Query(*g);
        std::cout << res.json << std::endl;
        return res.success ? 0 : 1;
    }

    std::cout << "{\"error\":\"unknown verb\"}\n";
    std::cout << "VirtualDisplayController exiting.\n";
    return 2;
}
