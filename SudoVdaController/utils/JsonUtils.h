#pragma once
#include <string>
#include <map>

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
