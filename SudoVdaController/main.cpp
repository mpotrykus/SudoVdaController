#include "pch.h"

#include "controller/VirtualDisplayController.h"
#include "controller/TrayServer.h"
#include "utils/CliParser.h"
#include "utils/GuidUtils.h"
#include "models/VirtualDisplayConfig.h"

#include <iostream>
#include <codecvt>
#include <locale>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstdlib>

using namespace vdc;

static const std::wstring PIPE_NAME = L"SudoVdaTrayPipe";

// percent-encode utf8
static std::string PercentEncodeUtf8(const std::wstring& w) {
    std::string u = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(w);
    std::ostringstream oss;
    for (unsigned char c : u) {
        // safe characters: alnum and -_.~
        if ( (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
             c == '-' || c == '_' || c == '.' || c == '~') {
            oss << c;
        } else if (c == ' ') {
            oss << '+';
        } else {
            oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << int(c) << std::dec;
        }
    }
    return oss.str();
}

static bool StartTrayProcessIfNeeded() {
    // Attempt to connect briefly; if fails, create process with --tray
    std::wstring pipePath = L"\\\\.\\pipe\\" + PIPE_NAME;
    if (WaitNamedPipeW(pipePath.c_str(), 50)) return true;

    // not ready -> start tray process
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exePath, ARRAYSIZE(exePath))) {
        std::cout << "GetModuleFileNameW failed.\n";
        return false;
    }

    std::wstring cmd = std::wstring(L"\"") + exePath + L"\" --tray";
    std::cout << "Starting tray process: " << std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(cmd) << "\n";

    PROCESS_INFORMATION pi{};
    STARTUPINFOW si{}; 
    si.cb = sizeof(si);
    BOOL ok = CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (!ok) {
        std::cout << "CreateProcessW failed: " << GetLastError() << "\n";
        return false;
    }
    std::cout << "Started tray process, pid=" << pi.dwProcessId << "\n";

    // Poll by trying to open the pipe. Print GetLastError for diagnostics.
    const int maxAttempts = 40;
    const std::chrono::milliseconds delay(250);
    for (int i = 0; i < maxAttempts; ++i) {
        HANDLE h = CreateFileW(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return true;
        }
        DWORD err = GetLastError();
        // std::cout << "CreateFileW(pipe) failed attempt " << (i + 1) << ", GetLastError=" << err << "\n";
        if (err == ERROR_ACCESS_DENIED) {
            std::cout << "Access denied when attempting to open pipe. This likely indicates an ACL/integrity-level issue.\n";
            return false;
        }
        std::this_thread::sleep_for(delay);
    }

    std::cout << "Tray process pipe did not become available (timeout).\n";
    return false;
}

static bool SendToTray(const std::string& message, std::string& outResponse) {
    std::wstring fullPipe = L"\\\\.\\pipe\\" + PIPE_NAME;
    // Ensure tray is running (start if needed) and wait until the pipe becomes available.
    if (!StartTrayProcessIfNeeded()) {
		std::cout << "Failed to start or connect to tray process.\n";
        return false;
    }

    HANDLE h = INVALID_HANDLE_VALUE;
    const int maxAttempts = 40;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        h = CreateFileW(fullPipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            break;
        }
        // brief backoff; allow the tray process to create the pipe
        Sleep(1000);
    }
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(h, message.c_str(), (DWORD)message.size(), &written, NULL);
    if (!ok || written != message.size()) { CloseHandle(h); return false; }

    // read response
    char buf[8192];
    DWORD read = 0;
    ok = ReadFile(h, buf, (DWORD)sizeof(buf)-1, &read, NULL);
    if (ok && read > 0) {
        buf[read] = 0;
        outResponse = std::string(buf);
    } else {
        outResponse.clear();
    }

    CloseHandle(h);
    return true;
}

