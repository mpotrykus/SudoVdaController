// File: src/VirtualDisplay/VirtualDisplaySession.h
#pragma once
#include <string>
#include <optional>
#include <windows.h>
#include "../models/VirtualDisplayConfig.h"
#include "../models/VirtualDisplayTypes.h"

namespace vdisplay {

    class VirtualDisplaySession {
    public:
        VirtualDisplaySession(const GUID& guid, const std::wstring& deviceName, const vdc::VirtualDisplayConfig& config);
        void SetGuid(const GUID& g) { guid_ = g; }
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