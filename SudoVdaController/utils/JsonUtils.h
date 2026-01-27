#pragma once
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <type_traits>
#include "GuidUtils.h"

namespace vdc {

    class JsonBuilder {
    public:
        class JsonValue {
        public:
            enum class Type { Null, Bool, Number, String, Array, Object };

            JsonValue() : type_(Type::Null), num_(0), bool_(false) {}
            static JsonValue Object() { JsonValue v; v.type_ = Type::Object; v.obj_.clear(); return v; }
            static JsonValue Array() { JsonValue v; v.type_ = Type::Array; v.arr_.clear(); return v; }
            static JsonValue String(const std::string& s) { JsonValue v; v.type_ = Type::String; v.str_ = s; return v; }
            static JsonValue Number(double n) { JsonValue v; v.type_ = Type::Number; v.num_ = n; return v; }
            static JsonValue Bool(bool b) { JsonValue v; v.type_ = Type::Bool; v.bool_ = b; return v; }

            // object accessor
            JsonValue& operator[](const std::string& key) {
                if (type_ != Type::Object) { type_ = Type::Object; obj_.clear(); }
                for (auto &kv : obj_) {
                    if (kv.first == key) return kv.second;
                }
                obj_.emplace_back(key, JsonValue());
                return obj_.back().second;
            }
            // const accessor (returns Null if not present)
            const JsonValue& Get(const std::string& key) const {
                static JsonValue nullv;
                if (type_ != Type::Object) return nullv;
                for (const auto &kv : obj_) {
                    if (kv.first == key) return kv.second;
                }
                return nullv;
            }
            bool IsNull() const { return type_ == Type::Null; }
            bool IsObject() const { return type_ == Type::Object; }
            bool IsArray() const { return type_ == Type::Array; }
            bool IsString() const { return type_ == Type::String; }
            bool IsNumber() const { return type_ == Type::Number; }
            bool IsBool() const { return type_ == Type::Bool; }

            std::vector<std::string> ObjectKeys() const {
                std::vector<std::string> keys;
                if (type_ != Type::Object) return keys;
                for (const auto &kv : obj_) keys.push_back(kv.first);
                return keys;
            }
            const JsonValue& AtIndex(size_t idx) const { static JsonValue nullv; if (type_!=Type::Array || idx>=arr_.size()) return nullv; return arr_[idx]; }
            size_t ArraySize() const { return arr_.size(); }

            std::string AsString() const { if (type_ == Type::String) return str_; if (type_==Type::Number) { std::ostringstream ss; ss<<num_; return ss.str(); } if (type_==Type::Bool) return bool_?"true":"false"; return std::string(); }
            int AsInt() const { if (type_==Type::Number) return static_cast<int>(num_); if (type_==Type::String) { try { return std::stoi(str_); } catch(...){} } return 0; }
            long long AsLongLong() const { if (type_==Type::Number) return static_cast<long long>(num_); if (type_==Type::String) { try { return std::stoll(str_); } catch(...){} } return 0; }
            double AsDouble() const { if (type_==Type::Number) return num_; if (type_==Type::String) { try { return std::stod(str_); } catch(...){} } return 0.0; }
            bool AsBool() const { if (type_==Type::Bool) return bool_; if (type_==Type::String) { std::string s = str_; std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s=="true"; } return false; }
            // array push
            void push_back(const JsonValue& v) { if (type_ != Type::Array) { type_ = Type::Array; arr_.clear(); } arr_.push_back(v); }

            // helpers to set typed values
            void SetString(const std::string& s) { type_ = Type::String; str_ = s; }
            void SetNumber(double n) { type_ = Type::Number; num_ = n; }
            void SetBool(bool b) { type_ = Type::Bool; bool_ = b; }

            std::string ToString(int indent = 0) const {
                std::ostringstream oss;
                ToStream(oss, indent, 0);
                return oss.str();
            }

