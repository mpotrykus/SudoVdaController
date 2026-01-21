#pragma once

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

// Compatibility shims for older MSVC toolsets that don't enable C++17 by default.
// Provide minimal fallbacks for `std::optional` and `std::string_view` when
// compiling without C++17 support. This keeps the code compiling under
// legacy toolsets used by some environments in this workspace.
#if defined(_MSC_VER) && (!defined(_MSVC_LANG) || _MSVC_LANG < 201703L)
// Minimal std::nullopt_t/nullopt and optional<T> implementation covering the
// small subset used across this project (construction, bool test, deref,
// value_or). Not a full replacement for std::optional.
namespace std {
    struct nullopt_t { explicit constexpr nullopt_t(int) {} };
    constexpr nullopt_t nullopt{0};

    template<typename T>
    class optional {
        bool has_ = false;
        alignas(T) unsigned char storage_[sizeof(T)];

        T* ptr() { return reinterpret_cast<T*>(&storage_); }
        const T* ptr() const { return reinterpret_cast<const T*>(&storage_); }
    public:
        optional() noexcept : has_(false) {}
        optional(nullopt_t) noexcept : has_(false) {}
        optional(const T& v) : has_(true) { new (ptr()) T(v); }
        optional(T&& v) : has_(true) { new (ptr()) T(std::move(v)); }
        optional(const optional& o) : has_(o.has_) { if (has_) new (ptr()) T(*o); }
        optional(optional&& o) noexcept : has_(o.has_) { if (has_) new (ptr()) T(std::move(*o)); }
        ~optional() { if (has_) ptr()->~T(); }

        optional& operator=(nullopt_t) noexcept { if (has_) { ptr()->~T(); has_ = false; } return *this; }
        optional& operator=(const T& v) { if (has_) *ptr() = v; else { new (ptr()) T(v); has_ = true; } return *this; }

        bool has_value() const noexcept { return has_; }
        T* operator->() { return ptr(); }
        const T* operator->() const { return ptr(); }
        T& value() { return *ptr(); }
        const T& value() const { return *ptr(); }

        explicit operator bool() const noexcept { return has_; }
        T& operator*() { return *ptr(); }
        const T& operator*() const { return *ptr(); }
        T value_or(const T& def) const { return has_ ? *ptr() : def; }
    };
}

// Minimal string_view / wstring_view fallback used for simple slicing and
// comparisons with std::wstring. Only implements what's necessary in this
// project (construct from const CharT*, size, data(), size(), operator!=).
namespace std {
    template<typename CharT>
    class basic_string_view {
        const CharT* data_ = nullptr;
        size_t size_ = 0;
    public:
        using value_type = CharT;
        basic_string_view() noexcept = default;
        basic_string_view(const CharT* s) noexcept : data_(s), size_(s ? std::char_traits<CharT>::length(s) : 0) {}
        basic_string_view(const CharT* s, size_t n) noexcept : data_(s), size_(n) {}
        const CharT* data() const noexcept { return data_; }
        size_t size() const noexcept { return size_; }
        bool empty() const noexcept { return size_ == 0; }
    };
    using string_view = basic_string_view<char>;
    using wstring_view = basic_string_view<wchar_t>;

    // comparisons with std::basic_string
    inline bool operator!=(const basic_string_view<wchar_t>& a, const std::wstring& b) noexcept {
        if (a.size() != b.size()) return true;
        return wcsncmp(a.data(), b.c_str(), a.size()) != 0;
    }
    inline bool operator!=(const std::wstring& a, const basic_string_view<wchar_t>& b) noexcept { return b != a; }

    // Additional comparison operators to match std::wstring_view behaviour used in the codebase.
    inline bool operator==(const basic_string_view<wchar_t>& a, const basic_string_view<wchar_t>& b) noexcept {
        if (a.size() != b.size()) return false;
        return wcsncmp(a.data(), b.data(), a.size()) == 0;
    }
    inline bool operator!=(const basic_string_view<wchar_t>& a, const basic_string_view<wchar_t>& b) noexcept { return !(a == b); }

    inline bool operator==(const basic_string_view<wchar_t>& a, const std::wstring& b) noexcept {
        if (a.size() != b.size()) return false;
        return wcsncmp(a.data(), b.c_str(), a.size()) == 0;
    }
    inline bool operator==(const std::wstring& a, const basic_string_view<wchar_t>& b) noexcept { return b == a; }

    inline bool operator==(const basic_string_view<wchar_t>& a, const wchar_t* b) noexcept {
        if (!b) return a.size() == 0;
        size_t bn = wcslen(b);
        if (a.size() != bn) return false;
        return wcsncmp(a.data(), b, a.size()) == 0;
    }
    inline bool operator==(const wchar_t* a, const basic_string_view<wchar_t>& b) noexcept { return b == a; }

    inline bool operator!=(const basic_string_view<wchar_t>& a, const wchar_t* b) noexcept { return !(a == b); }
    inline bool operator!=(const wchar_t* a, const basic_string_view<wchar_t>& b) noexcept { return !(a == b); }
}
#else
// When C++17 is available, include the proper headers.
#include <optional>
#include <string_view>
#endif

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
