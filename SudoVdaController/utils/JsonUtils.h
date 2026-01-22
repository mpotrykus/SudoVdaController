#pragma once
#include <string>
#include <map>
#include <cctype>
#include <sstream>

namespace vdc {

    class JsonBuilder {
    public:
        void Add(const std::string& k, const std::string& v) { entries_[k] = "\"" + Escape(v) + "\""; }
        void AddRaw(const std::string& k, const std::string& v) { entries_[k] = v; }
        std::string Build() const {
            std::string s = "{";
            bool first = true;
            for (auto& kv : entries_) {
                if (!first) s += ",";
                first = false;
                s += "\"" + kv.first + "\":" + kv.second;
            }
            s += "}";
            return s;
        }
        static void SkipWs(const std::string& s, size_t& i) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        }

        static std::string ParseString(const std::string& s, size_t& i) {
            std::string out;
            if (i >= s.size() || s[i] != '"') return out;
            ++i;
            while (i < s.size()) {
                char c = s[i++];
                if (c == '\\' && i < s.size()) {
                    char esc = s[i++];
                    switch (esc) {
                    case '\"': out.push_back('\"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default: out.push_back(esc); break;
                    }
                }
                else if (c == '"') {
                    break;
                }
                else {
                    out.push_back(c);
                }
            }
            return out;
        }

        static std::string IndentStr(int indent) {
            return std::string(indent * 2, ' ');
        }

        static std::string ParseObject(const std::string& s, size_t& i, int indent) {
            // assumes s[i] == '{'
            std::string out;
            ++i;
            SkipWs(s, i);
            bool first = true;
            while (i < s.size() && s[i] != '}') {
                if (!first) {
                    // consume comma
                    if (s[i] == ',') ++i;
                    SkipWs(s, i);
                }
                first = false;
                // key
                std::string key = ParseString(s, i);
                SkipWs(s, i);
                if (i < s.size() && s[i] == ':') ++i;
                SkipWs(s, i);
                // value
                size_t valStart = i;
                std::string value = ParseValue(s, i, indent + 1);
                // Format: - key: value (value may include newlines)
                std::string prefix = IndentStr(indent) + "- " + key + ": ";
                if (!value.empty() && value.find('\n') != std::string::npos) {
                    // multi-line, place on subsequent lines
                    out += prefix + "\n";
                    // indent nested lines one more level
                    std::istringstream iss(value);
                    std::string line;
                    while (std::getline(iss, line)) {
                        out += IndentStr(indent + 1) + line + "\n";
                    }
                }
                else {
                    out += prefix + value + "\n";
                }
                SkipWs(s, i);
            }
            if (i < s.size() && s[i] == '}') ++i;
            return out;
        }

        static std::string ParseArray(const std::string& s, size_t& i, int indent) {
            // assumes s[i] == '['
            std::string out;
            ++i;
            SkipWs(s, i);
            bool first = true;
            while (i < s.size() && s[i] != ']') {
                if (!first) {
                    if (s[i] == ',') ++i;
                    SkipWs(s, i);
                }
                first = false;
                std::string value = ParseValue(s, i, indent + 1);
                // present each element on its own line
                out += "[ ] " + value + "\n";
                SkipWs(s, i);
            }
            if (i < s.size() && s[i] == ']') ++i;
            return out;
        }

        static std::string ParseValue(const std::string& s, size_t& i, int indent) {
            SkipWs(s, i);
            if (i >= s.size()) return {};
            char c = s[i];
            if (c == '"') {
                return ParseString(s, i);
            }
            if (c == '{') {
                // nested object -> indent and return its formatted block (without extra prefix)
                return ParseObject(s, i, indent);
            }
            if (c == '[') {
                return ParseArray(s, i, indent);
            }
            // literal (number, true, false, null)
            size_t start = i;
            while (i < s.size() && s[i] != ',' && s[i] != ']' && s[i] != '}') ++i;
            std::string token = s.substr(start, i - start);
            // trim
            size_t a = token.find_first_not_of(" \t\r\n");
            size_t b = token.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) return {};
            return token.substr(a, b - a + 1);
        }

        static std::string FormatJsonAsList(const std::string& json) {
            size_t i = 0;
            SkipWs(json, i);
            if (i < json.size() && json[i] == '{') {
                return ParseObject(json, i, 0);
            }
            // fallback: show raw json
            return json;
        }
    private:
        std::map<std::string, std::string> entries_;
        static std::string Escape(const std::string& in) {
            std::string out;
            for (char c : in) {
                if (c == '"') out += "\\\"";
                else out += c;
            }
            return out;
        }
    };

} // namespace vdc
