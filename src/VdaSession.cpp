#include "pch.h"
#include "VdaSession.h"
#include "VirtualDisplay.h"

#include <objbase.h>
#include <codecvt>
#include <optional>
#include <iostream>

using namespace vdc;

// Convert GUID to string without braces, UTF-8
static std::string guidToString(const GUID &g) {
    wchar_t wbuf[64] = {};
    if (!StringFromGUID2(g, wbuf, static_cast<int>(std::size(wbuf)))) {
        return {};
    }
    std::wstring ws(wbuf);
    if (!ws.empty() && ws.front() == L'{') ws.erase(ws.begin());
    if (!ws.empty() && ws.back() == L'}') ws.pop_back();
    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    return conv.to_bytes(ws);
}

static std::string wstringToUtf8(const std::wstring &ws) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    return conv.to_bytes(ws);
}

VdaSession::VdaSession() {
    // No reliance on internal member names; driver state is managed via VDISPLAY::SUDOVDA_DRIVER_HANDLE.
}

VdaSession::~VdaSession() {
    // Ensure driver is closed on destruction. Safe even if it wasn't opened here.
    VDISPLAY::closeVDisplayDevice();
}

bool VdaSession::Open() {
    // If the driver handle is already valid, consider it opened.
    if (VDISPLAY::SUDOVDA_DRIVER_HANDLE != INVALID_HANDLE_VALUE) {
        return true;
    }
    auto st = VDISPLAY::openVDisplayDevice();
    return st == VDISPLAY::DRIVER_STATUS::OK;
}

std::optional<std::wstring> VdaSession::AddVirtualDisplay(const GUID& guid, const std::wstring& deviceName) {
    if (!Open()) return std::nullopt;

    auto client_uid = guidToString(guid);
    if (client_uid.empty()) {
        std::cerr << "[VdaSession] Failed to stringify GUID\n";
        return std::nullopt;
    }
    auto name_utf8 = wstringToUtf8(deviceName);

    // Use conservative defaults; callers may extend API later to pass resolution/fps.
    uint32_t width = 2560;
    uint32_t height = 1600;
    float fps = 60.0f;

    auto dev = VDISPLAY::createVirtualDisplay(client_uid.c_str(), name_utf8.c_str(), width, height, fps, guid);
    if (dev.empty()) {
        std::cerr << "[VdaSession] createVirtualDisplay failed\n";
        return std::nullopt;
    }

    // Start ping thread if available (stubbed/no-op in prototype)
    VDISPLAY::startPingThread([dev]() {
        std::wcerr << L"[VdaSession] Watchdog detected driver failure for device: " << dev.c_str() << L'\n';
    });

    return dev;
}

bool VdaSession::RemoveVirtualDisplay(const GUID& guid) {
    if (!Open()) return false;
    return VDISPLAY::removeVirtualDisplay(guid);
}

bool VdaSession::SetRenderAdapter(const GUID& /*guid*/, uint64_t /*adapterLuid*/) {
    // Prototype: no-op. If you add a member or expose SetRenderAdapter by LUID in VDISPLAY,
    // implement conversion from uint64_t to LUID and call the driver helper here.
    return true;
}