        private:
            void ToStream(std::ostringstream& oss, int indent, int level) const {
                std::string pad(level * indent, ' ');
                switch (type_) {
                case Type::Null: oss << "null"; break;
                case Type::Bool: oss << (bool_ ? "true" : "false"); break;
                case Type::Number: {
                    // print as integer when possible
                    if (std::floor(num_) == num_) oss << (long long)num_; else oss << num_;
                    break; }
                case Type::String: {
                    oss.put('"');
                    oss << Escape(str_);
                    oss.put('"');
                    break;
                }
                case Type::Array: {
                    oss << "[";
                    if (!arr_.empty()) {
                        oss << '\n';
                        for (size_t i=0;i<arr_.size();++i) {
                            oss << std::string((level+1)*indent, ' ');
                            arr_[i].ToStream(oss, indent, level+1);
                            if (i+1 < arr_.size()) oss << ',';
                            oss << '\n';
                        }
                        oss << pad;
                    }
                    oss << "]";
                    break; }
                case Type::Object: {
                    oss << "{";
                    if (!obj_.empty()) {
                        oss << '\n';
                        size_t i = 0;
                        for (const auto &kv : obj_) {
                            oss << std::string((level+1)*indent, ' ') << '"' << Escape(kv.first) << '"' << ": ";
                            kv.second.ToStream(oss, indent, level+1);
                            if (++i < obj_.size()) oss << ',';
                            oss << '\n';
                        }
                        oss << pad;
                    }
                    oss << "}";
                    break; }
                }
            }

            Type type_;
            // preserve insertion order for object members
            std::vector<std::pair<std::string, JsonValue>> obj_;
            std::vector<JsonValue> arr_;
            std::string str_;
            double num_;
            bool bool_;
        };

        // Parse JSON text into JsonValue structure
        static JsonValue ParseJson(const std::string& s) {
            size_t i = 0; SkipWs(s, i); return ParseJsonValue(s, i);
        }
    private:
        static JsonValue ParseJsonValue(const std::string& s, size_t& i) {
            JsonValue out;
            SkipWs(s, i);
            if (i >= s.size()) return out;
            char c = s[i];
            if (c == '{') {
                out = JsonValue::Object(); ++i; SkipWs(s, i);
                while (i < s.size() && s[i] != '}') {
                    if (s[i] == ',') { ++i; SkipWs(s, i); continue; }
                    std::string key = ParseString(s, i);
                    SkipWs(s, i);
                    if (i < s.size() && s[i] == ':') ++i;
                    SkipWs(s, i);
                    JsonValue val = ParseJsonValue(s, i);
                    out[key] = val;
                    SkipWs(s, i);
                }
                if (i < s.size() && s[i] == '}') ++i;
                return out;
            }
            if (c == '[') {
                out = JsonValue::Array(); ++i; SkipWs(s, i);
                while (i < s.size() && s[i] != ']') {
                    if (s[i] == ',') { ++i; SkipWs(s, i); continue; }
                    JsonValue val = ParseJsonValue(s, i);
                    out.push_back(val);
                    SkipWs(s, i);
                }
                if (i < s.size() && s[i] == ']') ++i;
                return out;
            }
            if (c == '"') {
                std::string str = ParseString(s, i);
                out = JsonValue::String(str);
                return out;
            }
            // literals: number, true, false, null
            size_t start = i;
            while (i < s.size() && s[i] != ',' && s[i] != ']' && s[i] != '}') ++i;
            std::string token = s.substr(start, i - start);
            // trim
            size_t a = token.find_first_not_of(" \t\r\n");
            size_t b = token.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) return out;
            token = token.substr(a, b - a + 1);
            if (token == "true") { out = JsonValue::Bool(true); return out; }
            if (token == "false") { out = JsonValue::Bool(false); return out; }
            if (token == "null") { return out; }
            // try number
            try {
                double v = std::stod(token);
                out = JsonValue::Number(v);
                return out;
            } catch(...) {
                // fallback to string
                out = JsonValue::String(token);
                return out;
            }
        }

        public:
        
