// SPDX-License-Identifier: MIT
// kiln.ui: how the game's FSR 3 inputs become Intel XeSS's (upscale_xess.cpp; docs/kiln/graphics.md). Plain logic over
// Vulkan's headers (no windows.h): host test tests/xess_test.cpp.
//
// What the game hands its FSR 3 (B0, on the real client) and what XeSS takes:
//  colour      R16G16B16A16_SFLOAT, linear HDR          -> colorTexture (HDR is XeSS's default: no LDR flag)
//  motion      R16G16_SNORM, UV units, current->previous,
//              +y down, no jitter                        -> velocityTexture at the input size, with
//                                                           xessSetVelocityScale = the game's MV scale (the render size),
//                                                           which turns UV into the input pixels XeSS wants
//  depth       R32_SFLOAT, reverse-Z                     -> depthTexture with XESS_INIT_FLAG_INVERTED_DEPTH (low-res MVs
//                                                           are dilated with it)
//  reactive    R8_UNORM (sparks, particles)             -> responsivePixelMaskTexture (XeSS clamps it at 0.8)
//  transparency R8_UNORM                                 -> not used: XeSS has no such input
//  output      R16G16B16A16_SFLOAT, GENERAL              -> outputTexture
//  jitter      render pixels, FSR 3.1's sign             -> jitterOffset (XeSS composes it into the projection the same
//                                                           way: +2Jx/W, -2Jy/H)
//  pre-exposure  the game's auto-exposure                 -> exposureScale: see Exposure below
//  reset       the core's                                -> resetHistory
// The quality preset follows the game's ratio (FSR 3's presets: 1.5, 1.7, 2.0, 3.0, 1.0 native) to the XeSS preset whose
// dynamic range (1.0x up to its ratio) holds it; the game's render size is kept, so its jitter phases (8 * ratio^2),
// mip bias and frame budget stay as they are.
// Test knobs (kiln/control/xess, one per line, for the real-client check; nothing reads them in normal play): flips of
// the jitter and motion axes, the exposure mode, the responsive mask, the motion-vector copy, XeSS's own log level.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "device_merge.h"
#include "upscale_backend.h"
#include "xess_abi.h"

