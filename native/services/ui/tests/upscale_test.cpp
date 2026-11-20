// SPDX-License-Identifier: MIT
// Host test of the upscaler swap's plain logic (phase B): kiln.json "upscaler"/"amdUpscaler" (graphics_settings.h), the
// upscaler tools Kiln steps aside for (graphics_aside.h), the game's FSR description and Kiln's render command, the
// frame time, history and contexts, the status (upscale_logic.h), and the AMD FSR mapping (upscale_amd_map.h).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "services/kiln_ui.h"

#include "../graphics_aside.h"
#include "../graphics_settings.h"
#include "../upscale_amd_map.h"
#include "../upscale_logic.h"

using namespace kui;
using namespace kui::upscale;

static int g_checks = 0;
#define CHECK(...)                                                                                   \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                        \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #__VA_ARGS__);     \
            std::exit(1);                                                                            \
        }                                                                                            \
    } while (0)

static gfx::Settings parse(const char* json, std::vector<std::string>* w = nullptr) {
    return gfx::parse_settings(nlohmann::json::parse(json, nullptr, false, true), w);
}

static void settings() {
    auto s = parse(R"({"graphics": {}})");
    CHECK(s.upscaler == gfx::Upscaler::game && s.amd_pin == gfx::AmdPin::automatic && !s.wants_swap());
    s = parse(R"({"graphics": {"upscaler": "amd", "amdUpscaler": "fsr3"}})");
    CHECK(s.upscaler == gfx::Upscaler::amd && s.amd_pin == gfx::AmdPin::fsr3 && s.wants_swap() && !s.wants_ngx());
    s = parse(R"({"graphics": {"upscaler": "xess"}})");
    CHECK(s.upscaler == gfx::Upscaler::xess && s.amd_pin == gfx::AmdPin::automatic);
    std::vector<std::string> w;
    s = parse(R"({"graphics": {"upscaler": "dlss", "amdUpscaler": 4}})", &w);
    CHECK(s.upscaler == gfx::Upscaler::game && s.amd_pin == gfx::AmdPin::automatic && w.size() == 2);
    CHECK(w[0].find("graphics.upscaler") != std::string::npos && w[1].find("graphics.amdUpscaler") != std::string::npos);
    w.clear();
    s = parse(R"({"graphics": {"upscaler": "AMD"}})", &w);  // the keys are exact (lower case), as the others
    CHECK(s.upscaler == gfx::Upscaler::game && w.size() == 1);
    s = parse(R"({"graphics": {"upscaler": null, "amdUpscaler": null}})");
    CHECK(s.upscaler == gfx::Upscaler::game);
    gfx::Settings bad;
    bad.upscaler = static_cast<gfx::Upscaler>(7);
    bad.amd_pin = static_cast<gfx::AmdPin>(9);
    bad = gfx::clamped(bad);
    CHECK(bad.upscaler == gfx::Upscaler::game && bad.amd_pin == gfx::AmdPin::automatic);
    // the numbers are the ABI's
    CHECK(static_cast<uint32_t>(gfx::Upscaler::amd) == KILN_UI_UPSCALER_AMD && static_cast<uint32_t>(gfx::Upscaler::xess) == KILN_UI_UPSCALER_XESS);
    CHECK(static_cast<uint32_t>(Choice::amd) == KILN_UI_UPSCALER_AMD && static_cast<uint32_t>(AmdPin::fsr3) == KILN_UI_AMD_UPSCALER_FSR3);
    CHECK(std::string(gfx::upscaler_key(gfx::Upscaler::xess)) == "xess");
    CHECK(std::string(gfx::upscaler_name(gfx::Upscaler::amd)).find("DLSS") == std::string::npos);
}

static void aside(const std::string& tmp) {
    namespace fs = std::filesystem;
    const fs::path game = fs::path(tmp) / "upscale-aside";
    fs::remove_all(game);
    fs::create_directories(game);
    auto a = gfx::detect_aside_files(game, false, false);
    CHECK(a.bits == 0);
    std::ofstream(game / "amd_fidelityfx_upscaler_vk.dll") << "x";
    a = gfx::detect_aside_files(game, false, false);
    CHECK(a.bits == gfx::kAsideUpscaler && a.files.size() == 1);
    CHECK((a.bits & gfx::kAsideUpscale) && !(a.bits & gfx::kAsideDlss) && !(a.bits & gfx::kAsideCap));
    std::ofstream(game / "libxess.dll") << "x";
    a = gfx::detect_aside_files(game, true, false);
    CHECK((a.bits & gfx::kAsideOptiScaler) && (a.bits & gfx::kAsideUpscaler) && a.files.size() == 2);
    CHECK(gfx::aside_names(gfx::kAsideUpscaler).find("upscaler tool") != std::string::npos);
    CHECK((gfx::kAsideUpscale & gfx::kAsideOverlayOff) && !(gfx::kAsideUpscale & gfx::kAsideDlssTweaks) && !(gfx::kAsideUpscale & gfx::kAsideNvngx));
    CHECK(static_cast<uint32_t>(gfx::kAsideUpscaler) == static_cast<uint32_t>(KILN_UI_GFX_ASIDE_UPSCALER));
    fs::remove_all(game);
}

