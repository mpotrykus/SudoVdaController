#pragma once

#include <optional>
#include <string>
#include <functional>
#include <windows.h>
#include <guiddef.h>

namespace vdc {

    // Thin RAII wrapper around the SUDOVDA driver handle and basic operations.
    class SudovdaDriver {
    public:
        enum class Status {
            Ok,
            Failed,
            VersionIncompatible
        };

        SudovdaDriver();
        ~SudovdaDriver();

        // Non-copyable, movable
        SudovdaDriver(const SudovdaDriver&) = delete;
        SudovdaDriver& operator=(const SudovdaDriver&) = delete;
        SudovdaDriver(SudovdaDriver&& other) noexcept;
        SudovdaDriver& operator=(SudovdaDriver&& other) noexcept;

        // Open the driver if not already open.
        Status Open();

        // True if handle is valid.
        bool IsOpen() const noexcept;

        // Close the driver explicitly.
        void Close();

        // Start watchdog ping thread; calls failCb if driver stops responding.
        bool StartWatchdog(std::function<void()> failCb);

        // Set render adapter by DXGI adapter name.
        bool SetRenderAdapterByName(const std::wstring& adapterName);

        // Create a virtual display. Returns GDI device name on success.
        std::optional<std::wstring> CreateVirtualDisplay(
            const char* clientUid,
            const char* clientName,
            uint32_t width,
            uint32_t height,
            float fps,
            const GUID& guid
        );

        // Remove a virtual display by GUID.
        bool RemoveVirtualDisplay(const GUID& guid);

    private:
        HANDLE m_handle = INVALID_HANDLE_VALUE;

        Status OpenInternal();
        void CloseInternal();

        bool CheckProtocolCompatibleInternal();
        bool StartWatchdogInternal(std::function<void()> failCb);
    };

} // namespace vdc