namespace kui::xess {

// ---- quality --------------------------------------------------------------------------------------------------------
struct Preset {
    Quality quality;
    float ratio;  // output / input, XeSS 1.3+
    const char* name;
};
inline constexpr Preset kPresets[] = {
    {kNativeAA, 1.0f, "Native Anti-Aliasing"}, {kUltraQualityPlus, 1.3f, "Ultra Quality Plus"}, {kUltraQuality, 1.5f, "Ultra Quality"},
    {kQuality, 1.7f, "Quality"},               {kBalanced, 2.0f, "Balanced"},                  {kPerformance, 2.3f, "Performance"},
    {kUltraPerformance, 3.0f, "Ultra Performance"},
};
inline const Preset& preset_of(Quality q) {
    for (const auto& p : kPresets)
        if (p.quality == q) return p;
    return kPresets[0];
}
// The XeSS preset for the game's ratio: the smallest whose ratio is at least the game's (2% slack for rounding).
// `beyond`: the game's ratio is past Ultra Performance's 3.0 (XeSS then runs outside its range).
inline Quality quality_for_ratio(float ratio, bool* beyond = nullptr) {
    if (beyond) *beyond = false;
    if (!std::isfinite(ratio) || ratio <= 1.02f) return kNativeAA;
    for (const auto& p : kPresets)
        if (p.ratio + 0.02f >= ratio) return p.quality;
    if (beyond) *beyond = true;
    return kUltraPerformance;
}
// The ratio a CreateInfo means: its own, else output / render width (and height, the larger).
inline float ratio_of(const upscale::CreateInfo& ci) {
    if (ci.ratio > 0 && std::isfinite(ci.ratio)) return ci.ratio;
    float r = 1;
    if (ci.max_render_width) r = std::fmax(r, static_cast<float>(ci.output_width) / static_cast<float>(ci.max_render_width));
    if (ci.max_render_height) r = std::fmax(r, static_cast<float>(ci.output_height) / static_cast<float>(ci.max_render_height));
    return r;
}

// ---- the test knobs -------------------------------------------------------------------------------------------------
enum class Exposure : uint32_t {
    one = 0,      // exposureScale 1: the colour is the game's exposed signal (its pre-exposure is its auto-exposure)
    inverse = 1,  // exposureScale 1 / pre-exposure: XeSS sees the signal before the game's pre-exposure (as FSR 3 does)
    automatic = 2,  // XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE: XeSS measures it (costs GPU time)
};
inline const char* exposure_name(Exposure e) { return e == Exposure::inverse ? "inverse" : e == Exposure::automatic ? "auto" : "one"; }

struct Tuning {
    float jitter_x = 1, jitter_y = 1;  // -1 flips the axis
    float mv_x = 1, mv_y = 1;
    Exposure exposure = Exposure::one;
    bool mask = true;                  // the game's reactive mask as XeSS's responsive pixel mask
    bool motion_copy = true;           // the motion vectors copied into R16G16_SFLOAT first (where the build can)
    int32_t log_level = kLogWarning;   // XeSS's own log callback: this level and up
    std::vector<std::string> notes;    // lines not understood
    bool tuned() const {
        return jitter_x != 1 || jitter_y != 1 || mv_x != 1 || mv_y != 1 || exposure != Exposure::one || !mask || !motion_copy ||
               log_level != kLogWarning;
    }
    std::string describe() const {
        std::string s = "jitter " + std::string(jitter_x < 0 ? "-x" : "+x") + (jitter_y < 0 ? " -y" : " +y") + ", motion " +
                        (mv_x < 0 ? "-x" : "+x") + (mv_y < 0 ? " -y" : " +y") + ", exposure " + exposure_name(exposure) + ", mask " +
                        (mask ? "on" : "off") + ", motion copy " + (motion_copy ? "on" : "off");
        return s;
    }
};

// kiln/control/xess: "jitter-x flip", "jitter-y flip", "motion-x flip", "motion-y flip", "exposure one|inverse|auto",
// "mask on|off", "motion-copy on|off", "log debug|info|warning|error"; '#' starts a comment.
inline Tuning parse_tuning(std::string_view text) {
    Tuning t;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string line(text.substr(at, end - at));
        at = end + 1;
        if (const size_t h = line.find('#'); h != std::string::npos) line.resize(h);
        std::vector<std::string> w;
        std::string cur;
        for (char c : line) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '=') {
                if (!cur.empty()) w.push_back(cur), cur.clear();
            } else {
                cur.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
            }
        }
        if (!cur.empty()) w.push_back(cur);
        if (w.empty()) continue;
        const std::string v = w.size() > 1 ? w[1] : "";
        const bool flip = v == "flip" || v == "-1";
        if (w[0] == "jitter-x" && (flip || v == "1")) t.jitter_x = flip ? -1.f : 1.f;
        else if (w[0] == "jitter-y" && (flip || v == "1")) t.jitter_y = flip ? -1.f : 1.f;
        else if (w[0] == "motion-x" && (flip || v == "1")) t.mv_x = flip ? -1.f : 1.f;
        else if (w[0] == "motion-y" && (flip || v == "1")) t.mv_y = flip ? -1.f : 1.f;
        else if (w[0] == "exposure" && v == "one") t.exposure = Exposure::one;
        else if (w[0] == "exposure" && v == "inverse") t.exposure = Exposure::inverse;
        else if (w[0] == "exposure" && v == "auto") t.exposure = Exposure::automatic;
        else if (w[0] == "mask" && (v == "on" || v == "off")) t.mask = v == "on";
        else if (w[0] == "motion-copy" && (v == "on" || v == "off")) t.motion_copy = v == "on";
        else if (w[0] == "log" && v == "debug") t.log_level = kLogDebug;
        else if (w[0] == "log" && v == "info") t.log_level = kLogInfo;
        else if (w[0] == "log" && v == "warning") t.log_level = kLogWarning;
        else if (w[0] == "log" && v == "error") t.log_level = kLogError;
        else t.notes.push_back(line);
    }
    return t;
}

