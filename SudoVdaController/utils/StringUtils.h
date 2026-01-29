#pragma once
#include <string>
#include <optional>
#include <windows.h>
#include <sstream>
#include <iomanip>

namespace vdc {

    static std::wstring StringToWString(const std::string& s) {
        if (s.empty()) return std::wstring();
        int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
        if (sz <= 0) return std::wstring();
        std::wstring out; out.resize(sz);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], sz);
        return out;
    }

    static std::string WStringToString(const std::wstring& w) {
        if (w.empty()) return std::string();
        int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
        if (sz <= 0) return std::string();
        std::string out; out.resize(sz);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], sz, NULL, NULL);
        return out;
    }

    static std::string ToLower(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static std::string PercentEncodeUtf8(const std::wstring& w) {
        std::string u = WStringToString(w);
        std::ostringstream oss;
        for (unsigned char c : u) {
            // safe characters: alnum and -_.~
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                c == '-' || c == '_' || c == '.' || c == '~') {
                oss << c;
            }
            else if (c == ' ') {
                oss << '+';
            }
            else {
                oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << int(c) << std::dec;
            }
        }
        return oss.str();
    }

    static std::string TrimWhitespace(std::wstring& s) {
        const std::wstring ws = L" \t\n\r\f\v";
        const auto start = s.find_first_not_of(ws);
        if (start == std::wstring::npos) { s.clear(); return; }
        const auto end = s.find_last_not_of(ws);
        s = s.substr(start, end - start + 1);
    };

} // namespace vdc
