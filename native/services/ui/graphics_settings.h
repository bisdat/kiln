// SPDX-License-Identifier: MIT
// kiln.ui: the player's graphics settings (kiln.json "graphics"; docs/kiln/graphics.md) and the DLSS render size they
// ask for. Plain logic (nlohmann::json for the file), no windows.h: host test tests/graphics_test.cpp.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "frame_pacer.h"
#include "nlohmann/json.hpp"

namespace kui::gfx {

// NVSDK_NGX_DLSS_Hint_Render_Preset values Kiln offers. nvngx_dlss.dll 310.9.1 (the one the game ships) has models for
// E, F, J, K, L and M (its preset table is filled at 5, 6, 10, 11, 12 and 13; docs/re/client-upscale.md); Kiln offers the
// transformer ones. 0: Kiln does not set a preset (the game's hint, else DLSS's default).
inline constexpr uint32_t kPresetDefault = 0, kPresetJ = 10, kPresetK = 11, kPresetL = 12, kPresetM = 13;
inline constexpr std::array<uint32_t, 4> kPresets = {kPresetJ, kPresetK, kPresetL, kPresetM};

// Bit 31 of a preset hint the game passes: the NGX SDK code linked into the game (enshrouded.exe 0x12f0100, run by its
// CreateFeature wrappers right before NGX's core) fills each hint the game left at 0 with NVIDIA's default for the mode
// and sets this bit: 0x8000000B (K) for DLAA, Quality, Balanced and Ultra Quality, 0x8000000D (M) for Performance,
// 0x8000000C (L) for Ultra Performance (docs/re/client-upscale.md). The preset is the bits below it.
inline constexpr uint32_t kPresetSdkDefault = 0x80000000u;
// The last preset value with a letter (O, 15): NGX's enum leaves room for these; the game's DLSS has models up to M.
inline constexpr uint32_t kPresetLast = 15;
// A hint's preset without the SDK's default bit (0: DLSS's default; above kPresetLast: not one with a letter).
inline constexpr uint32_t preset_value(uint32_t raw) { return raw & ~kPresetSdkDefault; }
// The name of a preset as DLSS logs it ("K" for 11, and for the SDK's 0x8000000B), "default" for 0, "?" for a value
// without a letter (shown on screen: never a ten-digit number).
inline std::string preset_name(uint32_t raw) {
    const uint32_t v = preset_value(raw);
    if (v == kPresetDefault) return "default";
    if (v <= kPresetLast) return std::string(1, static_cast<char>('A' + v - 1));
    return "?";
}
// kiln.log's words for the preset DLSS is created with: its name, whose it is (Kiln's, the game's, or the default the
// game's NGX SDK code filled in), `note` after a colon, and the raw value only when it is not a plain preset:
// "K (Kiln's)", "K (the game's)", "K (the game's default; raw 0x8000000B)", "unknown (the game's; raw 0x00000063)".
inline std::string preset_log(uint32_t raw, bool kilns, std::string_view note = {}) {
    const uint32_t v = preset_value(raw);
    std::string out = v <= kPresetLast ? preset_name(raw) : "unknown";
    out += kilns ? " (Kiln's" : (raw & kPresetSdkDefault) ? " (the game's default" : " (the game's";
    if (!note.empty()) out.append(": ").append(note);
    if (raw != v || v > kPresetLast) {
        char b[24];
        std::snprintf(b, sizeof b, "; raw 0x%08X", raw);
        out += b;
    }
    return out + ")";
}
// A preset Kiln offers by its name ("J".."M", any case), "default" -> 0; nullopt for anything else.
inline std::optional<uint32_t> preset_from_name(std::string_view s) {
    if (s.size() == 1) {
        const char c = static_cast<char>(s[0] & ~0x20);
        for (uint32_t p : kPresets)
            if (c == static_cast<char>('A' + p - 1)) return p;
        return std::nullopt;
    }
    if (s.size() == 7) {
        std::string l(s);
        for (auto& c : l) c = static_cast<char>(c | 0x20);
        if (l == "default") return kPresetDefault;
    }
    return std::nullopt;
}

// How the DLSS render size is chosen (kiln.json graphics.dlssScale).
enum class DlssScale : uint32_t { game = 0, per_mode = 1, custom = 2 };
inline const char* scale_name(DlssScale s) { return s == DlssScale::per_mode ? "perMode" : s == DlssScale::custom ? "custom" : "game"; }

// The render scale's range, as a share of the output's width and height. DLSS's own Ultra Performance is a third.
inline constexpr float kMinScale = 0.33f, kMaxScale = 1.0f;
// Quality, Balanced, Performance, Ultra Performance: DLSS's own scales (the defaults of the per-mode setting).
inline constexpr std::array<float, 4> kDlssScales = {0.667f, 0.58f, 0.5f, 0.333f};
inline constexpr std::array<const char*, 4> kModeKeys = {"dlssQualityScale", "dlssBalancedScale", "dlssPerformanceScale",
                                                         "dlssUltraPerformanceScale"};
inline float clamp_scale(double v, float fallback) {
    if (!std::isfinite(v)) return fallback;
    return static_cast<float>(std::fmin(std::fmax(v, kMinScale), kMaxScale));
}

// What runs behind the game's FSR 3 option (kiln.json graphics.upscaler, KILN_UI_UPSCALER_*; upscale_backend.h Choice).
enum class Upscaler : uint32_t { game = 0, amd = 1, xess = 2 };
inline const char* upscaler_key(Upscaler u) { return u == Upscaler::amd ? "amd" : u == Upscaler::xess ? "xess" : "game"; }
inline const char* upscaler_name(Upscaler u) {
    return u == Upscaler::amd ? "AMD FSR" : u == Upscaler::xess ? "Intel XeSS" : "the game's own FSR 3";
}
// kiln.json graphics.amdUpscaler (KILN_UI_AMD_UPSCALER_*): auto (FSR 3.1 until FSR 4 is confirmed in this game), fsr3
// (FSR 3.1), fsr4 (AMD's own choice: FSR 4 where it runs; experimental: dark scenes too dark in 29.19 betas 1 and 3).
enum class AmdPin : uint32_t { automatic = 0, fsr3 = 1, fsr4 = 2 };

struct Settings {
    uint32_t fps_cap = 0;  // clamp_fps: 0 off, 20..500
    uint32_t dlss_preset = kPresetDefault;
    DlssScale dlss_scale = DlssScale::game;
    float custom_scale = 0.67f;
    std::array<float, 4> mode_scale = kDlssScales;  // Quality, Balanced, Performance, Ultra Performance
    bool readout = false;
    Upscaler upscaler = Upscaler::game;
    AmdPin amd_pin = AmdPin::automatic;

