#pragma once
#include <cstdint>
#include <string>
#include <optional>
#include <windows.h>

namespace vdc {

class VdaSession {
public:
    VdaSession();
    ~VdaSession();

    bool Open(); // open driver handle
    std::optional<std::wstring> AddVirtualDisplay(const GUID& guid, const std::wstring& deviceName);
    bool RemoveVirtualDisplay(const GUID& guid);
    bool SetRenderAdapter(const GUID& guid, uint64_t adapterLuid);

private:
    // opaque handle
    void* handle_;
};

} // namespace vdc
