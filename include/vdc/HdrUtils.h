#pragma once
#include <string>

namespace vdc {

class HdrUtils {
public:
    static bool SetHdrState(const std::wstring& deviceName, bool enable);
    static bool IsHdrEnabled(const std::wstring& deviceName);
};

} // namespace vdc