            void Add(const std::string& key, const std::string& value) {
                std::string v = "\"" + Escape(value) + "\"";
                for (auto &kv : entries_) {
                    if (kv.first == key) { kv.second = v; return; }
                }
                entries_.emplace_back(key, v);
            }
            void AddRaw(const std::string& key, const std::string& value) {
                for (auto &kv : entries_) {
                    if (kv.first == key) { kv.second = value; return; }
                }
                entries_.emplace_back(key, value);
            }
        
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
        // preserve insertion order for entries added via JsonBuilder::Add
        std::vector<std::pair<std::string, std::string>> entries_;
        static std::string Escape(const std::string& in) {
            std::string out;
            for (char c : in) {
                if (c == '"') out += "\\\"";
                else out += c;
            }
            return out;
        }
    };

    // Generic JSON (de)serialization helpers.
    // JsonSerializer falls back to calling T::ToJson()/T::FromJson() when available
    // and provides specializations for common types (string, bool, numbers, vectors, maps).
    template<typename T, typename Enable = void>
    struct JsonSerializer {
        static JsonBuilder::JsonValue ToJson(const T& v) { return v.ToJson(); }
        static T FromJson(const JsonBuilder::JsonValue& j) { return T::FromJson(j); }
    };

    // string
    template<>
    struct JsonSerializer<std::string, void> {
        static JsonBuilder::JsonValue ToJson(const std::string& s) { return JsonBuilder::JsonValue::String(s); }
        static std::string FromJson(const JsonBuilder::JsonValue& j) { return j.AsString(); }
    };

    // bool
    template<>
    struct JsonSerializer<bool, void> {
        static JsonBuilder::JsonValue ToJson(bool b) { return JsonBuilder::JsonValue::Bool(b); }
        static bool FromJson(const JsonBuilder::JsonValue& j) { return j.AsBool(); }
    };

    // integral types (except bool)
    template<typename T>
    struct JsonSerializer<T, typename std::enable_if<std::is_integral<T>::value && !std::is_same<T,bool>::value>::type> {
        static JsonBuilder::JsonValue ToJson(T v) { return JsonBuilder::JsonValue::Number((double)v); }
        static T FromJson(const JsonBuilder::JsonValue& j) { return static_cast<T>(j.AsLongLong()); }
    };

    // floating point
    template<typename T>
    struct JsonSerializer<T, typename std::enable_if<std::is_floating_point<T>::value>::type> {
        static JsonBuilder::JsonValue ToJson(T v) { return JsonBuilder::JsonValue::Number((double)v); }
        static T FromJson(const JsonBuilder::JsonValue& j) { return static_cast<T>(j.AsDouble()); }
    };

    // vector
    template<typename U>
    struct JsonSerializer<std::vector<U>, void> {
        static JsonBuilder::JsonValue ToJson(const std::vector<U>& arr) {
            JsonBuilder::JsonValue a = JsonBuilder::JsonValue::Array();
            for (const auto &e : arr) a.push_back(JsonSerializer<U>::ToJson(e));
            return a;
        }
        static std::vector<U> FromJson(const JsonBuilder::JsonValue& j) {
            std::vector<U> out;
            if (!j.IsArray()) return out;
            for (size_t i = 0; i < j.ArraySize(); ++i) out.push_back(JsonSerializer<U>::FromJson(j.AtIndex(i)));
            return out;
        }
    };

    // map<string, V>
    template<typename V>
    struct JsonSerializer<std::map<std::string, V>, void> {
        static JsonBuilder::JsonValue ToJson(const std::map<std::string, V>& m) {
            JsonBuilder::JsonValue obj = JsonBuilder::JsonValue::Object();
            for (const auto &kv : m) obj[kv.first] = JsonSerializer<V>::ToJson(kv.second);
            return obj;
        }
        static std::map<std::string, V> FromJson(const JsonBuilder::JsonValue& j) {
            std::map<std::string, V> out;
            if (!j.IsObject()) return out;
            for (const auto &k : j.ObjectKeys()) out[k] = JsonSerializer<V>::FromJson(j.Get(k));
            return out;
        }
    };

    // convenience wrappers
    template<typename T>
    inline JsonBuilder::JsonValue ToJson(const T& v) { return JsonSerializer<T>::ToJson(v); }

    template<typename T>
    inline T FromJson(const JsonBuilder::JsonValue& j) { return JsonSerializer<T>::FromJson(j); }

    // Array helpers: convert Json array -> std::vector<T> and back.
    template<typename T>
    inline std::vector<T> FromJsonArray(const JsonBuilder::JsonValue& arr) {
        std::vector<T> out;
        if (!arr.IsArray()) return out;
        for (size_t i = 0; i < arr.ArraySize(); ++i) out.push_back(FromJson<T>(arr.AtIndex(i)));
        return out;
    }

    template<typename T>
    inline JsonBuilder::JsonValue ToJsonArray(const std::vector<T>& vec) {
        JsonBuilder::JsonValue a = JsonBuilder::JsonValue::Array();
        for (const auto &e : vec) a.push_back(ToJson<T>(e));
        return a;
    }

    // GUID specialization
    template<>
    struct JsonSerializer<GUID, void> {
        static JsonBuilder::JsonValue ToJson(const GUID& g) { return JsonBuilder::JsonValue::String(vdc::GuidToString(g)); }
        static GUID FromJson(const JsonBuilder::JsonValue& j) { auto s = j.AsString(); auto opt = vdc::StringToGuid(s); if (opt) return *opt; return GUID{}; }
    };

} // namespace vdc