// A description and input struct as the game fills them (B0's values: 960x540 -> 1920x1080).
struct GameFrame {
    uint8_t desc[DescLayout::kDescBytes]{};
    uint8_t in[kInBytes]{};
    DescLayout L;
    template <class T>
    void put(uint8_t* p, int64_t off, T v) { std::memcpy(p + off, &v, sizeof v); }
    GameFrame() {
        for (int r = 0; r < kRoles; ++r) {
            desc[kDescRes[r]] = r == kOutput ? 5 : 3;
            for (int k = 0; k < kHandleBytes; ++k) desc[kDescRes[r] + kDescHandle + k] = in[kInHandle[r] + k] = static_cast<uint8_t>(0x10 * r + k + 1);
        }
        put(desc, L.jitter, -0.25f), put(desc, L.jitter + 4, 0.1667f), put(in, 0, -0.25f), put(in, 4, 0.1667f);
        put(desc, L.render, 960u), put(desc, L.render + 4, 540u), put(in, 0x70, 960u), put(in, 0x74, 540u);
        put(desc, L.prev_render, 960u), put(desc, L.prev_render + 4, 540u);
        put(desc, L.upscale, 1920u), put(desc, L.upscale + 4, 1080u), put(in, 0x80, 1920u), put(in, 0x84, 1080u);
        put(desc, L.near_plane, 0.01f), put(desc, L.far_plane, 10000.0f), put(desc, L.fov, 1.1345f), put(desc, L.exposure, 0.42f);
        put(desc, L.mv_scale, 960.0f), put(desc, L.mv_scale + 4, 540.0f), put(desc, L.frame_ns, int64_t{16'666'667});
        put(desc, L.sharpness, 0.0f);
    }
};

static void description() {
    DescLayout L;
    CHECK(L.sane() && L.extent() == 0x144);
    DescLayout broken = L;
    broken.sharpness = 0x200;
    CHECK(!broken.sane());
    broken = L;
    broken.fov = -1;
    CHECK(!broken.sane());
    GameFrame g;
    const DescValues v = read_desc(g.desc, L);
    CHECK(v.render_w == 960 && v.render_h == 540 && v.upscale_w == 1920 && v.upscale_h == 1080 && v.prev_w == 960);
    CHECK(v.jitter_x == -0.25f && std::fabs(v.jitter_y - 0.1667f) < 1e-6f && v.mv_x == 960.0f && v.mv_y == 540.0f);
    CHECK(v.plane_a == 0.01f && v.plane_b == 10000.0f && v.fov == 1.1345f && v.exposure == 0.42f && v.frame_ns == 16'666'667 && !v.reset);
    CHECK(desc_matches_input(g.in, g.desc, L) == nullptr);
    CHECK(desc_matches_input(nullptr, g.desc, L) == nullptr);
    GameFrame moved;
    moved.desc[kDescRes[kDepth] + kDescHandle] ^= 0xff;  // a build that moved the resources: caught
    CHECK(std::string(desc_matches_input(moved.in, moved.desc, L)) == "depth");
    GameFrame j;
    j.put(j.in, 0, 0.3f);
    CHECK(std::string(desc_matches_input(j.in, j.desc, L)) == "jitter");
    GameFrame o;
    o.put(o.in, 0x80, 1280u);
    CHECK(std::string(desc_matches_input(o.in, o.desc, L)) == "output size");
}