// ---- creation -------------------------------------------------------------------------------------------------------
inline uint32_t init_flags(const upscale::CreateInfo& ci, const Tuning& t) {
    uint32_t f = 0;
    if (ci.depth_inverted) f |= kFlagInvertedDepth;
    if (!ci.hdr) f |= kFlagLdrInputColor;
    if (t.mask) f |= kFlagResponsivePixelMask;
    if (t.exposure == Exposure::automatic && ci.hdr) f |= kFlagAutoExposure;
    return f;
}
inline std::string flags_text(uint32_t f) {
    std::string s;
    auto add = [&](uint32_t bit, const char* n) {
        if (f & bit) s += s.empty() ? n : std::string("|") + n;
    };
    add(kFlagHighResMv, "HIGH_RES_MV");
    add(kFlagInvertedDepth, "INVERTED_DEPTH");
    add(kFlagExposureScaleTexture, "EXPOSURE_SCALE_TEXTURE");
    add(kFlagResponsivePixelMask, "RESPONSIVE_PIXEL_MASK");
    add(kFlagNdcVelocity, "USE_NDC_VELOCITY");
    add(kFlagLdrInputColor, "LDR_INPUT_COLOR");
    add(kFlagJitteredMv, "JITTERED_MV");
    add(kFlagAutoExposure, "ENABLE_AUTOEXPOSURE");
    return s.empty() ? "NONE" : s;
}

// What a CreateInfo must have for XeSS. False: `why` (English).
inline bool check_create(const upscale::CreateInfo& ci, std::string* why) {
    auto no = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (!ci.output_width || !ci.output_height || !ci.max_render_width || !ci.max_render_height)
        return no("the FSR 3 pass has no size (render " + std::to_string(ci.max_render_width) + "x" + std::to_string(ci.max_render_height) +
                  ", output " + std::to_string(ci.output_width) + "x" + std::to_string(ci.output_height) + ")");
    if (ci.max_render_width > ci.output_width || ci.max_render_height > ci.output_height)
        return no("XeSS upscales only: the render size " + std::to_string(ci.max_render_width) + "x" + std::to_string(ci.max_render_height) +
                  " is larger than the output " + std::to_string(ci.output_width) + "x" + std::to_string(ci.output_height));
    if (ci.output_width > 16384 || ci.output_height > 16384) return no("the output is larger than 16384 pixels a side");
    if (ci.color_format != VK_FORMAT_UNDEFINED && ci.output_format != VK_FORMAT_UNDEFINED && ci.color_format != ci.output_format)
        return no("XeSS writes the output in the colour's format; the game's output has another one");
    return true;
}

