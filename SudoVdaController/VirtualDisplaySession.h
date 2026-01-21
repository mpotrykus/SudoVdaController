// File: src/VirtualDisplay/VirtualDisplaySession.h
#pragma once
#include <string>
#include <optional>
#include <windows.h>
#include "VirtualDisplayConfig.h"
#include "VirtualDisplayTypes.h"

namespace vdisplay {

    class VirtualDisplaySession {
    public:
        VirtualDisplaySession(const GUID& guid, const std::wstring& deviceName, const vdc::VirtualDisplayConfig& config);
        ~VirtualDisplaySession();

        const GUID& GetGuid() const;
        const std::wstring& GetDeviceName() const;
        const vdc::VirtualDisplayConfig& GetConfig() const;

        bool SetMode(int width, int height, int refreshRateMilliHz, bool isolatedLayout = false);
        bool SetPrimary();
        bool SetHdr(bool enable);

        bool IsHdrEnabled() const;
        std::optional<DisplayMode> GetCurrentMode() const;

    private:
        GUID guid_;
        std::wstring deviceName_;
        vdc::VirtualDisplayConfig config_;
    };

} // namespace vdisplay