#pragma once

// Allow use of legacy C and C++ APIs that MSVC marks as deprecated in favor
// of "secure" variants. This codebase intentionally uses some older APIs and
// std::codecvt utilities; silence the corresponding deprecation warnings here.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#endif

// Reduce Windows header bloat
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Windows and DirectX
#include <windows.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_6.h>

// Link required DirectX libraries for DXGI / D3D helper functions used by the
// codebase (CreateDXGIFactory1, D3D11 CreateDevice, etc.). Adding pragmas
// here avoids modifying project files and keeps the linker flags in-source.
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
// Link GDI+ for tray icon bitmap helpers
#pragma comment(lib, "Gdiplus.lib")

// GDI+ headers used by tray server / icon helpers
#include <gdiplus.h>

// Standard library
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <memory>
#include <vector>
#include <string>
#include <cstring>
#include <cwchar>


// Locale / codecvt (used in some translation units)
#include <locale>
#include <codecvt>

// Provide C++ standard utilities commonly used across translation units.
#include <optional>
#include <string_view>

// Ensure device/IOCTL macros like CTL_CODE, METHOD_BUFFERED, FILE_ANY_ACCESS
// are available for headers that declare driver IOCTLs.
#include <winioctl.h>

// Minimal helper to ensure COM is initialized on the calling thread.
// Safe to call multiple times; callers may ignore return value if they
// handle COM elsewhere.
inline bool EnsureComInitialized() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE || hr == S_FALSE;
}