static void command() {
    const KCmd c = make_cmd(kUpscale, 77, 5);
    CHECK(c.size == 32 && c.type == 0xF0 && c.kind == kUpscale && c.seq == 77 && c.slot == 5);
    uint8_t buf[40] = {};
    std::memcpy(buf, &c, sizeof c);
    KCmd out{};
    CHECK(is_kiln_cmd(buf, sizeof buf, &out) && out.seq == 77 && out.slot == 5);
    CHECK(!is_kiln_cmd(buf, 31, nullptr));  // less than a whole command readable
    uint8_t other[32] = {};
    other[2] = 0xF0;  // a type-0xF0 command that is not Kiln's
    CHECK(!is_kiln_cmd(other, sizeof other, nullptr));
    uint8_t tampered[32];
    std::memcpy(tampered, &c, sizeof c);
    tampered[4] ^= 1;  // seq changed: the check fails
    CHECK(!is_kiln_cmd(tampered, sizeof tampered, nullptr));
    uint8_t game[32] = {0x20, 0, 0x2c};  // the game's own command 0x2c
    CHECK(!is_kiln_cmd(game, sizeof game, nullptr));
    CHECK(kCmdType > 0x2e);  // above the game's last type on 26430625
    CHECK(chunk_has_room(100, 132) && !chunk_has_room(100, 131) && !chunk_has_room(200, 100));
}