// ---- one frame ------------------------------------------------------------------------------------------------------
inline VkImageAspectFlags aspect_of(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT;  // XeSS: the depth aspect only
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
inline ImageInfo image_info(const upscale::Texture& t) {
    ImageInfo i{};
    i.view = t.view;
    i.image = t.image;
    i.range = {aspect_of(t.format), 0, 1, 0, 1};
    i.format = t.format;
    i.width = t.width;
    i.height = t.height;
    return i;
}

// Whether a frame is one XeSS can take, for a context made for `output` (its init size) with `flags`. False: `why`.
inline bool check_frame(const upscale::Frame& f, uint32_t init_w, uint32_t init_h, uint32_t flags, std::string* why) {
    auto no = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    const upscale::Texture* need[] = {&f.color, &f.motion, &f.depth, &f.output};
    const char* names[] = {"colour", "motion vectors", "depth", "output"};
    for (int i = 0; i < 4; ++i)
        if (!need[i]->present() || !need[i]->view || !need[i]->width || !need[i]->height) return no(std::string("the ") + names[i] + " texture is missing");
    if ((flags & kFlagResponsivePixelMask) && (!f.reactive.present() || !f.reactive.view)) return no("the reactive mask is missing");
    if (!f.cb) return no("no command buffer");
    if (!f.render_width || !f.render_height) return no("the render size is 0");
    if (f.render_width > f.color.width || f.render_height > f.color.height)
        return no("the render size " + std::to_string(f.render_width) + "x" + std::to_string(f.render_height) + " is larger than the colour texture");
    if (f.render_width > f.motion.width || f.render_height > f.motion.height || f.render_width > f.depth.width || f.render_height > f.depth.height)
        return no("the motion or depth texture is smaller than the render size");
    const uint32_t ow = f.output_width ? f.output_width : init_w, oh = f.output_height ? f.output_height : init_h;
    if (ow > f.output.width || oh > f.output.height) return no("the output texture is smaller than the output size");
    if (ow != init_w || oh != init_h)
        return no("the output size " + std::to_string(ow) + "x" + std::to_string(oh) + " is not the " + std::to_string(init_w) + "x" +
                  std::to_string(init_h) + " XeSS was set up for");
    if (f.color.format != f.output.format) return no("the output's format is not the colour's");
    if (f.output.layout != VK_IMAGE_LAYOUT_UNDEFINED && f.output.layout != VK_IMAGE_LAYOUT_GENERAL) return no("the output is not in GENERAL");
    return true;
}

// The exposure scale this frame (Tuning::exposure; automatic: XeSS's own, the value is ignored).
inline float exposure_scale(const upscale::Frame& f, const Tuning& t) {
    if (t.exposure != Exposure::inverse) return 1.0f;
    const float pe = f.pre_exposure;
    return std::isfinite(pe) && pe > 1e-6f ? 1.0f / pe : 1.0f;
}

inline ExecuteParams execute_params(const upscale::Frame& f, const Tuning& t, uint32_t flags, bool reset) {
    ExecuteParams p{};
    p.color = image_info(f.color);
    p.velocity = image_info(f.motion);
    p.depth = image_info(f.depth);
    if (flags & kFlagResponsivePixelMask) p.responsive = image_info(f.reactive);
    p.output = image_info(f.output);
    auto clamp_half = [](float v) { return std::isfinite(v) ? std::fmin(std::fmax(v, -0.5f), 0.5f) : 0.0f; };
    p.jitter_x = clamp_half(f.jitter_x * t.jitter_x);
    p.jitter_y = clamp_half(f.jitter_y * t.jitter_y);
    p.exposure_scale = exposure_scale(f, t);
    p.reset_history = reset || f.reset ? 1u : 0u;
    p.input_width = f.render_width;
    p.input_height = f.render_height;
    return p;
}

// The velocity scale: the game's MV scale (UV -> render pixels), with the test flips. 0 or not finite: the render size.
inline void velocity_scale(const upscale::Frame& f, const Tuning& t, float* x, float* y) {
    const float sx = std::isfinite(f.mv_scale_x) && f.mv_scale_x != 0 ? f.mv_scale_x : static_cast<float>(f.render_width);
    const float sy = std::isfinite(f.mv_scale_y) && f.mv_scale_y != 0 ? f.mv_scale_y : static_cast<float>(f.render_height);
    *x = sx * t.mv_x;
    *y = sy * t.mv_y;
}

// ---- device needs ---------------------------------------------------------------------------------------------------
// What the SDK's guide says XeSS needs on Vulkan, for when libxess.dll is not loaded yet: Vulkan 1.1,
// VK_EXT_mutable_descriptor_type with mutableDescriptorType and shaderStorageImageWriteWithoutFormat (required);
// shaderInt8 and shaderIntegerDotProduct where the device has them (XeSS asks for them "if supported").
inline upscale::InstanceNeeds documented_instance_needs() {
    upscale::InstanceNeeds n;
    n.min_api_version = VK_API_VERSION_1_1;
    return n;
}
inline upscale::DeviceNeeds documented_device_needs() {
    upscale::DeviceNeeds n;
    n.want_extension("VK_EXT_mutable_descriptor_type", true);
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    n.want(f2, true);
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT m{};
    m.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT;
    m.mutableDescriptorType = VK_TRUE;
    n.want(m, true);
    n.want_extension("VK_KHR_shader_float16_int8");
    VkPhysicalDeviceShaderFloat16Int8Features i8{};
    i8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
    i8.shaderInt8 = VK_TRUE;
    n.want(i8);
    n.want_extension("VK_KHR_shader_integer_dot_product");
    VkPhysicalDeviceShaderIntegerDotProductFeatures dp{};
    dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES;
    dp.shaderIntegerDotProduct = VK_TRUE;
    n.want(dp);
    return n;
}

// XeSS's own answers (xessVKGetRequiredDeviceExtensions / Features) as DeviceNeeds: all required (XeSS lists only
// what the device has, and what it lists it must get). `chain`: the struct chain XeSS returned (its memory: only read).
// False: a struct of a type Kiln does not know is in it (`why` names it); nothing Kiln cannot copy is passed on.
inline bool needs_from_xess(uint32_t ext_count, const char* const* exts, const void* chain, upscale::DeviceNeeds* out, std::string* why) {
    for (uint32_t i = 0; i < ext_count && exts; ++i)
        if (exts[i] && *exts[i]) out->want_extension(exts[i], true);
    int guard = 0;
    for (auto* p = static_cast<const VkBaseInStructure*>(chain); p; p = p->pNext) {
        if (++guard > 256) {
            if (why) *why = "XeSS's feature chain does not end";
            return false;
        }
        const device_merge::StructInfo* s = device_merge::find_struct(p->sType);
        if (!s) {
            if (why) *why = "XeSS asks for a Vulkan feature struct Kiln does not know (sType " + std::to_string(static_cast<uint32_t>(p->sType)) + ")";
            return false;
        }
        upscale::FeatureNeed f;
        f.bytes.assign(reinterpret_cast<const uint8_t*>(p), reinterpret_cast<const uint8_t*>(p) + s->size);
        reinterpret_cast<VkBaseOutStructure*>(f.bytes.data())->pNext = nullptr;
        f.required = true;
        out->features.push_back(std::move(f));
    }
    return true;
}

// What of `needs` (its required part, or all with `all`) the created device does not have: extension names and
// "Struct.field" names. A feature counts under either of its names (core or promoted extension struct).
inline std::vector<std::string> missing_on_device(const upscale::VkContext& vk, const upscale::DeviceNeeds& needs, bool all = false) {
    std::vector<std::string> out;
    for (const auto& e : needs.extensions)
        if ((all || e.required) && !vk.has_extension(e.name.c_str())) out.push_back(e.name);
    for (const auto& f : needs.features) {
        if (!all && !f.required) continue;
        const VkStructureType t = device_merge::type_of(f.bytes.data(), f.bytes.size());
        const device_merge::StructInfo* s = device_merge::find_struct(t);
        const size_t end = s ? std::min<size_t>(s->size, f.bytes.size()) : f.bytes.size();
        for (uint32_t o = device_merge::kHeader; o + sizeof(VkBool32) <= end; o += sizeof(VkBool32)) {
            if (!device_merge::bool_at(f.bytes.data(), f.bytes.size(), o)) continue;
            bool on = vk.has_feature(t, o);
            if (!on)
                if (const device_merge::Promoted* p = device_merge::find_promoted(t, o))
                    on = vk.has_feature(p->core, p->core_offset) || (p->ext && vk.has_feature(p->ext, p->ext_offset));
            if (!on) out.push_back(device_merge::field_name(t, o));
        }
    }
    return out;
}

inline std::string join(const std::vector<std::string>& v, size_t max = 4) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < max; ++i) s += (i ? ", " : "") + v[i];
    if (v.size() > max) s += " and " + std::to_string(v.size() - max) + " more";
    return s;
}

}  // namespace kui::xess
