#pragma once
#include <string>
#include <mutex>
#include <vector>
#include <cstdio>

namespace vdc {

    enum class LogLevel {
        Debug = 0,
        Info,
        Success,
        Warning,
        Error,
        Critical
    };

    class Logger {
    public:
        static void SetLevel(LogLevel level);
        static LogLevel GetLevel();
        // Initialize the "last run" file (truncate / create). Call once at startup.
        static void InitLastRunFile();

        // Core logging entry point
        static void Log(LogLevel level, const std::string& msg, const char* location = "");

        // printf-style entry points
        // Variadic-template overloads are provided to safely forward common C++ types
        // (e.g. `const char*`, `std::string`, integers, floating point) to
        // `std::snprintf` after converting `std::string` to `const char*`.
        template<typename... Args>
        static void Log(LogLevel level, const char* fmt, Args&&... args) {
            std::string out = Format(fmt, std::forward<Args>(args)...);
            Dispatch(level, "", out);
        }

        // Convenience helpers
        // Convenience helpers (templated to accept `std::string` as well).
        template<typename... Args>
        static void Debug(const char* fmt, Args&&... args) { auto out = Format(fmt, std::forward<Args>(args)...); Dispatch(LogLevel::Debug, "", out); }

        template<typename... Args>
        static void Info(const char* fmt, Args&&... args) { auto out = Format(fmt, std::forward<Args>(args)...); Dispatch(LogLevel::Info, "", out); }

        template<typename... Args>
        static void Success(const char* fmt, Args&&... args) { auto out = Format(fmt, std::forward<Args>(args)...); Dispatch(LogLevel::Success, "", out); }

        template<typename... Args>
        static void Warn(const char* fmt, Args&&... args) { auto out = Format(fmt, std::forward<Args>(args)...); Dispatch(LogLevel::Warning, "", out); }

        template<typename... Args>
        static void Error(const char* fmt, Args&&... args) { auto out = Format(fmt, std::forward<Args>(args)...); Dispatch(LogLevel::Error, "", out); }

        template<typename... Args>
        static void Critical(const char* fmt, Args&&... args) { auto out = Format(fmt, std::forward<Args>(args)...); Dispatch(LogLevel::Critical, "", out); }

    private:
        static std::string FormatVA(const char* fmt, va_list ap);
        // Helper formatting overload that accepts C++ types and forwards to `std::snprintf`.
        template<typename... Args>
        static std::string Format(const char* fmt, Args&&... args) {
            // convert std::string to const char* automatically via overloads below
            // Try formatting into a buffer and grow if it's too small. Some C
            // runtimes (MSVC) may return negative when the buffer is too small,
            // so treat negative as a signal to retry with a larger buffer.
            std::vector<char> buf(1024);
            for (;;) {
                int res = std::snprintf(buf.data(), buf.size(), fmt, ConvertArg(std::forward<Args>(args))...);
                if (res < 0) {
                    // likely MSVC behavior when buffer too small; increase and retry
                    if (buf.size() > (1 << 20)) return std::string(fmt); // give up after 1MB
                    buf.resize(buf.size() * 2);
                    continue;
                }
                if (static_cast<size_t>(res) < buf.size()) {
                    return std::string(buf.data(), res);
                }
                // res >= buf.size(): resize to required size and retry
                buf.resize(static_cast<size_t>(res) + 1);
            }
        }

        // Conversion helper: std::string -> const char*, passthrough otherwise
        static inline const char* ConvertArg(const std::string& s) { return s.c_str(); }
        template<typename T> static inline T&& ConvertArg(T&& v) { return std::forward<T>(v); }
        static void Dispatch(LogLevel level, const char* location, const std::string& msg);
    };

} // namespace vdc

// Forwarding macros
#define LOG_DEBUG(...)    vdc::Logger::Debug(__VA_ARGS__)
#define LOG_INFO(...)     vdc::Logger::Info(__VA_ARGS__)
#define LOG_SUCCESS(...)  vdc::Logger::Success(__VA_ARGS__)
#define LOG_WARN(...)     vdc::Logger::Warn(__VA_ARGS__)
#define LOG_ERROR(...)    vdc::Logger::Error(__VA_ARGS__)
#define LOG_CRITICAL(...) vdc::Logger::Critical(__VA_ARGS__)