static void frame_values() {
    CHECK(pick_frame_ms(16'666'667, 33.0) == static_cast<float>(16.666667));
    CHECK(pick_frame_ms(0, 12.5) == 12.5f);          // the game's 0 after a world load: Kiln's own
    CHECK(pick_frame_ms(0, 0.2) == 1.0f);            // clamped
    CHECK(pick_frame_ms(-5, 5000.0) == 1000.0f);
    CHECK(std::fabs(pick_frame_ms(0, 0.0) - 16.6667f) < 1e-3f);  // nothing known: 60 fps
    CHECK(pick_frame_ms(2'000'000'000, 20.0) == 20.0f);           // a game value over a second is not used
    float n = 0, f = 0;
    planes(10000.0f, 0.01f, &n, &f);
    CHECK(n == 0.01f && f == 10000.0f);
    planes(0.01f, 10000.0f, &n, &f);
    CHECK(n == 0.01f && f == 10000.0f);
    planes(0.0f, 0.0f, &n, &f);
    CHECK(n > 0 && f > n);
    planes(NAN, 5.0f, &n, &f);
    CHECK(std::isfinite(n) && n > 0 && f > n);
    CHECK(sane_exposure(0.0f) == 1.0f && sane_exposure(0.5f) == 0.5f && sane_exposure(NAN) == 1.0f);
    CHECK(sane_fov(1.1345f) == 1.1345f && sane_fov(0.0f) > 0.5f && sane_fov(70.0f) < 3.0f);
}

static Texture tex(VkFormat f, uint32_t w, uint32_t h, uintptr_t id) {
    Texture t;
    t.image = reinterpret_cast<VkImage>(id);
    t.view = reinterpret_cast<VkImageView>(id + 1);
    t.format = f;
    t.width = w;
    t.height = h;
    t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return t;
}
static Frame b0_frame() {
    Frame f;
    f.color = tex(VK_FORMAT_R16G16B16A16_SFLOAT, 960, 540, 0x1000);
    f.depth = tex(VK_FORMAT_R32_SFLOAT, 960, 540, 0x2000);
    f.motion = tex(VK_FORMAT_R16G16_SNORM, 960, 540, 0x3000);
    f.output = tex(VK_FORMAT_R16G16B16A16_SFLOAT, 1920, 1080, 0x4000);
    f.output.layout = VK_IMAGE_LAYOUT_GENERAL;
    f.reactive = tex(VK_FORMAT_R8_UNORM, 960, 540, 0x5000);
    f.transparency = tex(VK_FORMAT_R8_UNORM, 960, 540, 0x6000);
    f.render_width = 960, f.render_height = 540, f.output_width = 1920, f.output_height = 1080;
    f.jitter_x = -0.25f, f.jitter_y = 0.1667f, f.mv_scale_x = 960, f.mv_scale_y = 540;
    f.frame_ms = 16.6f, f.camera_near = 0.01f, f.camera_far = 10000.0f, f.fov_y = 1.1345f, f.pre_exposure = 0.42f;
    f.cb = reinterpret_cast<VkCommandBuffer>(uintptr_t{0x7000});
    return f;
}

static void frames() {
    std::string d;
    Frame f = b0_frame();
    CHECK(check_frame(f, &d) == nullptr);
    f.depth.format = VK_FORMAT_D32_SFLOAT;  // the first frame's depth (B0)
    CHECK(check_frame(f, &d) == nullptr);
    f = b0_frame();
    f.motion.format = VK_FORMAT_R8G8_UNORM;
    CHECK(check_frame(f, &d) && d.find("motion vectors") != std::string::npos && d.find("VkFormat") != std::string::npos);
    f = b0_frame();
    f.reactive.image = VK_NULL_HANDLE;
    CHECK(check_frame(f, &d) && d.find("reactive mask missing") != std::string::npos);
    f = b0_frame();
    f.render_width = 1000;  // larger than the colour texture
    CHECK(check_frame(f, &d) && d.find("render size") != std::string::npos);
    f = b0_frame();
    f.render_width = 640, f.render_height = 360;  // dynamic resolution inside the texture: fine
    CHECK(check_frame(f, &d) == nullptr);
    f = b0_frame();
    f.output_width = 2560;
    CHECK(check_frame(f, &d) && d.find("output size") != std::string::npos);
    f = b0_frame();
    f.output.format = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;  // fine as colour, not as a storage output
    CHECK(check_frame(f, &d) && d.find("output") != std::string::npos && color_format_ok(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32));

    // contexts: for the colour texture, the upscale size (the description's) and the output texture
    f = b0_frame();
    const CreateInfo a = context_for(f, AmdPin::automatic);
    CHECK(a.max_render_width == 960 && a.max_render_height == 540 && a.output_width == 1920 && a.output_height == 1080 &&
          a.output_texture_width == 1920 && a.output_texture_height == 1080 && a.ratio == 2.0f && a.color_format == f.color.format);
    CHECK(a.hdr && a.depth_inverted && !a.depth_infinite && a.dynamic_resolution);
    CreateInfo b = a;
    CHECK(same_context(a, b));
    b.amd = AmdPin::fsr3;
    CHECK(!same_context(a, b));
    b = a;
    b.output_width = 1440;
    CHECK(!same_context(a, b));
    CHECK(frame_fits(a, f));
    f.render_width = 640, f.render_height = 360;  // dynamic resolution: the same context
    CHECK(frame_fits(a, f));
    f = b0_frame();
    f.color.width = 1280;
    CHECK(!frame_fits(a, f));
    // the 3D render scale: the upscale size below the output texture (1885x1061 in 1920x1080, B0): its own context
    f = b0_frame();
    f.output_width = 1885, f.output_height = 1061;
    CHECK(!frame_fits(a, f) && check_frame(f, &d) == nullptr);
    const CreateInfo scaled = context_for(f, AmdPin::automatic);
    CHECK(scaled.output_width == 1885 && scaled.output_texture_width == 1920 && frame_fits(scaled, f) && !same_context(a, scaled));

    // history
    HistoryGuard h;
    f = b0_frame();
    CHECK(h.frame(f, 0, false));    // the first frame
    CHECK(!h.frame(f, 0, false));
    CHECK(h.frame(f, 1, false));    // a world load (a gap in FSR frames)
    CHECK(!h.frame(f, 1, false));
    CHECK(h.frame(f, 1, true));     // the game's own reset byte
    f.render_width = 640;           // dynamic resolution: history stays
    CHECK(!h.frame(f, 1, false));
    f.output_width = 1440;          // the output changed
    CHECK(h.frame(f, 1, false));
    h.drop();                       // Fsr3Create
    CHECK(h.frame(f, 1, false) && !h.frame(f, 1, false));
    // a frame the game's FSR 3 upscaled in between (an unusable frame, a command Kiln could not write): the core drops
    // the backend's history, so the next of Kiln's frames does not blend in a picture from before the game's
    CHECK(!h.frame(f, 1, false));
    h.drop();
    CHECK(h.frame(f, 1, false) && !h.frame(f, 1, false));

    CHECK(!retire_due(10, 12) && !retire_due(10, 13) && retire_due(10, 14) && retire_due(0, 4));
}

// Which backend calls the core still makes (upscale_backend.h "Faults"): after a fault none at all (the backend may hold
// a lock: also no needs or device_ready for a later instance or a re-created device); after a session fall-back only
// the calls that free what it holds.
static void calls() {
    const Call all[] = {Call::load, Call::instance_needs, Call::device_needs, Call::device_ready, Call::create, Call::record,
                        Call::reset, Call::tick, Call::destroy, Call::device_lost};
    for (Call c : all) {
        CHECK(may_call(c, false, false));
        CHECK(!may_call(c, true, false) && !may_call(c, true, true));
    }
    CHECK(may_call(Call::destroy, false, true) && may_call(Call::device_lost, false, true) && may_call(Call::tick, false, true));
    CHECK(!may_call(Call::instance_needs, false, true) && !may_call(Call::device_needs, false, true));
    CHECK(!may_call(Call::device_ready, false, true) && !may_call(Call::create, false, true) && !may_call(Call::record, false, true));
    CHECK(!may_call(Call::reset, false, true) && !may_call(Call::load, false, true));
}

static StatusOut st(StatusIn in) { return decide_status(in); }
// A choice changed in the game applies at the next FSR frame, not only at the game's next FSR 3 set-up: the game does
// not set FSR 3 up again for every quality change (SleepyPuppy at native AA, 29.19 beta 1: AMD FSR stayed "ready").
static void latch() {
    const uint32_t game = static_cast<uint32_t>(Choice::game), amd = static_cast<uint32_t>(Choice::amd);
    const uint32_t autop = static_cast<uint32_t>(AmdPin::automatic), fsr3 = static_cast<uint32_t>(AmdPin::fsr3);
    CHECK(!latch_at_frame(false, game, autop, amd, autop));  // no set-up seen yet: the game's first Fsr3Create latches
    CHECK(!latch_at_frame(true, amd, autop, amd, autop));    // nothing changed
    CHECK(latch_at_frame(true, game, autop, amd, autop));    // back to AMD FSR after "the game's own"
    CHECK(latch_at_frame(true, amd, autop, game, autop));    // to the game's own: its FSR 3 at once
    CHECK(latch_at_frame(true, amd, autop, amd, fsr3));      // the FSR 3.1 pin
}

static void status() {
    StatusIn in;
    // "game" chosen, nothing hooked: off, no reason
    auto o = st(in);
    CHECK(o.running == Running::off && o.why == Why::none);
    // AMD chosen while the game runs (no hooks from the start): restart
    in.chosen = Choice::amd;
    o = st(in);
    CHECK(o.running == Running::off && o.why == Why::restart);
    in.aside = KILN_UI_GFX_ASIDE_OPTISCALER;
    CHECK(st(in).why == Why::aside);
    in.aside = 0;
    in.signatures = false;
    CHECK(st(in).why == Why::no_signatures);
    in.signatures = true;
    in.why = Why::unavailable;
    CHECK(st(in).why == Why::unavailable);
    // hooked for AMD, the game not upscaling with FSR 3 now
    in = StatusIn{};
    in.chosen = Choice::amd, in.hooked = true, in.hooked_for = Choice::amd;
    o = st(in);
    CHECK(o.running == Running::not_fsr && o.why == Why::none);
    in.why = Why::files_missing;
    CHECK(st(in).why == Why::files_missing);
    // FSR 3 running, AMD not started (files missing): the game's FSR 3, files missing
    in.fsr_seen = true;
    o = st(in);
    CHECK(o.running == Running::game_fsr3 && o.why == Why::files_missing);
    // no reason of the backend's own, but Kiln never saw the game's device made (its Vulkan hooks came after it): not
    // "waiting" (it would wait for ever), NO_VULKAN
    in.why = Why::none;
    o = st(in);
    CHECK(o.running == Running::game_fsr3 && o.why == Why::no_vulkan);
    in.fsr_seen = false;  // the game not upscaling with FSR 3: nothing to say yet
    CHECK(st(in).running == Running::not_fsr && st(in).why == Why::none);
    in.fsr_seen = true;
    // ready, waiting for the next set-up
    in.device_ready = true;
    o = st(in);
    CHECK(o.running == Running::game_fsr3 && o.why == Why::waiting);
    // running FSR 4
    in.started = true, in.backend_running = Running::amd_fsr4, in.latched = Choice::amd;
    o = st(in);
    CHECK(o.running == Running::amd_fsr4 && o.why == Why::none);
    // the player chose "game" (or pinned another): AMD runs until the next FSR 3 set-up, and the status says so
    in.chosen = Choice::game;
    o = st(in);
    CHECK(o.running == Running::amd_fsr4 && o.why == Why::waiting);
    in.chosen = Choice::xess;  // another backend: from the next start, AMD still runs now
    o = st(in);
    CHECK(o.running == Running::amd_fsr4 && o.why == Why::restart);
    in.chosen = Choice::amd;
    // failed in this session
    in.failed = true, in.why = Why::runtime_failed;
    o = st(in);
    CHECK(o.running == Running::game_fsr3 && o.why == Why::runtime_failed);
    in.why = Why::none;
    CHECK(st(in).why == Why::runtime_failed);
    in.why = Why::fault;
    CHECK(st(in).why == Why::fault);
    // hooked for AMD, the player went back to "game": the game's FSR 3, nothing to say
    in = StatusIn{};
    in.chosen = Choice::game, in.hooked = true, in.hooked_for = Choice::amd, in.fsr_seen = true;
    o = st(in);
    CHECK(o.running == Running::game_fsr3 && o.why == Why::none);
    // the numbers are the ABI's
    CHECK(static_cast<uint32_t>(Running::amd_fsr4) == KILN_UI_UPSCALE_RUN_AMD_FSR4 && static_cast<uint32_t>(Running::xess) == KILN_UI_UPSCALE_RUN_XESS);
    CHECK(static_cast<uint32_t>(Why::restart) == KILN_UI_UPSCALE_WHY_RESTART && static_cast<uint32_t>(Why::no_vulkan) == KILN_UI_UPSCALE_WHY_NO_VULKAN);
    CHECK(static_cast<uint32_t>(Why::waiting) == KILN_UI_UPSCALE_WHY_WAITING && static_cast<uint32_t>(Why::inputs) == KILN_UI_UPSCALE_WHY_INPUTS);
    for (uint32_t w = 1; w <= 14; ++w) {
        const std::string t = why_text(static_cast<Why>(w));
        CHECK(!t.empty() && t.find("DLSS") == std::string::npos && t.find("unlock") == std::string::npos);
    }
    CHECK(std::string(running_name(Running::amd_fsr3)) == "AMD FSR 3.1");
    // the files are downloaded on the launcher's Settings > Graphics card (phase B took them out of the Upscalers preview)
    CHECK(std::string(why_text(Why::files_missing)).find("Settings, Graphics") != std::string::npos);
    for (uint32_t w = 1; w <= 14; ++w) CHECK(std::string(why_text(static_cast<Why>(w))).find("Upscalers") == std::string::npos);

    CHECK(parse_version("4.1.2") == make_version(4, 1, 2) && parse_version("3.1.6") == make_version(3, 1, 6));
    CHECK(parse_version("3.1.6*") == make_version(3, 1, 6) && parse_version("x") == 0 && parse_version(nullptr) == 0);
    CHECK(version_text(make_version(4, 1, 2)) == "4.1.2" && version_text(0).empty());
    char buf[8];
    copy_detail(buf, sizeof buf, "abc");
    CHECK(std::string(buf) == "abc");
    copy_detail(buf, sizeof buf, "abcdefghij");
    CHECK(std::string(buf) == "abcdefg");
    copy_detail(buf, sizeof buf, "abcde\xc3\xa9z");  // a two-byte character at the cut: not split
    CHECK(std::string(buf) == "abcde\xc3\xa9");
    copy_detail(buf, sizeof buf, "abcdef\xc3\xa9");
    CHECK(std::string(buf) == "abcdef");
    // the status struct's layout (kiln_ui.h)
    CHECK(sizeof(KilnUiGraphicsStatus) == KILN_UI_GRAPHICS_STATUS_SIZE_V2 && offsetof(KilnUiGraphicsStatus, upscaler) == KILN_UI_GRAPHICS_STATUS_SIZE_V1);
    CHECK(offsetof(KilnUiGraphicsStatus, upscale_detail) == 120 && sizeof(KilnUiGraphics) == 48 && offsetof(KilnUiGraphics, amd_upscaler) == 44);
}

static bool has_ext(const DeviceNeeds& n, const char* e) {
    for (const auto& x : n.extensions)
        if (x.name == e) return !x.required;
    return false;
}
static const FeatureNeed* need_of(const DeviceNeeds& n, VkStructureType t) {
    for (const auto& f : n.features) {
        VkStructureType s;
        std::memcpy(&s, f.bytes.data(), sizeof s);
        if (s == t) return &f;
    }
    return nullptr;
}

static void amd_map() {
    // device needs: everything FFX uses when offered, and FSR 4's set; all optional
    const DeviceNeeds n = amd::device_needs();
    for (const char* e : {VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_AMD_BUFFER_MARKER_EXTENSION_NAME,
                          VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME,
                          VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME,
                          VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME})
        CHECK(has_ext(n, e));
    for (const auto& f : n.features) CHECK(!f.required && f.bytes.size() > 16);
    const FeatureNeed* ssc = need_of(n, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES);
    CHECK(ssc);
    VkPhysicalDeviceSubgroupSizeControlFeatures sscv;
    std::memcpy(&sscv, ssc->bytes.data(), sizeof sscv);
    CHECK(sscv.subgroupSizeControl && sscv.computeFullSubgroups);
    const FeatureNeed* f2 = need_of(n, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    CHECK(f2);
    VkPhysicalDeviceFeatures2 f2v;
    std::memcpy(&f2v, f2->bytes.data(), sizeof f2v);
    CHECK(f2v.features.shaderStorageImageWriteWithoutFormat && f2v.features.shaderInt16 && !f2v.features.geometryShader);
    // every extension of used_if_offered is asked for
    for (const auto& u : amd::used_if_offered()) CHECK(has_ext(n, u.extension));

    // missing_used_if_offered: what FFX would misuse
    VkContext vk;
    vk.instance_api = VK_API_VERSION_1_2;
    std::vector<std::string> offered = {VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME};
    auto all = [](VkStructureType, uint32_t) { return true; };
    CHECK(amd::missing_used_if_offered(vk, offered, all) == VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);  // the game's own device
    vk.extensions = {VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME};
    CHECK(amd::missing_used_if_offered(vk, offered, all) == VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);  // extension, no feature
    vk.features.push_back({});
    {
        VkPhysicalDeviceSubgroupSizeControlFeatures s{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, nullptr, VK_TRUE, VK_TRUE};
        vk.features.back().bytes.resize(sizeof s);
        std::memcpy(vk.features.back().bytes.data(), &s, sizeof s);
        VkPhysicalDeviceSynchronization2Features y{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, nullptr, VK_TRUE};
        FeatureNeed fy;
        fy.bytes.resize(sizeof y);
        std::memcpy(fy.bytes.data(), &y, sizeof y);
        vk.features.push_back(fy);
    }
    CHECK(amd::missing_used_if_offered(vk, offered, all).empty());
    // a feature the GPU does not have is not used by FFX: nothing missing
    VkContext bare;
    auto none = [](VkStructureType, uint32_t) { return false; };
    CHECK(amd::missing_used_if_offered(bare, offered, none).empty());
    // buffer marker: offering it is enough for FFX to use it
    CHECK(amd::missing_used_if_offered(bare, {VK_AMD_BUFFER_MARKER_EXTENSION_NAME}, none) == VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
    // on a 1.3 instance and device the core struct counts
    VkContext core;
    core.instance_api = VK_API_VERSION_1_3;
    core.props.apiVersion = VK_API_VERSION_1_4;
    {
        VkPhysicalDeviceVulkan13Features v13{}; v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        v13.subgroupSizeControl = VK_TRUE;
        v13.synchronization2 = VK_TRUE;
        FeatureNeed fv;
        fv.bytes.resize(sizeof v13);
        std::memcpy(fv.bytes.data(), &v13, sizeof v13);
        core.features.push_back(fv);
    }
    CHECK(amd::missing_used_if_offered(core, offered, all).empty());
    core.props.apiVersion = VK_API_VERSION_1_2;  // a 1.2 device under a 1.3 instance: the core struct does not count
    CHECK(amd::missing_used_if_offered(core, offered, all) == VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);

    // creation
    CreateInfo ci;  // the defaults are the game's: linear HDR colour, reverse-Z with a far plane, dynamic resolution
    // The game gives its FSR 3 no exposure texture and a pre-exposure of 0 (its port reads 1): its colour is the scene
    // before the game's own exposure, which its tone mapping applies after the upscaler. AMD FSR (FSR 4 above all: its
    // network works on the exposed signal) needs an exposure to match: AMD's own auto-exposure, as AMD recommends.
    // Without it FSR 4 took exposure 1.0 and drew a dark scene almost black (SleepyPuppy, RX 9070 XT, 29.19 beta 1).
    CHECK(amd::create_flags(ci) == (FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION |
                                    FFX_UPSCALE_ENABLE_AUTO_EXPOSURE));
    ci.depth_infinite = true;
    CHECK(amd::create_flags(ci) & FFX_UPSCALE_ENABLE_DEPTH_INFINITE);
    ci.depth_infinite = false;
    CHECK(!(amd::create_flags(ci) & FFX_UPSCALE_ENABLE_DEPTH_INFINITE) && (amd::create_flags(ci) & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE));
    CHECK(!(amd::create_flags(ci) & FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE));  // the colour is linear
    {
        // the test knob kiln/control/amd ("exposure one": the game's pre-exposure only, as up to 29.19 beta 1)
        amd::Tuning one = amd::parse_tuning("# A/B\nexposure one\n");
        CHECK(one.exposure == amd::Exposure::one && one.changed());
        CHECK(!(amd::create_flags(ci, one) & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE));
        CHECK(amd::create_flags(ci, one) == (FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION));
        CHECK(amd::parse_tuning("EXPOSURE = Auto\r\n").exposure == amd::Exposure::automatic && !amd::parse_tuning("").changed());
        CHECK(amd::parse_tuning("exposure maybe\nnoise").notes.size() == 2);
        // what kiln.log says at each set-up
        CHECK(amd::flags_text(amd::create_flags(ci)) == "HDR, inverted depth, dynamic resolution, auto exposure");
        CHECK(amd::flags_text(amd::create_flags(ci, one)) == "HDR, inverted depth, dynamic resolution, exposure 1 (no auto exposure)");
        CreateInfo ldr = ci;
        ldr.hdr = false;
        CHECK(amd::flags_text(amd::create_flags(ldr)) == "inverted depth, dynamic resolution, auto exposure");
    }
    CHECK(!(amd::create_flags(ci) & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) &&
          !(amd::create_flags(ci) & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS));
    CHECK(amd::version_major("4.1.2") == 4 && amd::version_major("3.1.6") == 3 && amd::version_major("") == 0 && amd::version_major(nullptr) == 0);
    CHECK(amd::running_of("4.1.2") == Running::amd_fsr4 && amd::running_of("3.1.6") == Running::amd_fsr3 && amd::running_of("2.3.4") == Running::off);
    CHECK(amd::pick_fsr3({"4.1.2", "3.1.6", "2.3.4"}) == 1);
    CHECK(amd::pick_fsr3({"3.1.4", "4.1.2", "3.1.6"}) == 2);
    CHECK(amd::pick_fsr3({"3.1", "3.0.9"}) == 0);
    CHECK(amd::pick_fsr3({"4.1.2", "2.3.4"}) == -1);

    // the dispatch: the game's frame as AMD's struct
    Frame f = b0_frame();
    Texture mv = tex(VK_FORMAT_R16G16_SFLOAT, 960, 540, 0x8000);
    const ffxDispatchDescUpscale d = amd::dispatch(f, mv, true);
    CHECK(d.header.type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE && d.commandList == reinterpret_cast<void*>(f.cb));
    CHECK(d.color.resource == reinterpret_cast<void*>(f.color.image) && d.color.state == FFX_API_RESOURCE_STATE_COMPUTE_READ);
    CHECK(d.color.description.format == FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT && d.color.description.width == 960);
    CHECK(d.depth.description.format == FFX_API_SURFACE_FORMAT_R32_FLOAT && d.depth.description.usage == FFX_API_RESOURCE_USAGE_READ_ONLY);
    CHECK(d.motionVectors.resource == reinterpret_cast<void*>(mv.image) && d.motionVectors.description.format == FFX_API_SURFACE_FORMAT_R16G16_FLOAT);
    CHECK(d.reactive.resource == reinterpret_cast<void*>(f.reactive.image) && d.reactive.description.format == FFX_API_SURFACE_FORMAT_R8_UNORM);
    CHECK(d.transparencyAndComposition.resource == reinterpret_cast<void*>(f.transparency.image));
    CHECK(d.output.resource == reinterpret_cast<void*>(f.output.image) && d.output.state == FFX_API_RESOURCE_STATE_UNORDERED_ACCESS &&
          (d.output.description.usage & FFX_API_RESOURCE_USAGE_UAV) && d.output.description.width == 1920);
    CHECK(d.exposure.resource == nullptr);
    CHECK(d.jitterOffset.x == -0.25f && d.motionVectorScale.x == 960.0f && d.motionVectorScale.y == 540.0f);
    CHECK(d.renderSize.width == 960 && d.renderSize.height == 540 && d.upscaleSize.width == 1920 && d.upscaleSize.height == 1080);
    CHECK(d.reset && d.frameTimeDelta == 16.6f && d.preExposure == 0.42f && d.cameraNear == 0.01f && d.cameraFar == 10000.0f);
    CHECK(d.cameraFovAngleVertical == 1.1345f && !d.enableSharpening && d.flags == 0 && d.viewSpaceToMetersFactor == 1.0f);
    CHECK(!amd::dispatch(f, mv, false).reset);
    f.reset = true;
    CHECK(amd::dispatch(f, mv, false).reset);
    f.depth.format = VK_FORMAT_D32_SFLOAT;  // the first frame's depth buffer: a depth target for AMD
    CHECK(amd::dispatch(f, mv, false).depth.description.usage & FFX_API_RESOURCE_USAGE_DEPTHTARGET);
    f.sharpness = 0.5f;
    CHECK(amd::dispatch(f, mv, false).enableSharpening && amd::dispatch(f, mv, false).sharpness == 0.5f);
    CHECK(std::string(amd::code_name(FFX_API_RETURN_ERROR_PARAMETER)).find("parameter") != std::string::npos);
    // AMD's API cannot describe the game's R16G16_SNORM motion vectors: why Kiln converts them (upscale_mv.h)
    CHECK(ffxApiGetSurfaceFormatVK(VK_FORMAT_R16G16_SNORM) == FFX_API_SURFACE_FORMAT_UNKNOWN);
}

int main(int argc, char** argv) {
    const std::string tmp = argc > 1 ? argv[1] : "/tmp";
    settings();
    aside(tmp);
    description();
    command();
    frame_values();
    frames();
    calls();
    status();
    latch();
    amd_map();
    std::printf("upscale_test: %d checks passed\n", g_checks);
    return 0;
}
