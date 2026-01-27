#include "pch.h"

#include "utils/CliParser.h"
#include "utils/Logger.h"
#include "handlers/VerbHandlers.h"
#include "client/TrayServer.h"
#include <iostream>

using namespace vdc;

int main(int argc, char** argv) {
    LOG_INFO("SudoVdaController starting...");
    EnsureComInitialized();

    if (argc <= 1)
        return vdc::RunTrayServer(L"SudoVdaTrayPipe");

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--tray")
            return vdc::RunTrayServer(L"SudoVdaTrayPipe");
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h" || arg == "help") {
            PrintHelp();
            return 0;
        }
    }

    auto cli = CliParser::Parse(argc, argv);
    return HandleVerb(cli);
}