    // Kiln's upscaler swap is asked for (its hooks go in only when this is so at the game's start).
    bool wants_swap() const { return upscaler != Upscaler::game; }
    // Something asks for Kiln's hooks on DLSS: a DLSS setting off its default, or the readout (which reads what DLSS runs at).
    bool dlss_set() const { return dlss_preset != kPresetDefault || dlss_scale != DlssScale::game; }
    bool wants_ngx() const { return dlss_set() || readout; }
    bool operator==(const Settings&) const = default;
};

// Every value in its range (a value from managed code or the file).
inline Settings clamped(Settings s) {
    s.fps_cap = clamp_fps(s.fps_cap);
    bool known = s.dlss_preset == kPresetDefault;
    for (uint32_t p : kPresets) known |= s.dlss_preset == p;
    if (!known) s.dlss_preset = kPresetDefault;
    if (static_cast<uint32_t>(s.dlss_scale) > 2) s.dlss_scale = DlssScale::game;
    s.custom_scale = clamp_scale(s.custom_scale, 0.67f);
    for (size_t i = 0; i < 4; ++i) s.mode_scale[i] = clamp_scale(s.mode_scale[i], kDlssScales[i]);
    if (static_cast<uint32_t>(s.upscaler) > 2) s.upscaler = Upscaler::game;
    if (static_cast<uint32_t>(s.amd_pin) > 2) s.amd_pin = AmdPin::automatic;
    return s;
}

// kiln.json's "graphics" section (the whole document's root is given). Missing keys keep their defaults; a value that
// is not usable keeps the default and adds a line to `warnings`.
inline Settings parse_settings(const nlohmann::json& root, std::vector<std::string>* warnings = nullptr) {
    Settings s;
    auto warn = [&](const std::string& w) { if (warnings) warnings->push_back(w); };
    if (!root.is_object()) return s;
    const auto it = root.find("graphics");
    if (it == root.end() || it->is_null()) return s;
    if (!it->is_object()) {
        warn("\"graphics\" is not an object: Kiln's graphics settings stay at their defaults");
        return s;
    }
    const nlohmann::json& g = *it;
    auto number = [&](const char* key, double* out) {
        const auto v = g.find(key);
        if (v == g.end() || v->is_null()) return false;
        if (!v->is_number()) {
            warn(std::string("graphics.") + key + " is not a number: its default is used");
            return false;
        }
        *out = v->get<double>();
        return true;
    };
    double d = 0;
    if (number("fpsCap", &d)) {
        s.fps_cap = std::isfinite(d) ? clamp_fps(static_cast<int64_t>(std::llround(std::fmin(std::fmax(d, -1.0), 100000.0)))) : 0;
        if (std::isfinite(d) && d > 0 && (d < kMinFps || d > kMaxFps))
            warn("graphics.fpsCap " + std::to_string(static_cast<int64_t>(d)) + " is outside 20..500: " + std::to_string(s.fps_cap) + " is used");
    }
    if (const auto v = g.find("dlssPreset"); v != g.end() && !v->is_null()) {
        const auto p = v->is_string() ? preset_from_name(v->get<std::string>()) : std::nullopt;
        if (p) s.dlss_preset = *p;
        else warn("graphics.dlssPreset is not default, J, K, L or M: default is used");
    }
    if (const auto v = g.find("dlssScale"); v != g.end() && !v->is_null()) {
        const std::string m = v->is_string() ? v->get<std::string>() : std::string();
        if (m == "game") s.dlss_scale = DlssScale::game;
        else if (m == "perMode") s.dlss_scale = DlssScale::per_mode;
        else if (m == "custom") s.dlss_scale = DlssScale::custom;
        else warn("graphics.dlssScale is not game, perMode or custom: game is used");
    }
    if (number("dlssRenderScale", &d)) s.custom_scale = clamp_scale(d, 0.67f);
    for (size_t i = 0; i < 4; ++i)
        if (number(kModeKeys[i], &d)) s.mode_scale[i] = clamp_scale(d, kDlssScales[i]);
    if (const auto v = g.find("readout"); v != g.end() && !v->is_null()) {
        if (v->is_boolean()) s.readout = v->get<bool>();
        else warn("graphics.readout is not true or false: off");
    }
    if (const auto v = g.find("upscaler"); v != g.end() && !v->is_null()) {
        const std::string m = v->is_string() ? v->get<std::string>() : std::string();
        if (m == "game") s.upscaler = Upscaler::game;
        else if (m == "amd") s.upscaler = Upscaler::amd;
        else if (m == "xess") s.upscaler = Upscaler::xess;
        else warn("graphics.upscaler is not game, amd or xess: game is used");
    }
    if (const auto v = g.find("amdUpscaler"); v != g.end() && !v->is_null()) {
        const std::string m = v->is_string() ? v->get<std::string>() : std::string();
        if (m == "auto") s.amd_pin = AmdPin::automatic;
        else if (m == "fsr3") s.amd_pin = AmdPin::fsr3;
        else if (m == "fsr4") s.amd_pin = AmdPin::fsr4;
        else warn("graphics.amdUpscaler is not auto, fsr3 or fsr4: auto is used");
    }
    return s;
}

// NVSDK_NGX_PerfQuality_Value: the game's DLSS mode.
enum PerfQuality : int { kPerformance = 0, kBalanced = 1, kQuality = 2, kUltraPerformance = 3, kUltraQuality = 4, kDlaa = 5 };
inline const char* mode_name(int q) {
    switch (q) {
        case kPerformance: return "Performance";
        case kBalanced: return "Balanced";
        case kQuality: return "Quality";
        case kUltraPerformance: return "Ultra Performance";
        case kUltraQuality: return "Ultra Quality";
        case kDlaa: return "DLAA";
        default: return "?";
    }
}
// Index into Settings::mode_scale, -1 for a mode the per-mode setting does not cover (Ultra Quality, DLAA).
inline int mode_index(int q) {
    switch (q) {
        case kQuality: return 0;
        case kBalanced: return 1;
        case kPerformance: return 2;
        case kUltraPerformance: return 3;
        default: return -1;
    }
}

struct Size {
    uint32_t w = 0, h = 0;
    bool operator==(const Size&) const = default;
};

// The render scale the setting asks for in this mode; nullopt: DLSS's own (game, DLAA, a mode per-mode leaves alone).
inline std::optional<float> wanted_scale(const Settings& s, int quality) {
    if (quality == kDlaa) return std::nullopt;  // DLAA renders at the output's size, as the game asks
    if (s.dlss_scale == DlssScale::custom) return s.custom_scale;
    if (s.dlss_scale == DlssScale::per_mode) {
        const int i = mode_index(quality);
        if (i >= 0) return s.mode_scale[static_cast<size_t>(i)];
    }
    return std::nullopt;
}

// After DLSS's optimal-settings answer: the render size Kiln sets instead of DLSS's (`dlss`; nullopt: keep it), and
// whether DLSS's range for this mode limited the player's scale. `output`: the size asked for; `min`/`max`: DLSS's range
// for this output and mode (zero: unknown, not clamped; DLSS renders nothing outside it). A size within a pixel of DLSS's
// own is DLSS's (the per-mode defaults are DLSS's scales, rounded). DLSS's own size 0x0: DLSS does not offer the mode
// (Ultra Quality, which the game asks about but neither offers nor uses): nothing is set for it.
struct RenderPick {
    std::optional<Size> set;
    bool limited = false;  // the wanted size was outside DLSS's range: the nearest size in it is used
    bool offered = true;   // DLSS gave a render size for this mode (false: 0x0, the mode is not offered)
    Size wanted;           // output x scale, before DLSS's range (zero: Kiln wants none)
};
inline RenderPick pick_render_size(const Settings& s, int quality, Size output, Size dlss, Size min, Size max) {
    RenderPick p;
    p.offered = dlss.w && dlss.h;
    const auto scale = wanted_scale(s, quality);
    if (!scale || !output.w || !output.h || !p.offered) return p;
    auto want = [](uint32_t out, float k) {
        return static_cast<uint32_t>(std::clamp<int64_t>(std::llround(static_cast<double>(out) * k), 1, out));
    };
    auto fit = [](uint32_t v, uint32_t lo, uint32_t hi) { return lo && hi && lo <= hi ? std::clamp(v, lo, hi) : v; };
    p.wanted = {want(output.w, *scale), want(output.h, *scale)};
    const Size r{fit(p.wanted.w, min.w, max.w), fit(p.wanted.h, min.h, max.h)};
    p.limited = !(r == p.wanted);
    const auto within_one = [](uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) <= 1; };
    if (!(within_one(r.w, dlss.w) && within_one(r.h, dlss.h))) p.set = r;
    return p;
}
inline std::optional<Size> render_size(const Settings& s, int quality, Size output, Size dlss, Size min, Size max) {
    return pick_render_size(s, quality, output, dlss, min, max).set;
}

}  // namespace kui::gfx
