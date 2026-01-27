#pragma once

#include <string>
#include <map>
#include <windows.h>
#include <mutex>

namespace vdc {

    class VirtualDisplayController;

    // Parse "key=value&key2=value2" into a map
    std::map<std::string, std::string> ParseKvForm(const std::string& s);

    // Percent-decode UTF‑8 (for device names)
    std::string PercentDecode(const std::string& s);

    // Read entire message from pipe until closed or no more data
    bool ReadAllFromPipe(HANDLE hPipe, std::string& out);

    // Write entire message to pipe
    bool WriteAllToPipe(HANDLE hPipe, const std::string& msg);

    // Main pipe server loop (runs in its own thread)
    void RunPipeServerLoop(const std::wstring& pipeName,
        VirtualDisplayController* controller,
        std::mutex* controllerMutex);

} // namespace vdc