int main(int argc, char** argv) {
    
    (void)argc; (void)argv;
    std::cout << "SudoVdaController starting...\n";

    // Optionally initialize COM for modules that expect it:
    EnsureComInitialized();
    // Default to tray mode when launched with no verbs/arguments (double-click)
    if (argc <= 1) {
        return vdc::RunTrayServer(PIPE_NAME);
    }

    // If launched with explicit --tray flag, run server loop
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "--tray") {
            // run server (blocks)
            return vdc::RunTrayServer(PIPE_NAME);
        }
    }

    // New: handle --help / -h / help before parsing and detect --stay
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h" || arg == "help") {
            std::cout << "Usage: SudoVdaController <verb> [args]\n";
            std::cout << "Verbs:\n";
            std::cout << "  create [name]                        Create a virtual display (optional UTF-8 name)\n";
            std::cout << "      Flags for create:\n";
            std::cout << "        --width  N, --w N              Width in pixels (default 1920)\n";
            std::cout << "        --height N, --h N              Height in pixels (default 1080)\n";
            std::cout << "        --refresh N or --refresh N.N   Refresh rate; integer is milliHz, float is Hz (e.g. 119.98)\n";
            std::cout << "        --hdr                          Enable HDR\n";
            std::cout << "        --primary                      Make display primary\n";
            std::cout << "        --adapter <id>                 Adapter LUID as decimal or 0xhex\n";
            std::cout << "        --guid <guid>                  Request a specific GUID for the device\n";
            std::cout << "  remove <guid>                        Remove a virtual display\n";
            std::cout << "  mode <guid> <w> <h> <refresh> <iso>  Set mode (iso: 1=isolated, 0=not)\n";
            std::cout << "  primary <guid>                       Make display primary\n";
            std::cout << "  hdr <guid> <0|1>                     Disable/enable HDR\n";
            std::cout << "  query <guid>                         Query display state\n";
            std::cout << "  sunshine                             Will automatically capture and assign Sunshine or Apollo environment variables\n";
            std::cout << "Options:\n";
            std::cout << "  --help, -h                           Show this help message\n";
            std::cout << "Examples:\n";
            std::cout << "  SudoVdaController create \"My Display\" --width=2560 --height=1440 --refresh=119.98\n";
            std::cout << "  SudoVdaController create --width 2560 --height 1440 --refresh 119980 --stay\n";
            std::cout << "  SudoVdaController remove 01234567-89ab-cdef-0123-456789abcdef\n";
            return 0;
        }
    }

    auto cli = CliParser::Parse(argc, argv);

    // All commands are forwarded to the tray server which owns displays.
    // Build a simple message format: verb\nkey=val&key2=val2...\n
    auto send_kv = [&](const std::string& verb, const std::map<std::string,std::string>& kv) -> std::pair<bool,std::string> {

        std::ostringstream msg;
        msg << verb << "\n";

        bool first = true;
        for (const auto& p : kv) {
            if (!first) msg << "&";
            first = false;
            msg << p.first << "=" << p.second;
        }

        std::string resp;
        if (!SendToTray(msg.str(), resp)) {
            return { false, std::string("{\"error\":\"failed to contact tray\"}") };
        }

        return { true, resp };
    };

    if (cli.verb == "create") {
        VirtualDisplayConfig cfg;
        std::optional<GUID> guidOpt;

        // Parse args: first non-flag is device name. Flags supported:
        // --width N, --height N, --refresh N (milliHz or float Hz), --hdr [0|1], --no-hdr, --adapter <uint64>, --guid <guid>
        for (size_t i = 0; i < cli.args.size(); ++i) {
            std::string a = cli.args[i];
            if (a.rfind("--", 0) == 0) {
                // support --key=value or --key value
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
                            // float -> interpret as Hz
                            double hz = std::stod(val);
                            auto hzi = static_cast<int>(hz * 1000.0 + 0.5);
                            cfg.refreshRateMilliHz = hzi;
                        }
                        else {
                            // integer -> be helpful:
                            //  - values < 1000 are interpreted as Hz (e.g. 115 -> 115000)
                            //  - values >= 1000 are interpreted as milliHz (e.g. 115000 -> 115000)
                            long long v = std::stoll(val);
                            if (v < 20) v = 20;
                            if (v < 1000) {
                                auto hzi = static_cast<int>(v * 1000);
                                cfg.refreshRateMilliHz = hzi;
                            }
                            else {
                                cfg.refreshRateMilliHz = static_cast<int>(v);
                            }
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
                        if (val.rfind("0x", 0) == 0 || val.rfind("0X", 0) == 0) {
                            cfg.adapterLuid = std::stoull(val, nullptr, 16);
                        }
                        else {
                            cfg.adapterLuid = std::stoull(val);
                        }
                    }
                    catch (...) {}
                }
                else if (key == "--guid") {
                    auto g = vdc::StringToGuid(val);
                    if (g) guidOpt = *g;
                }
            }
            else {
                // first non-flag argument is device name
                if (cfg.deviceName.empty()) {
                    std::wstring device = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(a);
                    cfg.deviceName = device;
                }
            }
        }

        // Build kv map and send to tray
        std::map<std::string,std::string> kv;
        kv["deviceName"] = PercentEncodeUtf8(cfg.deviceName);
        kv["width"] = std::to_string(cfg.width);
        kv["height"] = std::to_string(cfg.height);
        kv["refresh"] = std::to_string(cfg.refreshRateMilliHz);
        kv["hdr"] = cfg.hdr ? "1" : "0";
        kv["primary"] = cfg.primary ? "1" : "0";
        if (cfg.adapterLuid) kv["adapter"] = std::to_string(cfg.adapterLuid);
        if (guidOpt) kv["guid"] = vdc::GuidToString(*guidOpt);

        auto res = send_kv("create", kv);
        std::cout << res.second << std::endl;

        return 0;
    }

    if (cli.verb == "remove" && cli.args.size() >= 1) {
        std::map<std::string,std::string> kv;
        kv["guid"] = cli.args[0];
        auto res = send_kv("remove", kv);
        std::cout << res.second << std::endl;

        // fallback: try remove locally when tray not reachable
        if (!res.first) {
            auto g = vdc::StringToGuid(cli.args[0]);
            if (!g) { std::cout << "{\"error\":\"invalid guid\"}\n"; return 2; }
            VirtualDisplayController localController;
            auto localRes = localController.RemoveDisplay(*g);
            std::cout << localRes.json << std::endl;
            return localRes.success ? 0 : 1;
        }

        return res.first ? 0 : 1;
    }

    if (cli.verb == "mode" && cli.args.size() >= 5) {
        std::map<std::string,std::string> kv;
        kv["guid"] = cli.args[0];
        kv["w"] = cli.args[1];
        kv["h"] = cli.args[2];
        kv["refresh"] = cli.args[3];
        kv["iso"] = cli.args[4];
        auto res = send_kv("mode", kv);
        std::cout << res.second << std::endl;
        return res.first ? 0 : 1;
    }

    if (cli.verb == "primary" && cli.args.size() >= 1) {
        std::map<std::string,std::string> kv;
        kv["guid"] = cli.args[0];
        auto res = send_kv("primary", kv);
        std::cout << res.second << std::endl;
        return res.first ? 0 : 1;
    }

    if (cli.verb == "hdr" && cli.args.size() >= 2) {
        std::map<std::string,std::string> kv;
        kv["guid"] = cli.args[0];
        kv["enable"] = cli.args[1];
        auto res = send_kv("hdr", kv);
        std::cout << res.second << std::endl;
        return res.first ? 0 : 1;
    }

    if (cli.verb == "query" && cli.args.size() >= 1) {
        std::map<std::string,std::string> kv;
        kv["guid"] = cli.args[0];
        auto res = send_kv("query", kv);
        std::cout << res.second << std::endl;
        return res.first ? 0 : 1;
    }

    if (cli.verb == "kill") {
        // Request the tray process to exit
        auto res = send_kv("exit", {});
        std::cout << res.second << std::endl;
        return res.first ? 0 : 1;
    }

    if (cli.verb == "sunshine") {
        // Gather environment variables used by Sunshine / Apollo streaming clients
        auto getenv_first = [&](std::initializer_list<const char*> names)->std::optional<std::string> {
            for (const char* n : names) {
                const char* v = std::getenv(n);
                if (v && *v) return std::string(v);
            }
            return std::nullopt;
            };

        VirtualDisplayConfig cfg;

        if (auto dn = getenv_first({ "SUNSHINE_APP_NAME","APOLLO_APP_NAME","APOLLO_CLIENT_NAME" })) {
            std::wstring wdn = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(*dn);
            cfg.deviceName = wdn;
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
                // prefer integer fps -> milliHz
                if (f->find('.') != std::string::npos) {
                    double hz = std::stod(*f);
                    cfg.refreshRateMilliHz = static_cast<int>(hz * 1000.0 + 0.5);
                }
                else {
                    int fps = std::stoi(*f);
                    cfg.refreshRateMilliHz = fps * 1000;
                }
            }
            catch (...) {}
        }
        if (auto hdr = getenv_first({ "SUNSHINE_CLIENT_HDR","APOLLO_CLIENT_HDR" })) {
            std::string v = *hdr; for (auto& c : v) c = (char)tolower((unsigned char)c);
            cfg.hdr = (v == "1" || v == "true" || v == "yes");
        }

        // Build kv and send
        std::map<std::string, std::string> kv;
        kv["deviceName"] = PercentEncodeUtf8(cfg.deviceName);
        kv["width"] = std::to_string(cfg.width);
        kv["height"] = std::to_string(cfg.height);
        kv["refresh"] = std::to_string(cfg.refreshRateMilliHz);
        kv["hdr"] = cfg.hdr ? "1" : "0";
        kv["primary"] = "1";

        auto res = send_kv("create", kv);
        std::cout << res.second << std::endl;
        return 0;
    }

    std::cout << "{\"error\":\"unknown verb\"}\n";
    std::cout << "SudoVdaController exiting...\n";
    return 2;
}
