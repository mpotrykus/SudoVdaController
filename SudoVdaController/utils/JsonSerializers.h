#pragma once
#include "../models/DisplayConfig.h"
#include "JsonUtils.h"

namespace vdc {

    template<>
    struct JsonSerializer<Topology> {

        static JsonBuilder::JsonValue ToJson(const Topology& t) {
            JsonBuilder::JsonValue obj = JsonBuilder::JsonValue::Object();
            obj["displayId"] = JsonBuilder::JsonValue::String(t.displayId);
            obj["edid"] = JsonBuilder::JsonValue::String(t.edid);
            obj["displayName"] = JsonBuilder::JsonValue::String(t.displayName);
            obj["enabled"] = JsonBuilder::JsonValue::Bool(t.enabled);
            return obj;
        }

        static Topology FromJson(const JsonBuilder::JsonValue& j) {
            Topology t; if (j.IsNull() || !j.IsObject()) return t;
            t.displayId = j.Get("displayId").AsString();
            t.edid = j.Get("edid").AsString();
            t.displayName = j.Get("displayName").AsString();
            t.enabled = j.Get("enabled").AsBool();
            return t;
        }

    };

    template<>
    struct vdc::JsonSerializer<vdc::DisplayConfig> {

        static JsonBuilder::JsonValue ToJson(const DisplayConfig& c) {
            JsonBuilder::JsonValue obj = JsonBuilder::JsonValue::Object();
            obj["displayId"] = JsonBuilder::JsonValue::String(c.displayId);
            obj["width"] = JsonBuilder::JsonValue::Number(c.width);
            obj["height"] = JsonBuilder::JsonValue::Number(c.height);
            obj["refreshRateHz"] = JsonBuilder::JsonValue::Number(c.refreshRateHz);
            obj["topology"] = vdc::ToJsonArray<Topology>(c.topology);
            return obj;
        }

        static vdc::DisplayConfig FromJson(const JsonBuilder::JsonValue& j) {
            vdc::DisplayConfig c; if (j.IsNull() || !j.IsObject()) return c;
            c.displayId = j.Get("displayId").AsString();
            c.width = j.Get("width").AsInt(); c.height = j.Get("height").AsInt();
            c.refreshRateHz = (int)j.Get("refreshRateHz").AsLongLong();
            c.topology = vdc::FromJsonArray<Topology>(j.Get("topology"));
            return c;
        }

    };

    template<>
    struct vdc::JsonSerializer<vdc::Display> {

        static JsonBuilder::JsonValue ToJson(const Display& d) {
            JsonBuilder::JsonValue obj = JsonBuilder::JsonValue::Object();
            obj["displayName"] = JsonBuilder::JsonValue::String(d.displayName);
            obj["configs"] = vdc::ToJsonArray<DisplayConfig>(d.configs);
            return obj;
        }

        static vdc::Display FromJson(const JsonBuilder::JsonValue& j) {
            vdc::Display d; if (j.IsNull() || !j.IsObject()) return d;
            d.displayName = j.Get("displayName").AsString();
            d.configs = vdc::FromJsonArray<DisplayConfig>(j.Get("configs"));
            return d;
        }

    };

    template<>
    struct vdc::JsonSerializer<vdc::DisplayMap> {

        static JsonBuilder::JsonValue ToJson(const DisplayMap& m) {
            JsonBuilder::JsonValue obj = JsonBuilder::JsonValue::Object();
            obj["displays"] = vdc::ToJsonArray<Display>(m.displays);
            obj["globalTopology"] = vdc::ToJsonArray<Topology>(m.globalTopology);
            obj["mergeDisabledWins"] = JsonBuilder::JsonValue::Bool(m.mergeDisabledWins);
            return obj;
        }

        static vdc::DisplayMap FromJson(const JsonBuilder::JsonValue& j) {
            vdc::DisplayMap m; if (j.IsNull() || !j.IsObject()) return m;
            m.displays = vdc::FromJsonArray<Display>(j.Get("displays"));
            m.globalTopology = vdc::FromJsonArray<Topology>(j.Get("globalTopology"));

            const auto& md = j.Get("mergeDisabledWins"); 
            if (!md.IsNull()) m.mergeDisabledWins = md.AsBool();
            
            return m;
        }

    };


} // namespace vdc
