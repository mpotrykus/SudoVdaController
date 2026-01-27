#include "Logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <cstdarg>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vdc {

    static LogLevel g_logLevel = LogLevel::Info;
    static std::mutex g_logMutex;

    void Logger::SetLevel(LogLevel level) {
        g_logLevel = level;
    }

    LogLevel Logger::GetLevel() {
        return g_logLevel;
    }

    static const char* LevelToString(LogLevel level) {
        switch (level) {
        case LogLevel::Debug:    return "DEBUG";
        case LogLevel::Info:     return "INFO";
        case LogLevel::Success:  return "SUCCESS";
        case LogLevel::Warning:  return "WARN";
        case LogLevel::Error:    return "ERROR";
        case LogLevel::Critical: return "CRITICAL";
        default:                 return "UNKNOWN";
        }
    }

#ifdef _WIN32
    static WORD LevelToColor(LogLevel level) {
        switch (level) {
        case LogLevel::Debug:    return FOREGROUND_INTENSITY | FOREGROUND_BLUE | FOREGROUND_GREEN;
        case LogLevel::Info:     return FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        case LogLevel::Success:  return FOREGROUND_INTENSITY | FOREGROUND_GREEN;
        case LogLevel::Warning:  return FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN;
        case LogLevel::Error:    return FOREGROUND_INTENSITY | FOREGROUND_RED;
        case LogLevel::Critical: return BACKGROUND_RED | FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        default:                 return FOREGROUND_INTENSITY;
        }
    }
#endif

    // -----------------------------------------------------------------------------
    // INTERNAL HELPERS
    // -----------------------------------------------------------------------------

    // Acceptable data types for printf-style format specifiers (used by the
    // template `Format` helper in Logger.h and by legacy variadic entrypoints):
    //  - %s : expects `const char*` or `std::string` (we convert `std::string` -> c_str())
    //  - %d, %i, %u, %x, %X, %o : integer types (`int`, `unsigned`, `long`, etc.)
    //  - %ld, %lld : `long`, `long long`
    //  - %zu : `size_t`
    //  - %f, %F, %g, %G : floating point (`double` / `float`)
    //  - %p : pointer types
    //  - %c : `char` (pass as `int` due to default argument promotions)
    // Note: passing types that don't match the format specifier is undefined
    // behavior. Prefer matching specifiers or explicitly convert values (e.g.
    // use `.c_str()` for `std::string` when calling legacy variadic APIs).

    std::string Logger::FormatVA(const char* fmt, va_list ap) {
        std::vector<char> buf(1024);

        va_list ap_copy;
        va_copy(ap_copy, ap);
        int needed = vsnprintf(buf.data(), buf.size(), fmt, ap_copy);
        va_end(ap_copy);

        if (needed < 0)
            return fmt;

        if (static_cast<size_t>(needed) < buf.size())
            return std::string(buf.data(), needed);

        buf.resize(needed + 1);
        vsnprintf(buf.data(), buf.size(), fmt, ap);
        return std::string(buf.data(), needed);
    }

    void Logger::Dispatch(LogLevel level, const char* location, const std::string& msg) {
        if (static_cast<int>(level) < static_cast<int>(g_logLevel))
            return;

        std::lock_guard<std::mutex> lock(g_logMutex);

        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::tm tmBuf{};
#ifdef _WIN32
        localtime_s(&tmBuf, &t);
#else
        localtime_r(&t, &tmBuf);
#endif

#ifdef _WIN32
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO oldInfo;

        if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &oldInfo)) {
            SetConsoleTextAttribute(h, LevelToColor(level));

            // Convert UTF-8 strings to wide and write using WriteConsoleW so Unicode
            // characters appear correctly in the Windows console.
            auto Utf8ToW = [](const std::string& s) -> std::wstring {
                if (s.empty()) return std::wstring();
                int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
                if (sz <= 0) return std::wstring();
                std::wstring out; out.resize(sz);
                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], sz);
                return out;
            };

            // Build wide string for the timestamp/level/location/message
            std::wostringstream wss;
            wss << std::put_time(&tmBuf, L"%Y-%m-%d %H:%M:%S")
                << L"." << std::setw(3) << std::setfill(L'0') << ms.count();
            // level string -> wide
            std::string lvlStr = LevelToString(level);
            std::wstring lvlW = Utf8ToW(lvlStr);
            wss << L" [" << lvlW << L"] ";

            if (location && location[0]) {
                std::wstring locW = Utf8ToW(std::string(location));
                if (!locW.empty()) wss << L"(" << locW << L") ";
            }

            std::wstring msgW = Utf8ToW(msg);
            wss << msgW;

            std::wstring outW = wss.str();

            DWORD written = 0;
            WriteConsoleW(h, outW.c_str(), static_cast<DWORD>(outW.size()), &written, NULL);
            // write trailing newline using WriteConsoleW
            const wchar_t nl = L'\n';
            WriteConsoleW(h, &nl, 1, &written, NULL);

            SetConsoleTextAttribute(h, oldInfo.wAttributes);
            return;
        }
#endif

        // ANSI fallback
        const char* colorStart = "";
        const char* colorEnd = "\x1b[0m";

        switch (level) {
        case LogLevel::Debug:    colorStart = "\x1b[36m"; break;
        case LogLevel::Info:     colorStart = "\x1b[32m"; break;
        case LogLevel::Success:  colorStart = "\x1b[32m"; break;
        case LogLevel::Warning:  colorStart = "\x1b[33m"; break;
        case LogLevel::Error:    colorStart = "\x1b[31m"; break;
        case LogLevel::Critical: colorStart = "\x1b[41;97m"; break;
        default: break;
        }

        std::cout << colorStart
            << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S")
            << "." << std::setw(3) << std::setfill('0') << ms.count()
            << " [" << LevelToString(level) << "] ";

        if (location && location[0])
            std::cout << "(" << location << ") ";

        std::cout << msg << colorEnd << std::endl;
    }

    // -----------------------------------------------------------------------------
    // PUBLIC API
    // -----------------------------------------------------------------------------

    void Logger::Log(LogLevel level, const std::string& msg, const char* location) {
        Dispatch(level, location, msg);
    }

    // Note: printf-style and convenience templated helpers are defined inline in
    // `Logger.h` (templated `Log`, `Info`, `Warn`, etc.). Those forward to the
    // core `Log(LogLevel, const std::string&, const char*)` entry point.
} // namespace vdc
