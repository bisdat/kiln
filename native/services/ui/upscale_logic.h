// SPDX-License-Identifier: MIT
// kiln.ui: the upscaler swap's plain logic (upscale.cpp; docs/kiln/graphics.md "Upscaler behind the game's FSR 3"):
// reading the game's FSR description, Kiln's render command, the frame time, when history is dropped and contexts are
// made again, which textures a backend is given, and what the status says. No windows.h, no Vulkan calls: host test
// tests/upscale_test.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "upscale_backend.h"

namespace kui::upscale {

// ---- the game's FSR description (Fsr3Dispatch's r8) and input struct (PassRecord's r8) ----------------------------
// Field offsets of the description, captured by sigdb (client.d/upscale.json, upscale/client.desc.*).
struct DescLayout {
    int64_t jitter = 0x128, prev_render = 0x118, render = 0x110, upscale = 0x120, reset = 0x138, fov = 0x100, near_plane = 0xf8,
            far_plane = 0xfc, exposure = 0x13c, mv_scale = 0x130, frame_ns = 0x108, sharpness = 0x140;
    // the largest offset + its size: how many bytes of the description are read
    size_t extent() const {
        const int64_t m = std::max({jitter + 8, prev_render + 8, render + 8, upscale + 8, reset + 1, fov + 4, near_plane + 4, far_plane + 4,
                                    exposure + 4, mv_scale + 8, frame_ns + 8, sharpness + 4});
        return static_cast<size_t>(m);
    }
    // every offset inside a description of `size` bytes (the game's is 0x148), none negative
    bool sane(size_t size = kDescBytes) const {
        const int64_t all[] = {jitter, prev_render, render, upscale, reset, fov, near_plane, far_plane, exposure, mv_scale, frame_ns, sharpness};
        for (int64_t v : all)
            if (v < 0) return false;
        return extent() <= size;
    }
    static constexpr size_t kDescBytes = 0x148;
};

// The six resources: their place in the description (entries of 0x28 bytes: a kind byte, then the 16-byte texture
// handle at +8) and in the input struct (16-byte handles from +0x10). Fixed in code, checked every frame: the handles
// in both must be the same (a game build that moved them turns the swap off with reason INPUTS).
enum Role : int { kColor = 0, kDepth = 1, kMotion = 2, kOutput = 3, kReactive = 4, kTransparency = 5, kRoles = 6 };
inline constexpr int kDescRes[kRoles] = {0x00, 0x28, 0x50, 0x78, 0xA0, 0xC8};
inline constexpr int kDescHandle = 8;
inline constexpr int kInHandle[kRoles] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
inline constexpr int kHandleBytes = 16;
inline constexpr size_t kInBytes = 0xA8;
inline const char* role_name(int r) {
    static const char* const n[kRoles] = {"colour", "depth", "motion vectors", "output", "reactive mask", "transparency mask"};
    return r >= 0 && r < kRoles ? n[r] : "?";
}

template <class T>
inline T rd(const uint8_t* p, int64_t off) {
    T v{};
    std::memcpy(&v, p + off, sizeof v);
    return v;
}

// The description's values (the textures come separately, from their handles).
struct DescValues {
    float jitter_x = 0, jitter_y = 0;
    uint32_t prev_w = 0, prev_h = 0, render_w = 0, render_h = 0, upscale_w = 0, upscale_h = 0;
    bool reset = false;
    float fov = 0, plane_a = 0, plane_b = 0, exposure = 0, mv_x = 0, mv_y = 0, sharpness = 0;
    int64_t frame_ns = 0;
};
inline DescValues read_desc(const uint8_t* d, const DescLayout& L) {
    DescValues v;
    v.jitter_x = rd<float>(d, L.jitter);
    v.jitter_y = rd<float>(d, L.jitter + 4);
    v.prev_w = rd<uint32_t>(d, L.prev_render);
    v.prev_h = rd<uint32_t>(d, L.prev_render + 4);
    v.render_w = rd<uint32_t>(d, L.render);
    v.render_h = rd<uint32_t>(d, L.render + 4);
    v.upscale_w = rd<uint32_t>(d, L.upscale);
    v.upscale_h = rd<uint32_t>(d, L.upscale + 4);
    v.reset = rd<uint8_t>(d, L.reset) != 0;
    v.fov = rd<float>(d, L.fov);
    v.plane_a = rd<float>(d, L.near_plane);
    v.plane_b = rd<float>(d, L.far_plane);
    v.exposure = rd<float>(d, L.exposure);
    v.mv_x = rd<float>(d, L.mv_scale);
    v.mv_y = rd<float>(d, L.mv_scale + 4);
    v.sharpness = rd<float>(d, L.sharpness);
    v.frame_ns = rd<int64_t>(d, L.frame_ns);
    return v;
}

// The input struct and the description hold the same six handles (and the same jitter and sizes): the layout guard.
// Returns nullptr when they agree, else what differs. `in` may be null (PassRecord was not seen this frame): handles
// only from the description then, nothing to compare.
inline const char* desc_matches_input(const uint8_t* in, const uint8_t* desc, const DescLayout& L) {
    if (!in) return nullptr;
    for (int r = 0; r < kRoles; ++r)
        if (std::memcmp(in + kInHandle[r], desc + kDescRes[r] + kDescHandle, kHandleBytes) != 0) return role_name(r);
    if (std::memcmp(in + 0x00, desc + L.jitter, 8) != 0) return "jitter";
    if (std::memcmp(in + 0x70, desc + L.render, 8) != 0) return "render size";
    if (std::memcmp(in + 0x80, desc + L.upscale, 8) != 0) return "output size";
    return nullptr;
}

// ---- Kiln's render command in the game's stream ---------------------------------------------------------------------
// The game's header (u16 size, u8 type) and Kiln's fields. Type 0xF0 is above the game's last command type (sigdb
// upscale/client.cmd.last, 0x2e): the game's executor skips it, Kiln's executor hook runs it.
struct KCmd {
    uint16_t size;
    uint8_t type;
    uint8_t kind;
    uint32_t seq;
    uint64_t magic;
    uint64_t slot;
    uint64_t check;  // magic ^ seq ^ slot: a stray 0xF0 from anything else is not Kiln's
};
static_assert(sizeof(KCmd) == 32, "KCmd is the 32 bytes Kiln writes into the game's command chunk");
inline constexpr uint8_t kCmdType = 0xF0;
inline constexpr uint64_t kCmdMagic = 0x3250435355504C4BULL;  // "KLPUSCP2" in memory order
enum CmdKind : uint8_t { kUpscale = 1 };
inline KCmd make_cmd(uint8_t kind, uint32_t seq, uint64_t slot) {
    return KCmd{static_cast<uint16_t>(sizeof(KCmd)), kCmdType, kind, seq, kCmdMagic, slot, kCmdMagic ^ seq ^ slot};
}
// A command at `p` of `avail` readable bytes is Kiln's (type, size, magic and check).
inline bool is_kiln_cmd(const uint8_t* p, size_t avail, KCmd* out) {
    if (!p || avail < sizeof(KCmd) || p[2] != kCmdType) return false;
    KCmd c;
    std::memcpy(&c, p, sizeof c);
    if (c.size != sizeof(KCmd) || c.magic != kCmdMagic || c.check != (kCmdMagic ^ c.seq ^ c.slot)) return false;
    if (out) *out = c;
    return true;
}
// Room in a chunk {next, write, end, count}: Kiln's command fits, or a new chunk is needed.
inline bool chunk_has_room(uintptr_t write, uintptr_t end) { return end >= write && end - write >= sizeof(KCmd); }

// ---- frame time -----------------------------------------------------------------------------------------------------
// FSR 3.1 needs a frame time > 0; the game passes 0 for about 500 frames after a world load (B0). The game's own value
// (ns) when it is in (0, 1 s]; else Kiln's measured time between FSR frames, clamped to 1..1000 ms; else 1/60 s.
inline float pick_frame_ms(int64_t game_ns, double measured_ms) {
    if (game_ns > 0 && game_ns <= 1'000'000'000) return static_cast<float>(static_cast<double>(game_ns) / 1e6);
    if (std::isfinite(measured_ms) && measured_ms > 0) return static_cast<float>(std::clamp(measured_ms, 1.0, 1000.0));
    return 1000.0f / 60.0f;
}

// ---- camera planes --------------------------------------------------------------------------------------------------
// The game's port takes min and max of its two plane values (0.01 and 10000 seen); FSR wants near < far, both > 0.
inline void planes(float a, float b, float* near_plane, float* far_plane) {
    float n = std::fmin(a, b), f = std::fmax(a, b);
    if (!std::isfinite(n) || n <= 0) n = 0.01f;
    if (!std::isfinite(f) || f <= n) f = std::fmax(n * 2, 10000.0f);
    *near_plane = n;
    *far_plane = f;
}
inline float sane_exposure(float e) { return std::isfinite(e) && e > 0 ? e : 1.0f; }
inline float sane_fov(float f) { return std::isfinite(f) && f > 0.01f && f < 3.1f ? f : 1.1345f; }

// ---- textures -------------------------------------------------------------------------------------------------------
// What the swap gives a backend, by role: formats AMD's and Intel's upscalers take (FfxApiSurfaceFormat's set; R16G16_SNORM
// motion vectors are converted to R16G16_SFLOAT by the core first: upscale_mv.h). Null: fine, else why not.
inline bool color_format_ok(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return true;
        default: return false;
    }
}
inline bool depth_format_ok(VkFormat f) {
    return f == VK_FORMAT_R32_SFLOAT || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_R16_UNORM || f == VK_FORMAT_D16_UNORM ||
           f == VK_FORMAT_R16_SFLOAT;
}
inline bool motion_format_ok(VkFormat f) {
    return f == VK_FORMAT_R16G16_SFLOAT || f == VK_FORMAT_R32G32_SFLOAT || f == VK_FORMAT_R16G16_SNORM;
}
inline bool mask_format_ok(VkFormat f) { return f == VK_FORMAT_R8_UNORM || f == VK_FORMAT_R16_SFLOAT || f == VK_FORMAT_R16_UNORM; }
inline bool output_format_ok(VkFormat f) { return color_format_ok(f); }

// The frame as the core built it: every texture there, formats known, the render size inside the input textures and
// the output size inside the output. Null when fine.
inline const char* check_frame(const Frame& f, std::string* detail) {
    auto say = [&](const char* role, const char* what, VkFormat fmt) {
        if (detail) *detail = std::string(role) + " " + what + (fmt ? " (VkFormat " + std::to_string(static_cast<int>(fmt)) + ")" : "");
        return "inputs";
    };
    const Texture* t[kRoles] = {&f.color, &f.depth, &f.motion, &f.output, &f.reactive, &f.transparency};
    for (int r = 0; r < kRoles; ++r)
        if (!t[r]->present() || !t[r]->width || !t[r]->height) return say(role_name(r), "missing", VK_FORMAT_UNDEFINED);
    if (!color_format_ok(f.color.format)) return say("colour", "in a format the upscaler does not take", f.color.format);
    if (!depth_format_ok(f.depth.format)) return say("depth", "in a format the upscaler does not take", f.depth.format);
    if (!motion_format_ok(f.motion.format)) return say("motion vectors", "in a format the upscaler does not take", f.motion.format);
    if (!mask_format_ok(f.reactive.format)) return say("reactive mask", "in a format the upscaler does not take", f.reactive.format);
    if (!mask_format_ok(f.transparency.format)) return say("transparency mask", "in a format the upscaler does not take", f.transparency.format);
    if (!output_format_ok(f.output.format)) return say("output", "in a format the upscaler does not take", f.output.format);
    if (!f.render_width || !f.render_height || f.render_width > f.color.width || f.render_height > f.color.height)
        return say("render size", "outside the colour texture", VK_FORMAT_UNDEFINED);
    if (f.depth.width < f.render_width || f.depth.height < f.render_height || f.motion.width < f.render_width || f.motion.height < f.render_height)
        return say("depth or motion vectors", "smaller than the render size", VK_FORMAT_UNDEFINED);
    if (!f.output_width || !f.output_height || f.output_width > f.output.width || f.output_height > f.output.height)
        return say("output size", "outside the output texture", VK_FORMAT_UNDEFINED);
    return nullptr;
}

// ---- contexts and history -------------------------------------------------------------------------------------------
// A backend's context is made again when its sizes or the pin change; else the game's Fsr3Create (a world load, the
// quality, ...) only drops history.
inline bool same_context(const CreateInfo& a, const CreateInfo& b) {
    return a.max_render_width == b.max_render_width && a.max_render_height == b.max_render_height && a.output_width == b.output_width &&
           a.output_height == b.output_height && a.amd == b.amd && a.color_format == b.color_format && a.output_format == b.output_format;
}
// A frame whose textures are larger than the context was made for (the game grew them without an Fsr3Create): make it
// again for the textures' sizes.
inline bool frame_fits(const CreateInfo& ci, const Frame& f) {
    return f.color.width <= ci.max_render_width && f.color.height <= ci.max_render_height && f.output_width <= ci.output_width &&
           f.output_height <= ci.output_height;
}

// History is dropped on the first frame after a create, after a world load, when the render or output size changes
// (dynamic resolution moves only the render size inside the textures: that keeps history, as FSR wants), and when the
// game asks (its reset byte, 0 so far).
struct HistoryGuard {
    uint32_t tex_w = 0, tex_h = 0, out_w = 0, out_h = 0;
    uint64_t world = 0;
    bool pending = true;
    void drop() { pending = true; }
    bool frame(const Frame& f, uint64_t world_epoch, bool game_reset) {
        bool r = pending || game_reset || world_epoch != world || f.color.width != tex_w || f.color.height != tex_h ||
                 f.output_width != out_w || f.output_height != out_h;
        pending = false;
        world = world_epoch;
        tex_w = f.color.width;
        tex_h = f.color.height;
        out_w = f.output_width;
        out_h = f.output_height;
        return r;
    }
};

// Kiln's old contexts wait until the GPU is surely done with them: the game records into two command buffers in turn
// and waits for one's fence before it records into it again, so a context last used in frame N is free once frame
// N + 2 is being recorded. Kiln waits 4.
inline constexpr uint64_t kRetireFrames = 4;
inline bool retire_due(uint64_t last_used_frame, uint64_t now_frame) { return now_frame >= last_used_frame + kRetireFrames; }

// ---- the status -----------------------------------------------------------------------------------------------------
struct StatusIn {
    Choice chosen = Choice::game;
    bool hooked = false;            // the pass hooks are in (decided at the game's start)
    Choice hooked_for = Choice::game;  // the choice at start (whose device needs were merged)
    uint32_t aside = 0;             // KILN_UI_GFX_ASIDE_* keeping the swap off
    bool signatures = true;         // sigdb feature upscale.swap on
    bool fsr_seen = false;          // the game ran its FSR 3 pass in the last two seconds
    bool started = false;           // a backend context exists for the chosen backend and recorded a frame recently
    bool failed = false;            // the chosen backend failed this session (fell back)
    Why why = Why::none;            // the chosen backend's own reason (files, device, load...), none when fine
    Running backend_running = Running::off;  // what the backend says it runs (while started)
};
struct StatusOut {
    Running running = Running::off;
    Why why = Why::none;
};
inline StatusOut decide_status(const StatusIn& s) {
    StatusOut o;
    if (!s.hooked) {
        o.running = Running::off;
        if (s.chosen == Choice::game) return o;
        if (s.aside) o.why = Why::aside;
        else if (!s.signatures) o.why = Why::no_signatures;
        else if (s.why != Why::none) o.why = s.why;
        else o.why = Why::restart;  // chosen while the game runs: the hooks go in at the start
        return o;
    }
    if (!s.fsr_seen) {
        o.running = Running::not_fsr;
        if (s.chosen != Choice::game) o.why = s.failed ? (s.why != Why::none ? s.why : Why::runtime_failed) : s.why;
        return o;
    }
    if (s.chosen == Choice::game) {
        o.running = Running::game_fsr3;
        return o;
    }
    if (s.started && !s.failed) {
        o.running = s.backend_running;
        return o;
    }
    o.running = Running::game_fsr3;
    if (s.failed) o.why = s.why != Why::none ? s.why : Why::runtime_failed;
    else if (s.why != Why::none) o.why = s.why;
    else o.why = Why::waiting;
    return o;
}

inline const char* why_text(Why w) {
    switch (w) {
        case Why::none: return "";
        case Why::restart: return "it runs from the next start of the game";
        case Why::files_missing: return "its files are not downloaded (launcher: Settings, Upscalers)";
        case Why::files_bad: return "its files are not the ones Kiln pins: download them again";
        case Why::load_failed: return "its files did not load";
        case Why::device: return "the graphics card or driver lacks a Vulkan feature it needs";
        case Why::create_failed: return "it could not be set up for these sizes";
        case Why::runtime_failed: return "it failed while upscaling: the game's FSR 3 until the game restarts";
        case Why::fault: return "it faulted: the game's FSR 3 until the game restarts";
        case Why::aside: return "Kiln steps aside for another tool";
        case Why::no_signatures: return "this game build's FSR 3 pass was not found";
        case Why::inputs: return "the game's textures are not what it takes";
        case Why::unavailable: return "it is not in this build of Kiln, or not on this system";
        case Why::waiting: return "it starts at the game's next FSR 3 set-up (load a world, or change FSR's quality)";
        case Why::no_vulkan: return "Kiln did not see the game's Vulkan device created: restart the game";
    }
    return "";
}
inline const char* running_name(Running r) {
    switch (r) {
        case Running::off: return "off";
        case Running::not_fsr: return "not FSR 3";
        case Running::game_fsr3: return "the game's FSR 3";
        case Running::amd_fsr4: return "AMD FSR 4";
        case Running::amd_fsr3: return "AMD FSR 3.1";
        case Running::xess: return "Intel XeSS";
    }
    return "?";
}
inline std::string version_text(uint32_t v) {
    if (!v) return {};
    return std::to_string(v >> 22) + "." + std::to_string((v >> 12) & 0x3ff) + "." + std::to_string(v & 0xfff);
}
// "4.1.2" -> make_version(4, 1, 2); 0 when not a version.
inline uint32_t parse_version(const char* s) {
    if (!s) return 0;
    uint32_t part[3] = {0, 0, 0};
    int i = 0;
    bool digit = false;
    for (const char* p = s; *p && i < 3; ++p) {
        if (*p >= '0' && *p <= '9') {
            part[i] = part[i] * 10 + static_cast<uint32_t>(*p - '0');
            digit = true;
            if (part[i] > 1023) return 0;
        } else if (*p == '.' && digit) {
            ++i;
            digit = false;
        } else {
            break;
        }
    }
    if (!part[0]) return 0;
    return make_version(part[0], part[1], part[2]);
}

// Copies `s` into a fixed char array, NUL-terminated, cut at a UTF-8 character boundary.
inline void copy_detail(char* out, size_t cap, const std::string& s) {
    if (!cap) return;
    size_t n = std::min(s.size(), cap - 1);
    while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    std::memcpy(out, s.data(), n);
    out[n] = 0;
}

}  // namespace kui::upscale
