#include "report.hpp"

#include <cmath>
#include <cstdio>
#include <format>
#include <iostream>
#include <sstream>

namespace camprobe {

namespace {

std::string num(double v) {
    if (std::isnan(v) || std::isinf(v)) return "null";
    if (v == std::floor(v) && std::fabs(v) < 1e15) return std::format("{}", static_cast<long long>(v));
    return std::format("{:.6g}", v);
}

std::string opt(const std::optional<double>& v) { return v ? num(*v) : "-"; }
std::string optInt(const std::optional<int>& v) { return v ? std::to_string(*v) : "-"; }

std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) o += std::format("\\u{:04x}", c);
                else o += c;
        }
    }
    return o + "\"";
}

std::string jopt(const std::optional<double>& v) { return v ? num(*v) : "null"; }
std::string joptInt(const std::optional<int>& v) { return v ? std::to_string(*v) : "null"; }

template <class T, class F>
std::string jarr(const std::vector<T>& v, F f) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + f(v[i]);
    return o + "]";
}

}  // namespace

void printText(const ProbeResult& r) {
    std::cout << std::format("== {} ==  SDK {}\n", r.vendor, r.sdkAvailable ? r.sdkVersion : "not built in");
    for (const auto& e : r.errors) std::cout << "  error: " << e << "\n";
    if (r.sdkAvailable && r.cameras.empty() && r.errors.empty()) std::cout << "  no cameras found\n";
    for (const auto& c : r.cameras) {
        std::cout << std::format("\n  {}  [{}]\n", c.model, c.id);
        std::cout << std::format("    sensor {} x {}  {} um  ADC {}  {}{}\n", c.sensorWidth, c.sensorHeight,
                                 num(c.pixelUm), c.adcBits ? std::to_string(c.adcBits) + "-bit" : "not reported", c.color ? "colour" : "mono",
                                 c.bayer.empty() ? "" : " (" + c.bayer + ")");
        std::string f, b;
        for (const auto& x : c.formats) f += x + " ";
        for (int x : c.bins) b += std::to_string(x) + " ";
        std::cout << "    formats: " << f << "\n    bins: " << b << "\n";

        std::cout << "\n    Readout modes:\n";
        if (c.readoutModes.empty()) std::cout << "      (none reported)\n";
        for (const auto& m : c.readoutModes)
            std::cout << std::format("      {} {:>2}  {:<36} {}\n", m.current ? "*" : " ", m.index, m.name,
                                     m.width ? std::format("{} x {}", *m.width, optInt(m.height)) : "");
        if (!c.triggerModes.empty()) {
            std::cout << "    Trigger modes: ";
            for (const auto& t : c.triggerModes) std::cout << t << " ";
            std::cout << "\n";
        }

        std::cout << "\n    Gain landmarks:\n";
        if (c.gainLandmarks.empty()) std::cout << "      (none reported)\n";
        for (const auto& g : c.gainLandmarks)
            std::cout << std::format("      {:<34} gain {:>7}  offset {:>5}   ({})\n", g.name, opt(g.gain), opt(g.offset), g.source);

        std::cout << "\n    Controls:\n";
        std::cout << std::format("      {:<26} {:>10} {:>10} {:>8} {:>10} {:>10}  {}\n", "name", "min", "max", "step", "default", "current", "flags");
        for (const auto& k : c.controls) {
            std::string flags;
            if (!k.writable) flags += "read-only ";
            if (k.autoSupported) flags += k.autoOn ? "auto:on " : "auto:off ";
            std::cout << std::format("      {:<26} {:>10} {:>10} {:>8} {:>10} {:>10}  {}\n", k.name, num(k.min), num(k.max),
                                     k.step ? num(k.step) : "-", opt(k.defaultValue), opt(k.current), flags);
        }
        if (!c.capabilities.empty()) {
            std::cout << "\n    Capabilities: ";
            for (const auto& x : c.capabilities) std::cout << x << " ";
            std::cout << "\n";
        }
        for (const auto& n : c.notes) std::cout << "    note: " << n << "\n";
    }
    std::cout << "\n";
}

std::string toJson(const std::vector<ProbeResult>& results) {
    return jarr(results, [](const ProbeResult& r) {
        return std::format("{{\"vendor\":{},\"sdkAvailable\":{},\"sdkVersion\":{},\"errors\":{},\"cameras\":{}}}", jstr(r.vendor),
            r.sdkAvailable ? "true" : "false", jstr(r.sdkVersion), jarr(r.errors, jstr),
            jarr(r.cameras, [](const CameraReport& c) {
                return std::format(
                    "{{\"model\":{},\"id\":{},\"sensor\":[{},{}],\"pixelUm\":{},\"adcBits\":{},\"color\":{},\"bayer\":{},"
                    "\"formats\":{},\"bins\":{},\"readoutModes\":{},\"triggerModes\":{},\"gainLandmarks\":{},\"controls\":{},"
                    "\"capabilities\":{},\"notes\":{}}}",
                    jstr(c.model), jstr(c.id), c.sensorWidth, c.sensorHeight, num(c.pixelUm), c.adcBits, c.color ? "true" : "false",
                    jstr(c.bayer), jarr(c.formats, jstr), jarr(c.bins, [](int b) { return std::to_string(b); }),
                    jarr(c.readoutModes, [](const ReadoutMode& m) {
                        return std::format("{{\"index\":{},\"name\":{},\"width\":{},\"height\":{},\"current\":{}}}", m.index, jstr(m.name),
                                           joptInt(m.width), joptInt(m.height), m.current ? "true" : "false");
                    }),
                    jarr(c.triggerModes, jstr),
                    jarr(c.gainLandmarks, [](const GainLandmark& g) {
                        return std::format("{{\"name\":{},\"gain\":{},\"offset\":{},\"source\":{}}}", jstr(g.name), jopt(g.gain),
                                           jopt(g.offset), jstr(g.source));
                    }),
                    jarr(c.controls, [](const Control& k) {
                        return std::format("{{\"name\":{},\"description\":{},\"min\":{},\"max\":{},\"step\":{},\"default\":{},\"current\":{},"
                                           "\"writable\":{},\"autoSupported\":{},\"autoOn\":{}}}",
                                           jstr(k.name), jstr(k.description), num(k.min), num(k.max), num(k.step), jopt(k.defaultValue),
                                           jopt(k.current), k.writable ? "true" : "false", k.autoSupported ? "true" : "false",
                                           k.autoOn ? "true" : "false");
                    }),
                    jarr(c.capabilities, jstr), jarr(c.notes, jstr));
            }));
    });
}

}  // namespace camprobe
