#pragma once
#include <string>
#include <vector>
#include <optional>
#include <windows.h>

namespace vdc {

    struct CliArgs {
        std::string verb;
        std::vector<std::string> args;
    };

    class CliParser {
    public:
        static CliArgs Parse(int argc, char** argv) {
            CliArgs out;
            if (argc > 1) out.verb = argv[1];
            for (int i = 2; i < argc; ++i) out.args.emplace_back(argv[i]);
            return out;
        }
    };

} // namespace vdc
