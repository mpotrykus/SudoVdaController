#pragma once
#include <string>

namespace vdc {
    // Run the tray-mode server. This function blocks until the tray server exits.
    int RunTrayServer(const std::wstring& pipeName);
}
