// SPDX-License-Identifier: MIT
// Host test of the XeSS backend (xess_map.h, xess_backend.h) against a fake libxess: the preset for each of the game's
// FSR ratios, the test knobs, the init flags, the inputs as XeSS gets them (images, aspects, jitter, velocity scale,
// exposure, reset), the device needs (XeSS's own answers and the guide's), device_ready's verdicts (device, restart),
// and the life of contexts: created per Fsr3Create, retired, destroyed after four more records or at device_lost, never
// while the GPU may still use them. No GPU, no Windows, no Intel file.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../xess_backend.h"

using namespace kui;
using namespace kui::xess;

static int g_checks = 0;
#define CHECK(...)                                                                                   \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                        \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #__VA_ARGS__);     \
            std::exit(1);                                                                            \
        }                                                                                            \
    } while (0)

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// ---- the plain logic ------------------------------------------------------------------------------------------------
static void presets() {
    bool beyond = true;
    CHECK(quality_for_ratio(3.0f, &beyond) == kUltraPerformance && !beyond);  // FSR Ultra Performance
    CHECK(quality_for_ratio(2.0f) == kBalanced);                               // FSR Performance
    CHECK(quality_for_ratio(1.7f) == kQuality);                                // FSR Balanced
    CHECK(quality_for_ratio(1.5f) == kUltraQuality);                           // FSR Quality
    CHECK(quality_for_ratio(1.0f) == kNativeAA);                               // FSR native AA
    CHECK(quality_for_ratio(1.3f) == kUltraQualityPlus);
    CHECK(quality_for_ratio(2.3f) == kPerformance);
    CHECK(quality_for_ratio(2.2f) == kPerformance);
    CHECK(quality_for_ratio(1.71f) == kQuality);  // rounding of an odd render size
    CHECK(quality_for_ratio(1.75f) == kBalanced);
    CHECK(quality_for_ratio(3.5f, &beyond) == kUltraPerformance && beyond);
    CHECK(quality_for_ratio(NAN) == kNativeAA && quality_for_ratio(0.5f) == kNativeAA);
    upscale::CreateInfo ci;
    ci.max_render_width = 960, ci.max_render_height = 540, ci.output_width = 1920, ci.output_height = 1080;
    CHECK(ratio_of(ci) == 2.0f);
    ci.ratio = 1.7f;
    CHECK(ratio_of(ci) == 1.7f);
    ci.ratio = 0;
    ci.max_render_width = 480, ci.max_render_height = 360, ci.output_width = 1440, ci.output_height = 1080;  // B0's 4:3 case
    CHECK(ratio_of(ci) == 3.0f);
    CHECK(std::string(preset_of(kBalanced).name) == "Balanced" && preset_of(kUltraPerformance).ratio == 3.0f);
}

static void knobs() {
    Tuning t = parse_tuning("");
    CHECK(!t.tuned() && t.mask && t.exposure == Exposure::one && t.log_level == kLogWarning);
    t = parse_tuning("jitter-y flip\r\n# a comment\nmotion-x = flip  # trailing\nexposure inverse\nmask off\nlog debug\nwhat is this\n");
    CHECK(t.jitter_x == 1 && t.jitter_y == -1 && t.mv_x == -1 && t.mv_y == 1);
    CHECK(t.exposure == Exposure::inverse && !t.mask && t.log_level == kLogDebug && t.tuned());
    CHECK(t.notes.size() == 1 && t.notes[0] == "what is this");
    CHECK(parse_tuning("EXPOSURE AUTO\nJitter-X -1\n").exposure == Exposure::automatic);
    CHECK(parse_tuning("jitter-x -1\n").jitter_x == -1 && parse_tuning("jitter-x 1\n").jitter_x == 1);
    CHECK(!parse_tuning("motion-copy off\n").motion_copy && parse_tuning("motion-copy off\n").tuned() && parse_tuning("motion-copy on").motion_copy);
    CHECK(has(t.describe(), "jitter +x -y") && has(t.describe(), "motion -x +y") && has(t.describe(), "exposure inverse") && has(t.describe(), "mask off"));
}

static upscale::CreateInfo game_ci() {
    upscale::CreateInfo ci;
    ci.max_render_width = 960, ci.max_render_height = 540, ci.output_width = 1920, ci.output_height = 1080;
    ci.ratio = 2.0f;
    ci.color_format = ci.output_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    return ci;
}

static void flags_and_create_checks() {
    const upscale::CreateInfo ci = game_ci();
    Tuning t;
    CHECK(init_flags(ci, t) == (kFlagInvertedDepth | kFlagResponsivePixelMask));
    CHECK(flags_text(init_flags(ci, t)) == "INVERTED_DEPTH|RESPONSIVE_PIXEL_MASK");
    t.mask = false;
    t.exposure = Exposure::automatic;
    CHECK(init_flags(ci, t) == (kFlagInvertedDepth | kFlagAutoExposure));
    upscale::CreateInfo ldr = ci;
    ldr.hdr = false;
    ldr.depth_inverted = false;
    CHECK(init_flags(ldr, Tuning{}) == (kFlagLdrInputColor | kFlagResponsivePixelMask));
    CHECK(flags_text(0) == "NONE");
    std::string why;
    CHECK(check_create(ci, &why));
    upscale::CreateInfo bad = ci;
    bad.output_width = 0;
    CHECK(!check_create(bad, &why) && has(why, "no size"));
    bad = ci;
    bad.max_render_width = 2560;
    CHECK(!check_create(bad, &why) && has(why, "upscales only"));
    bad = ci;
    bad.output_format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    CHECK(!check_create(bad, &why) && has(why, "format"));
    bad = ci;
    bad.output_texture_width = 1280, bad.output_texture_height = 720;
    CHECK(!check_create(bad, &why) && has(why, "smaller than the upscale size"));
    bad.output_texture_width = 1920, bad.output_texture_height = 1080;
    CHECK(check_create(bad, &why));
}

// Fake Vulkan handles
template <class T>
static T h(uintptr_t v) {
    return reinterpret_cast<T>(v);
}
static upscale::Texture tex(uintptr_t id, VkFormat f, uint32_t w, uint32_t hgt, VkImageLayout l = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    upscale::Texture t;
    t.image = h<VkImage>(id);
    t.view = h<VkImageView>(id + 1);
    t.format = f;
    t.width = w;
    t.height = hgt;
    t.layout = l;
    return t;
}
static upscale::Frame game_frame(uint64_t n = 1) {
    upscale::Frame f;
    f.frame = n;
    f.color = tex(0x100, VK_FORMAT_R16G16B16A16_SFLOAT, 960, 540);
    f.depth = tex(0x200, VK_FORMAT_R32_SFLOAT, 960, 540);
    f.motion = tex(0x300, VK_FORMAT_R16G16_SNORM, 960, 540);
    f.reactive = tex(0x400, VK_FORMAT_R8_UNORM, 960, 540);
    f.transparency = tex(0x500, VK_FORMAT_R8_UNORM, 960, 540);
    f.output = tex(0x600, VK_FORMAT_R16G16B16A16_SFLOAT, 1920, 1080, VK_IMAGE_LAYOUT_GENERAL);
    f.jitter_x = 0.25f, f.jitter_y = -0.375f;
    f.mv_scale_x = 960, f.mv_scale_y = 540;
    f.render_width = 960, f.render_height = 540, f.output_width = 1920, f.output_height = 1080;
    f.frame_ms = 16.6f, f.camera_near = 0.01f, f.camera_far = 10000, f.fov_y = 1.1345f, f.pre_exposure = 0.5f;
    f.cb = h<VkCommandBuffer>(0xcb);
    return f;
}

static void frame_mapping() {
    CHECK(aspect_of(VK_FORMAT_R32_SFLOAT) == VK_IMAGE_ASPECT_COLOR_BIT);  // the game's depth copy
    CHECK(aspect_of(VK_FORMAT_D32_SFLOAT) == VK_IMAGE_ASPECT_DEPTH_BIT);  // the first frame's
    CHECK(aspect_of(VK_FORMAT_D24_UNORM_S8_UINT) == VK_IMAGE_ASPECT_DEPTH_BIT);
    {
        // the first frame after a load: depth is the D32_SFLOAT image itself; XeSS gets its depth aspect
        upscale::Frame d32 = game_frame();
        d32.depth = tex(0x210, VK_FORMAT_D32_SFLOAT, 960, 540);
        std::string w;
        CHECK(check_frame(d32, 1920, 1080, kFlagInvertedDepth, &w));
        const ExecuteParams e = execute_params(d32, Tuning{}, kFlagInvertedDepth, true);
        CHECK(e.depth.format == VK_FORMAT_D32_SFLOAT && e.depth.range.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT && e.depth.image == h<VkImage>(0x210));
    }
    upscale::Frame f = game_frame();
    const uint32_t flags = kFlagInvertedDepth | kFlagResponsivePixelMask;
    std::string why;
    CHECK(check_frame(f, 1920, 1080, flags, &why));
    ExecuteParams p = execute_params(f, Tuning{}, flags, true);
    CHECK(p.color.image == f.color.image && p.color.view == f.color.view && p.color.format == VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK(p.color.width == 960 && p.color.height == 540 && p.color.range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
    CHECK(p.color.range.levelCount == 1 && p.color.range.layerCount == 1 && p.color.range.baseMipLevel == 0);
    CHECK(p.velocity.image == f.motion.image && p.velocity.format == VK_FORMAT_R16G16_SNORM);
    CHECK(p.depth.image == f.depth.image && p.responsive.image == f.reactive.image && p.output.image == f.output.image);
    CHECK(p.output.width == 1920 && p.exposure.image == VK_NULL_HANDLE);
    CHECK(p.jitter_x == 0.25f && p.jitter_y == -0.375f && p.exposure_scale == 1.0f && p.reset_history == 1);
    CHECK(p.input_width == 960 && p.input_height == 540 && p.color_base.x == 0 && p.output_base.y == 0);
    // no mask flag: no mask passed
    CHECK(execute_params(f, Tuning{}, kFlagInvertedDepth, false).responsive.image == VK_NULL_HANDLE);
    CHECK(execute_params(f, Tuning{}, flags, false).reset_history == 0);
    f.reset = true;
    CHECK(execute_params(f, Tuning{}, flags, false).reset_history == 1);
    f.reset = false;
    // knobs: flips, exposure
    Tuning t;
    t.jitter_y = -1;
    t.exposure = Exposure::inverse;
    p = execute_params(f, t, flags, false);
    CHECK(p.jitter_x == 0.25f && p.jitter_y == 0.375f && p.exposure_scale == 2.0f);
    f.pre_exposure = 0;
    CHECK(execute_params(f, t, flags, false).exposure_scale == 1.0f);
    f.jitter_x = 0.9f;
    CHECK(execute_params(f, Tuning{}, flags, false).jitter_x == 0.5f);
    f.jitter_x = NAN;
    CHECK(execute_params(f, Tuning{}, flags, false).jitter_x == 0.0f);
    // velocity scale: UV -> render pixels (the game's MV scale), with flips; 0 falls back to the render size
    f = game_frame();
    float vx = 0, vy = 0;
    velocity_scale(f, Tuning{}, &vx, &vy);
    CHECK(vx == 960 && vy == 540);
    Tuning m;
    m.mv_y = -1;
    f.mv_scale_x = 0;
    velocity_scale(f, m, &vx, &vy);
    CHECK(vx == 960 && vy == -540);
    // dynamic resolution: a frame smaller than the textures is fine; larger is not
    f = game_frame();
    f.render_width = 800, f.render_height = 450;
    CHECK(check_frame(f, 1920, 1080, flags, &why) && execute_params(f, Tuning{}, flags, false).input_width == 800);
    f.render_width = 1000;
    CHECK(!check_frame(f, 1920, 1080, flags, &why) && has(why, "larger than the colour"));
    // missing and wrong inputs
    f = game_frame();
    f.reactive = {};
    CHECK(!check_frame(f, 1920, 1080, flags, &why) && has(why, "reactive"));
    CHECK(check_frame(f, 1920, 1080, kFlagInvertedDepth, &why));
    f = game_frame();
    f.depth = {};
    CHECK(!check_frame(f, 1920, 1080, flags, &why) && has(why, "depth"));
    f = game_frame();
    f.output_width = 2560, f.output_height = 1440;
    CHECK(!check_frame(f, 1920, 1080, flags, &why) && has(why, "output texture is smaller"));
    f = game_frame();
    f.output.width = 2560, f.output.height = 1440;
    f.output_width = 2560, f.output_height = 1440;
    CHECK(!check_frame(f, 1920, 1080, flags, &why) && has(why, "set up for"));
    f = game_frame();
    f.output.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    CHECK(!check_frame(f, 1920, 1080, flags, &why) && has(why, "GENERAL"));
    f = game_frame();
    f.output.format = VK_FORMAT_R8G8B8A8_UNORM;
    CHECK(!check_frame(f, 1920, 1080, flags, &why) && has(why, "format"));
    f = game_frame();
    f.cb = VK_NULL_HANDLE;
    CHECK(!check_frame(f, 1920, 1080, flags, &why));
}

static void needs_lists() {
    const upscale::DeviceNeeds d = documented_device_needs();
    CHECK(d.extensions.size() == 3 && d.extensions[0].name == "VK_EXT_mutable_descriptor_type" && d.extensions[0].required);
    CHECK(!d.extensions[1].required && !d.extensions[2].required);
    CHECK(d.features.size() == 4 && d.features[0].required && d.features[1].required && !d.features[2].required);
    CHECK(documented_instance_needs().min_api_version == VK_API_VERSION_1_1 && documented_instance_needs().extensions.empty());
    // XeSS's own chain, copied (its memory only read), all required
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT m{};
    m.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT;
    m.mutableDescriptorType = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    f2.pNext = &m;
    const char* exts[] = {"VK_EXT_mutable_descriptor_type", ""};
    upscale::DeviceNeeds n;
    std::string why;
    CHECK(needs_from_xess(2, exts, &f2, &n, &why));
    CHECK(n.extensions.size() == 1 && n.extensions[0].required && n.features.size() == 2 && n.features[0].required && n.features[1].required);
    CHECK(reinterpret_cast<VkBaseInStructure*>(n.features[0].bytes.data())->pNext == nullptr && f2.pNext == &m);
    CHECK(n.features[1].bytes.size() == sizeof m);
    struct {
        VkStructureType sType;
        void* pNext;
        VkBool32 x;
    } odd{static_cast<VkStructureType>(1999999000), nullptr, VK_TRUE};
    m.pNext = &odd;
    upscale::DeviceNeeds n2;
    CHECK(!needs_from_xess(0, nullptr, &f2, &n2, &why) && has(why, "1999999000"));
    m.pNext = nullptr;
    // checked on a device under either name
    upscale::VkContext vk;
    vk.extensions = {"VK_EXT_mutable_descriptor_type"};
    VkPhysicalDeviceVulkan12Features v12{};
    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    v12.shaderInt8 = VK_TRUE;
    auto as_blob = [](const auto& s) {
        upscale::FeatureNeed f;
        f.bytes.assign(reinterpret_cast<const uint8_t*>(&s), reinterpret_cast<const uint8_t*>(&s) + sizeof s);
        return f;
    };
    vk.features = {as_blob(f2), as_blob(v12)};
    auto missing = missing_on_device(vk, d, true);
    // mutable: not on (the device's mutable struct absent); dot product and its extension absent; shaderInt8 on as Vulkan12
    CHECK(missing.size() == 4);
    CHECK(missing[0] == "VK_KHR_shader_float16_int8" && missing[1] == "VK_KHR_shader_integer_dot_product");
    CHECK(missing[2] == "VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT +16");
    CHECK(missing[3] == "VkPhysicalDeviceShaderIntegerDotProductFeatures.shaderIntegerDotProduct");
    CHECK(missing_on_device(vk, d).size() == 1);  // required only: the mutable feature
    vk.features.push_back(as_blob(m));
    CHECK(missing_on_device(vk, d).empty());
    CHECK(join({"a", "b", "c", "d", "e", "f"}) == "a, b, c, d and 2 more");
}

// ---- a fake libxess ---------------------------------------------------------------------------------------------------
namespace fx {
struct Ctx {
    int id;
    bool inited = false;
    InitParams init{};
    float vx = 0, vy = 0;
    int executes = 0;
    bool destroyed = false;
};
std::vector<Ctx*> all;
int32_t create_result = kOk, init_result = kOk, execute_result = kOk, device_ext_result = kOk, instance_result = kOk;
Version version{3, 0, 2, 0};
int destroyed = 0, set_vel = 0, log_calls = 0;
ExecuteParams last{};
VkCommandBuffer last_cb = VK_NULL_HANDLE;
LogFn logger = nullptr;
VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mut{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, nullptr, VK_TRUE};
VkPhysicalDeviceFeatures2 f2 = [] {
    VkPhysicalDeviceFeatures2 f{};
    f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    f.pNext = &mut;
    return f;
}();
const char* dev_exts[] = {"VK_EXT_mutable_descriptor_type"};
const char* inst_exts[] = {"VK_KHR_get_physical_device_properties2"};
void reset() {
    for (auto* c : all) delete c;
    all.clear();
    create_result = init_result = execute_result = device_ext_result = instance_result = kOk;
    version = {3, 0, 2, 0};
    destroyed = set_vel = log_calls = 0;
    last = {};
    logger = nullptr;
}
int32_t get_version(Version* v) { return *v = version, kOk; }
int32_t inst(uint32_t* n, const char* const** names, uint32_t* api) {
    if (instance_result < 0) return instance_result;
    *n = 1, *names = inst_exts, *api = VK_API_VERSION_1_1;
    return kOk;
}
int32_t dev(VkInstance, VkPhysicalDevice, uint32_t* n, const char* const** names) {
    if (device_ext_result < 0) return device_ext_result;
    *n = 1, *names = dev_exts;
    return kOk;
}
int32_t feats(VkInstance, VkPhysicalDevice, void** chain) { return *chain = &f2, kOk; }
int32_t create(VkInstance, VkPhysicalDevice, VkDevice, Context* out) {
    if (create_result < 0) return create_result;
    auto* c = new Ctx{static_cast<int>(all.size())};
    all.push_back(c);
    *out = reinterpret_cast<Context>(c);
    return kOk;
}
Ctx* of(Context c) { return reinterpret_cast<Ctx*>(c); }
int32_t init(Context c, const InitParams* p) {
    if (init_result < 0) return init_result;
    of(c)->inited = true;
    of(c)->init = *p;
    return kOk;
}
int32_t execute(Context c, VkCommandBuffer cb, const ExecuteParams* p) {
    CHECK(!of(c)->destroyed && of(c)->inited);
    if (execute_result < 0) return execute_result;
    ++of(c)->executes;
    last = *p;
    last_cb = cb;
    return kOk;
}
int32_t destroy(Context c) {
    CHECK(!of(c)->destroyed);
    of(c)->destroyed = true;
    ++destroyed;
    return kOk;
}
// XeSS's ranges as the guide gives them: 1.0x down to the preset's ratio, the low end rounded up (strict); Native AA
// only the output size itself
int32_t optimal(Context, const Dim* o, int32_t q, Dim* opt, Dim* lo, Dim* hi) {
    const float r = preset_of(static_cast<Quality>(q)).ratio;
    *opt = {static_cast<uint32_t>(std::lround(o->x / r)), static_cast<uint32_t>(std::lround(o->y / r))};
    *lo = {static_cast<uint32_t>(std::ceil(o->x / r - 1e-3)), static_cast<uint32_t>(std::ceil(o->y / r - 1e-3))};
    *hi = *o;
    return kOk;
}
int32_t vel(Context c, float x, float y) {
    ++set_vel;
    of(c)->vx = x, of(c)->vy = y;
    return kOk;
}
int32_t set_log(Context, int32_t, LogFn fn) { return logger = fn, kOk; }
int32_t optimal_driver(Context) { return kWarnOldDriver; }
Api api() {
    Api a;
    a.get_version = &get_version;
    a.vk_instance_extensions = &inst;
    a.vk_device_extensions = &dev;
    a.vk_device_features = &feats;
    a.vk_create_context = &create;
    a.vk_init = &init;
    a.vk_execute = &execute;
    a.destroy_context = &destroy;
    a.optimal_input_resolution = &optimal;
    a.set_velocity_scale = &vel;
    a.set_logging_callback = &set_log;
    a.is_optimal_driver = &optimal_driver;
    return a;
}
// the device's pipeline cache
int caches = 0, caches_destroyed = 0;
VKAPI_ATTR VkResult VKAPI_CALL create_cache(VkDevice, const VkPipelineCacheCreateInfo*, const VkAllocationCallbacks*, VkPipelineCache* out) {
    *out = h<VkPipelineCache>(0xcac0 + static_cast<uintptr_t>(++caches));
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_cache(VkDevice, VkPipelineCache, const VkAllocationCallbacks*) { ++caches_destroyed; }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice, const char* n) {
    if (!std::strcmp(n, "vkCreatePipelineCache")) return reinterpret_cast<PFN_vkVoidFunction>(&create_cache);
    if (!std::strcmp(n, "vkDestroyPipelineCache")) return reinterpret_cast<PFN_vkVoidFunction>(&destroy_cache);
    return nullptr;
}
}  // namespace fx

// The core's motion-vector copy, faked: hands out Kiln's image 0x700 in R16G16_SFLOAT.
struct FakeCopy final : MotionCopy {
    bool fail_init = false, fail_record = false;
    int *inits, *records, *destroys;
    FakeCopy(int* i, int* r, int* d) : inits(i), records(r), destroys(d) {}
    bool init(const upscale::VkContext& vk, std::string* detail) override {
        CHECK(vk.device != VK_NULL_HANDLE);
        ++*inits;
        if (fail_init) return *detail = "no memory type", false;
        return true;
    }
    bool record(VkCommandBuffer cb, const upscale::Texture& in, uint64_t, upscale::Texture* out, std::string* detail) override {
        CHECK(cb != VK_NULL_HANDLE && in.format == VK_FORMAT_R16G16_SNORM);
        ++*records;
        if (fail_record) return *detail = "a descriptor set", false;
        *out = tex(0x700, VK_FORMAT_R16G16_SFLOAT, in.width, in.height);
        return true;
    }
    void destroy() override { ++*destroys; }
    void want(upscale::DeviceNeeds* needs) override { needs->want_extension("VK_KILN_test_copy"); }
    void tick(uint64_t) override { ++*ticks; }
    int* ticks = nullptr;
};

struct FakeHost final : Host {
    Api api;
    bool fail_load = false;
    std::string tuning;
    std::vector<std::string>* lines;
    bool copy = false, copy_fail_init = false, copy_fail_record = false;
    int copy_inits = 0, copy_records = 0, copy_destroys = 0, copy_ticks = 0;
    explicit FakeHost(std::vector<std::string>* l) : api(fx::api()), lines(l) {}
    std::unique_ptr<MotionCopy> motion_copy() override {
        if (!copy) return nullptr;
        auto c = std::make_unique<FakeCopy>(&copy_inits, &copy_records, &copy_destroys);
        c->ticks = &copy_ticks;
        c->fail_init = copy_fail_init;
        c->fail_record = copy_fail_record;
        return c;
    }
    bool load_library(const std::wstring& path, Api* out, std::string* detail) override {
        CHECK(path.size() > 11 && path.substr(path.size() - 11) == L"LibXeSS.dll");  // found whatever its case
        if (fail_load) return *detail = "libxess.dll did not load (Windows error 126: a library it needs is missing)", false;
        *out = api;
        return true;
    }
    std::string read_tuning(const upscale::Files&) override { return tuning; }
    void log(int, const std::string& line) override { lines->push_back(line); }
};

static upscale::Files files() {
    upscale::Files f;
    f.dir = L"C:\\Games\\Enshrouded\\kiln\\vendor\\intel\\3.0.2";
    f.paths = {f.dir + L"\\LibXeSS.dll"};
    f.version = "3.0.2";
    return f;
}
static upscale::VkContext device(bool with_xess) {
    upscale::VkContext vk;
    vk.instance = h<VkInstance>(0x11);
    vk.physical = h<VkPhysicalDevice>(0x22);
    vk.device = h<VkDevice>(0x33);
    vk.gdpa = &fx::gdpa;
    vk.instance_api = VK_API_VERSION_1_2;
    vk.props.vendorID = 0x10de;
    std::strcpy(vk.props.deviceName, "NVIDIA GeForce RTX 3080 Ti");
    vk.extensions = {"VK_KHR_swapchain"};
    vk.instance_extensions = {"VK_KHR_surface", "VK_KHR_win32_surface", "VK_KHR_get_physical_device_properties2"};
    vk.needs_merged = with_xess;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    upscale::FeatureNeed a;
    a.bytes.assign(reinterpret_cast<uint8_t*>(&f2), reinterpret_cast<uint8_t*>(&f2) + sizeof f2);
    vk.features.push_back(a);
    if (with_xess) {
        vk.extensions.push_back("VK_EXT_mutable_descriptor_type");
        upscale::FeatureNeed b;
        b.bytes.assign(reinterpret_cast<uint8_t*>(&fx::mut), reinterpret_cast<uint8_t*>(&fx::mut) + sizeof fx::mut);
        reinterpret_cast<VkBaseOutStructure*>(b.bytes.data())->pNext = nullptr;
        vk.features.push_back(b);
    }
    return vk;
}
static upscale::PhysicalDevice gpu() {
    upscale::PhysicalDevice g;
    g.instance = h<VkInstance>(0x11);
    g.physical = h<VkPhysicalDevice>(0x22);
    std::strcpy(g.props.deviceName, "NVIDIA GeForce RTX 3080 Ti");
    return g;
}

static void render_scale_output() {
    // the game's 3D render scale: the upscale size (B0: 1885x1061) inside a 1920x1080 output texture. XeSS is set up
    // for the upscale size and writes it at the texture's top left (outputColorBase 0,0); the image info names the
    // whole texture
    fx::reset();
    std::vector<std::string> log;
    upscale::Why why;
    std::string detail;
    XessBackend b(std::make_unique<FakeHost>(&log));
    CHECK(b.load(files(), &why, &detail));
    b.device_needs(gpu());
    CHECK(b.device_ready(device(true), &why, &detail));
    upscale::CreateInfo ci = game_ci();
    ci.output_width = 1885, ci.output_height = 1061;
    ci.max_render_width = 943, ci.max_render_height = 531;
    ci.ratio = 2.0f;
    CHECK(b.create(ci, &detail));
    CHECK(fx::all[0]->init.output.x == 1885 && fx::all[0]->init.output.y == 1061);
    upscale::Frame f = game_frame();
    f.output_width = 1885, f.output_height = 1061;
    f.render_width = 943, f.render_height = 531, f.mv_scale_x = 943, f.mv_scale_y = 531;
    CHECK(b.record(f, &detail));
    CHECK(fx::last.output.width == 1920 && fx::last.output.height == 1080 && fx::last.output_base.x == 0 && fx::last.input_width == 943);
    // a frame of another upscale size than the context's never reaches XeSS (the core sets a new one up first)
    f.output_width = 1920, f.output_height = 1080;
    CHECK(!b.record(f, &detail) && has(detail, "set up for"));
    b.device_lost(h<VkDevice>(0x33));
}

static void load_failures() {
    std::vector<std::string> log;
    upscale::Why why = upscale::Why::none;
    std::string detail;
    {
        auto host = std::make_unique<FakeHost>(&log);
        host->fail_load = true;
        XessBackend b(std::move(host));
        CHECK(!b.load(files(), &why, &detail) && why == upscale::Why::load_failed && has(detail, "error 126") && !b.loaded());
        CHECK(b.version() == 0 && b.name() == "Intel XeSS 3");
    }
    {
        auto host = std::make_unique<FakeHost>(&log);
        host->api.vk_execute = nullptr;
        XessBackend b(std::move(host));
        CHECK(!b.load(files(), &why, &detail) && why == upscale::Why::load_failed && detail == "libxess.dll has no export xessVKExecute");
    }
    {
        fx::reset();
        fx::version = {2, 1, 0, 0};
        XessBackend b(std::make_unique<FakeHost>(&log));
        CHECK(!b.load(files(), &why, &detail) && why == upscale::Why::load_failed && has(detail, "XeSS 2.1.0"));
        fx::version = {3, 0, 2, 0};
    }
    {
        XessBackend b(std::make_unique<FakeHost>(&log));
        upscale::Files f = files();
        f.paths = {f.dir + L"\\amd_fidelityfx_loader_vk.dll"};
        CHECK(!b.load(f, &why, &detail) && why == upscale::Why::files_missing);
    }
}

static void needs_and_device() {
    fx::reset();
    std::vector<std::string> log;
    upscale::Why why = upscale::Why::none;
    std::string detail;
    // before load: the guide's needs
    {
        XessBackend b(std::make_unique<FakeHost>(&log));
        CHECK(b.choice() == upscale::Choice::xess && std::string(b.component()) == "intel-xess" && b.running() == upscale::Running::xess);
        CHECK(b.instance_needs().min_api_version == VK_API_VERSION_1_1);
        CHECK(b.device_needs(gpu()).extensions.size() == 3);
        // loaded later: device_ready asks XeSS itself and checks the device against that
        CHECK(b.load(files(), &why, &detail));
        CHECK(b.device_ready(device(true), &why, &detail));
        CHECK(!b.device_ready(device(false), &why, &detail) && why == upscale::Why::device && has(detail, "VK_EXT_mutable_descriptor_type"));
    }
    // loaded first (the order the core uses): XeSS's own answers
    {
        XessBackend b(std::make_unique<FakeHost>(&log));
        CHECK(b.load(files(), &why, &detail) && b.loaded() && b.version() == upscale::make_version(3, 0, 2) && b.name() == "Intel XeSS 3.0.2");
        CHECK(b.load(files(), &why, &detail));  // twice: still loaded, nothing again
        const upscale::InstanceNeeds in = b.instance_needs();
        CHECK(in.min_api_version == VK_API_VERSION_1_1 && in.extensions.size() == 1 && in.extensions[0].name == "VK_KHR_get_physical_device_properties2" &&
              in.extensions[0].required);
        const upscale::DeviceNeeds dn = b.device_needs(gpu());
        CHECK(dn.extensions.size() == 1 && dn.features.size() == 2 && dn.features[0].required && dn.features[1].required);
        // the device without them: device
        CHECK(!b.device_ready(device(false), &why, &detail) && why == upscale::Why::device);
        // the merge refused (needs_merged false): Kiln could not give them, no restart helps
        CHECK(!b.device_ready(device(false), &why, &detail) && why == upscale::Why::device && has(detail, "Kiln could not give"));
        // an instance without XeSS's instance extension, or below its API version
        upscale::VkContext no_ext = device(true);
        no_ext.instance_extensions.pop_back();
        CHECK(!b.device_ready(no_ext, &why, &detail) && why == upscale::Why::device && has(detail, "VK_KHR_get_physical_device_properties2 (instance)"));
        upscale::VkContext old = device(true);
        old.instance_api = VK_API_VERSION_1_0;
        CHECK(!b.device_ready(old, &why, &detail) && has(detail, "a Vulkan 1.1 instance"));
        // with them: ready, and Kiln's pipeline cache made
        fx::caches = fx::caches_destroyed = 0;
        CHECK(b.device_ready(device(true), &why, &detail) && fx::caches == 1);
        b.device_lost(h<VkDevice>(0x33));
        CHECK(fx::caches_destroyed == 1);
    }
    // XeSS chosen after the game's start: the device was created without asking it
    {
        XessBackend b(std::make_unique<FakeHost>(&log));
        CHECK(b.load(files(), &why, &detail));
        CHECK(!b.device_ready(device(false), &why, &detail) && why == upscale::Why::restart && has(detail, "next start"));
        // unless the device has what XeSS needs anyway
        CHECK(b.device_ready(device(true), &why, &detail));
    }
    // XeSS says the device cannot run it
    {
        fx::device_ext_result = kErrUnsupportedDriver;
        XessBackend b(std::make_unique<FakeHost>(&log));
        CHECK(b.load(files(), &why, &detail));
        CHECK(b.device_needs(gpu()).extensions.empty());
        CHECK(!b.device_ready(device(true), &why, &detail) && why == upscale::Why::device && has(detail, "driver is not supported"));
        fx::device_ext_result = kOk;
    }
    {
        fx::instance_result = kErrUnsupportedDriver;
        XessBackend b(std::make_unique<FakeHost>(&log));
        CHECK(b.load(files(), &why, &detail));
        CHECK(b.instance_needs().extensions.empty());
        b.device_needs(gpu());
        CHECK(!b.device_ready(device(true), &why, &detail) && why == upscale::Why::device);
        fx::instance_result = kOk;
    }
    // no device handles
    {
        XessBackend b(std::make_unique<FakeHost>(&log));
        CHECK(b.load(files(), &why, &detail));
        upscale::VkContext none;
        CHECK(!b.device_ready(none, &why, &detail) && why == upscale::Why::no_vulkan);
    }
    // not loaded at all
    {
        XessBackend b(std::make_unique<FakeHost>(&log));
        CHECK(!b.device_ready(device(true), &why, &detail) && why == upscale::Why::load_failed);
        CHECK(!b.create(game_ci(), &detail));
    }
}

static void contexts() {
    fx::reset();
    std::vector<std::string> log;
    upscale::Why why;
    std::string detail;
    auto host = std::make_unique<FakeHost>(&log);
    host->tuning = "log info\n";
    XessBackend b(std::move(host));
    CHECK(b.load(files(), &why, &detail));
    b.device_needs(gpu());
    CHECK(b.device_ready(device(true), &why, &detail));
    CHECK(!b.record(game_frame(), &detail) && has(detail, "no context"));
    // Fsr3Create: 960x540 -> 1920x1080, ratio 2 (FSR Performance) -> XeSS Balanced
    CHECK(b.create(game_ci(), &detail) && b.has_context() && b.quality() == kBalanced);
    CHECK(fx::all.size() == 1 && fx::all[0]->inited);
    const InitParams ip = fx::all[0]->init;
    CHECK(ip.output.x == 1920 && ip.output.y == 1080 && ip.quality == kBalanced && ip.flags == (kFlagInvertedDepth | kFlagResponsivePixelMask));
    CHECK(ip.pipeline_cache != VK_NULL_HANDLE && ip.temp_buffer_heap == VK_NULL_HANDLE && ip.temp_texture_heap == VK_NULL_HANDLE);
    CHECK(fx::logger != nullptr);
    // XeSS's own log lines reach kiln.log
    const size_t before = log.size();
    fx::logger("hello from XeSS", kLogWarning);
    CHECK(log.size() == before + 1 && log.back() == "XeSS says: hello from XeSS");
    bool driver_warned = false, set_up = false;
    for (const auto& l : log) driver_warned |= has(l, "driver is old"), set_up |= has(l, "set up: 960x540 -> 1920x1080 (ratio 2.00, Balanced)");
    CHECK(driver_warned && set_up);
    // the first frame: history reset, velocity scale set once
    CHECK(b.record(game_frame(1), &detail));
    CHECK(fx::last.reset_history == 1 && fx::set_vel == 1 && fx::all[0]->vx == 960 && fx::all[0]->vy == 540 && fx::last_cb == h<VkCommandBuffer>(0xcb));
    CHECK(b.record(game_frame(2), &detail) && fx::last.reset_history == 0 && fx::set_vel == 1);
    b.reset();
    CHECK(b.record(game_frame(3), &detail) && fx::last.reset_history == 1);
    CHECK(b.record(game_frame(4), &detail) && fx::last.reset_history == 0);
    // dynamic resolution: a smaller render size, the velocity scale follows the game's MV scale
    upscale::Frame small = game_frame(5);
    small.render_width = 800, small.render_height = 450, small.mv_scale_x = 800, small.mv_scale_y = 450;
    CHECK(b.record(small, &detail) && fx::set_vel == 2 && fx::all[0]->vx == 800 && fx::last.input_width == 800);
    // below Balanced's range for 1920 (960 and up): said once, XeSS still gets the frame
    int range_lines = 0;
    for (const auto& l : log) range_lines += has(l, "this frame, outside XeSS's Balanced range 960x540 to 1920x1080");
    CHECK(range_lines == 1);
    CHECK(b.record(small, &detail));
    range_lines = 0;
    for (const auto& l : log) range_lines += has(l, "this frame, outside XeSS's");
    CHECK(range_lines == 1);
    // a quality change: Fsr3Create again (the core destroys first): a new context, the old one retired, not destroyed
    b.destroy();
    upscale::CreateInfo q = game_ci();
    q.max_render_width = 1280, q.max_render_height = 720, q.ratio = 1.5f;
    CHECK(b.create(q, &detail) && b.quality() == kUltraQuality && fx::all.size() == 2 && b.retired() == 1 && fx::destroyed == 0);
    CHECK(!fx::all[0]->destroyed);
    upscale::Frame f = game_frame(6);
    f.color.width = f.depth.width = f.motion.width = f.reactive.width = 1280;
    f.color.height = f.depth.height = f.motion.height = f.reactive.height = 720;
    f.render_width = 1280, f.render_height = 720, f.mv_scale_x = 1280, f.mv_scale_y = 720;
    // the old context last ran in frame 5: frames 6, 7 and 8 may still have it on the GPU, frame 9 does not
    for (int i = 0; i < 3; ++i) {
        f.frame = 6 + static_cast<uint64_t>(i);
        CHECK(b.record(f, &detail));
        CHECK(!fx::all[0]->destroyed);  // its last frame may still run on the GPU
    }
    CHECK(fx::all[1]->executes == 3 && fx::last.reset_history == 0);
    f.frame = 9;
    CHECK(b.record(f, &detail) && fx::all[0]->destroyed && b.retired() == 0 && fx::destroyed == 1);
    // the first frame of the new context reset history
    CHECK(fx::all[1]->executes == 4);
    // a failure while upscaling: record says so (the core falls back)
    fx::execute_result = kErrInvalidArgument;
    CHECK(!b.record(f, &detail) && has(detail, "an invalid argument"));
    fx::execute_result = kOk;
    // a frame XeSS cannot take
    upscale::Frame bad = f;
    bad.output_width = 2560;
    CHECK(!b.record(bad, &detail) && has(detail, "cannot take this frame"));
    // init fails: that context is destroyed at once (never recorded), the old one stays retired
    b.destroy();
    fx::init_result = kErrDeviceOutOfMemory;
    CHECK(!b.create(game_ci(), &detail) && has(detail, "out of GPU memory") && !b.has_context());
    CHECK(fx::all.size() == 3 && fx::all[2]->destroyed && !fx::all[1]->destroyed && b.retired() == 1);
    fx::init_result = kOk;
    fx::create_result = kErrUnsupportedDevice;
    CHECK(!b.create(game_ci(), &detail) && has(detail, "xessVKCreateContext"));
    fx::create_result = kOk;
    // bad sizes never reach XeSS
    upscale::CreateInfo z = game_ci();
    z.output_width = 0;
    const size_t made = fx::all.size();
    CHECK(!b.create(z, &detail) && fx::all.size() == made);
    // the device goes (idle): everything is destroyed, the cache too
    CHECK(b.create(game_ci(), &detail));
    fx::caches_destroyed = 0;
    b.device_lost(h<VkDevice>(0x99));  // another device: nothing
    CHECK(b.has_context() && fx::caches_destroyed == 0);
    b.device_lost(h<VkDevice>(0x33));
    CHECK(!b.has_context() && b.retired() == 0 && fx::caches_destroyed == 1);
    for (auto* c : fx::all) CHECK(c->destroyed);
    CHECK(!b.create(game_ci(), &detail));  // no device any more
}

static void knobs_reach_xess() {
    fx::reset();
    std::vector<std::string> log;
    upscale::Why why;
    std::string detail;
    auto host = std::make_unique<FakeHost>(&log);
    host->tuning = "jitter-y flip\nmotion-y flip\nexposure inverse\nmask off\n";
    Host* host_ptr = host.get();
    XessBackend b(std::move(host));
    CHECK(b.load(files(), &why, &detail));
    bool said = false;
    for (const auto& l : log) said |= has(l, "test knobs in use");
    CHECK(said && b.tuning().tuned());
    b.device_needs(gpu());
    CHECK(b.device_ready(device(true), &why, &detail));
    CHECK(b.create(game_ci(), &detail) && b.flags() == kFlagInvertedDepth);  // mask off
    upscale::Frame f = game_frame();
    f.reactive = {};  // not needed without the mask
    CHECK(b.record(f, &detail));
    CHECK(fx::last.jitter_y == 0.375f && fx::last.exposure_scale == 2.0f && fx::last.responsive.image == VK_NULL_HANDLE);
    CHECK(fx::all[0]->vy == -540);
    // a knob changed during the test applies at the next create (the game's next FSR set-up), and is said
    static_cast<FakeHost*>(host_ptr)->tuning = "mask on\n";
    b.destroy();
    CHECK(b.create(game_ci(), &detail) && b.flags() == (kFlagInvertedDepth | kFlagResponsivePixelMask) && !b.tuning().tuned());
    bool said_default = false;
    for (const auto& l : log) said_default |= has(l, "every knob at its default");
    CHECK(said_default);
    b.device_lost(h<VkDevice>(0x33));
}

static void motion_copy() {
    std::vector<std::string> log;
    upscale::Why why;
    std::string detail;
    // with the core's copy: XeSS reads Kiln's R16G16_SFLOAT image, the game's own stays as it is
    {
        fx::reset();
        auto host = std::make_unique<FakeHost>(&log);
        host->copy = true;
        FakeHost* hp = host.get();
        XessBackend b(std::move(host));
        CHECK(b.load(files(), &why, &detail));
        const upscale::DeviceNeeds dn = b.device_needs(gpu());
        CHECK(dn.extensions.size() == 2 && dn.extensions[1].name == "VK_KILN_test_copy" && !dn.extensions[1].required);
        // the copy's optional need is not something device_ready insists on
        CHECK(b.device_ready(device(true), &why, &detail) && hp->copy_inits == 1);
        CHECK(b.create(game_ci(), &detail));
        CHECK(b.record(game_frame(), &detail) && hp->copy_records == 1);
        CHECK(fx::last.velocity.image == h<VkImage>(0x700) && fx::last.velocity.format == VK_FORMAT_R16G16_SFLOAT && fx::last.velocity.width == 960);
        CHECK(fx::last.color.image == h<VkImage>(0x100));
        CHECK(fx::all[0]->vx == 960);  // values unchanged: the same velocity scale
        // the knob turns it off at the next set-up
        hp->tuning = "motion-copy off\n";
        b.destroy();
        CHECK(b.create(game_ci(), &detail) && b.record(game_frame(2), &detail) && hp->copy_records == 1);
        CHECK(fx::last.velocity.image == h<VkImage>(0x300) && fx::last.velocity.format == VK_FORMAT_R16G16_SNORM);
        hp->tuning = "";
        b.destroy();
        CHECK(b.create(game_ci(), &detail) && b.record(game_frame(3), &detail) && hp->copy_records == 2);
        b.device_lost(h<VkDevice>(0x33));
        CHECK(hp->copy_destroys == 1);
    }
    // a copy that cannot start: XeSS reads the game's image
    {
        fx::reset();
        auto host = std::make_unique<FakeHost>(&log);
        host->copy = true;
        host->copy_fail_init = true;
        FakeHost* hp = host.get();
        XessBackend b(std::move(host));
        CHECK(b.load(files(), &why, &detail));
        b.device_needs(gpu());
        CHECK(b.device_ready(device(true), &why, &detail) && hp->copy_inits == 1);
        CHECK(b.create(game_ci(), &detail) && b.record(game_frame(), &detail) && fx::last.velocity.image == h<VkImage>(0x300));
        bool said = false;
        for (const auto& l : log) said |= has(l, "motion-vector copy for XeSS could not start (no memory type)");
        CHECK(said);
        b.device_lost(h<VkDevice>(0x33));
        CHECK(hp->copy_destroys == 0);
    }
    // a copy that fails in a frame: that frame fails (the core falls back), XeSS is not called with half a frame
    {
        fx::reset();
        auto host = std::make_unique<FakeHost>(&log);
        host->copy = true;
        host->copy_fail_record = true;
        XessBackend b(std::move(host));
        CHECK(b.load(files(), &why, &detail));
        b.device_needs(gpu());
        CHECK(b.device_ready(device(true), &why, &detail) && b.create(game_ci(), &detail));
        CHECK(!b.record(game_frame(), &detail) && has(detail, "motion-vector copy for XeSS failed: a descriptor set") && fx::all[0]->executes == 0);
        b.device_lost(h<VkDevice>(0x33));
    }
}

static void lifetime() {
    // the backend object goes (the process's end, or a core that deletes it early): it calls nothing of XeSS's, so the
    // context in use and the retired one stay; XeSS's log callback stops reaching kiln.log
    fx::reset();
    std::vector<std::string> log;
    upscale::Why why;
    std::string detail;
    {
        XessBackend b(std::make_unique<FakeHost>(&log));
        CHECK(b.load(files(), &why, &detail));
        b.device_needs(gpu());
        CHECK(b.device_ready(device(true), &why, &detail));
        CHECK(b.create(game_ci(), &detail));  // context 0: used in frame 1, retired, gone by frame 5
        CHECK(b.record(game_frame(1), &detail));
        b.destroy();
        CHECK(b.create(game_ci(), &detail));
        for (uint64_t i = 2; i <= 5; ++i) CHECK(b.record(game_frame(i), &detail));
        CHECK(fx::all[0]->destroyed);
        b.destroy();  // context 1: last used in frame 5, retired just now
        CHECK(b.create(game_ci(), &detail));  // context 2: in use
        CHECK(b.record(game_frame(6), &detail));
    }
    CHECK(!fx::all[1]->destroyed && !fx::all[2]->destroyed);  // the destructor calls nothing of XeSS's
    const size_t n = log.size();
    fx::logger("after the backend", kLogError);  // XeSS calling back late: nobody listens, nothing breaks
    CHECK(log.size() == n);
}

static void ticks() {
    // the core's tick on every FSR frame frees retired contexts with no record of XeSS's (a fall back, "game" chosen);
    // a context never recorded goes at once; the motion copy is ticked too
    fx::reset();
    std::vector<std::string> log;
    upscale::Why why;
    std::string detail;
    auto host = std::make_unique<FakeHost>(&log);
    host->copy = true;
    FakeHost* hp = host.get();
    XessBackend b(std::move(host));
    CHECK(b.load(files(), &why, &detail));
    b.device_needs(gpu());
    CHECK(b.device_ready(device(true), &why, &detail));
    CHECK(b.create(game_ci(), &detail));
    b.destroy();  // never recorded: destroyed at once
    CHECK(fx::all[0]->destroyed && b.retired() == 0);
    CHECK(b.create(game_ci(), &detail) && b.record(game_frame(100), &detail));
    b.destroy();  // the core falls back: no more records
    CHECK(b.retired() == 1);
    b.tick(101), b.tick(102), b.tick(103);
    CHECK(!fx::all[1]->destroyed && hp->copy_ticks == 3);
    b.tick(104);
    CHECK(fx::all[1]->destroyed && b.retired() == 0 && hp->copy_ticks == 4);
    // a record that fails still counts as a use of that frame
    CHECK(b.create(game_ci(), &detail));
    fx::execute_result = kErrDevice;
    CHECK(!b.record(game_frame(105), &detail));
    fx::execute_result = kOk;
    b.destroy();
    CHECK(b.retired() == 1 && !fx::all[2]->destroyed);
    b.tick(108);
    CHECK(!fx::all[2]->destroyed);
    b.tick(109);
    CHECK(fx::all[2]->destroyed);
    b.device_lost(h<VkDevice>(0x33));
}

static void ratios_of_the_game() {
    // every FSR 3 preset of the game (0xfa440: 3.0, 2.0, 1.7, 1.5, 1.0) at 1080p and 4K, as the core creates them
    fx::reset();
    std::vector<std::string> log;
    upscale::Why why;
    std::string detail;
    XessBackend b(std::make_unique<FakeHost>(&log));
    CHECK(b.load(files(), &why, &detail));
    b.device_needs(gpu());
    CHECK(b.device_ready(device(true), &why, &detail));
    // the game's render size rounded down (as a floor would): 1920 / 1.7 = 1129.4 -> 1129, a pixel below Quality's
    // 1130, so Balanced takes it; 1080 / 1.5 = 720 exactly, so Ultra Quality does
    struct Case {
        uint32_t out_w;
        float r;
        Quality q;
    };
    const Case want[] = {{1920, 3.0f, kUltraPerformance}, {1920, 2.0f, kBalanced}, {1920, 1.7f, kBalanced}, {1920, 1.5f, kUltraQuality},
                         {1920, 1.0f, kNativeAA},         {3840, 3.0f, kUltraPerformance}, {3840, 2.0f, kBalanced}, {3840, 1.7f, kBalanced},
                         {3840, 1.5f, kUltraQuality},     {3840, 1.0f, kNativeAA}};
    for (const auto& c : want) {
        upscale::CreateInfo ci = game_ci();
        ci.output_width = c.out_w, ci.output_height = c.out_w * 9 / 16;
        ci.max_render_width = static_cast<uint32_t>(ci.output_width / c.r);
        ci.max_render_height = static_cast<uint32_t>(ci.output_height / c.r);
        ci.ratio = c.r;
        b.destroy();
        CHECK(b.create(ci, &detail) && b.quality() == c.q);
    }
    bool outside = false;
    for (const auto& l : log) outside |= has(l, "outside XeSS's range");
    CHECK(!outside);
    // a render size no preset holds (larger than the output is refused before; here below Ultra Performance's low end)
    upscale::CreateInfo tiny = game_ci();
    tiny.max_render_width = 400, tiny.max_render_height = 225, tiny.ratio = 4.8f;
    b.destroy();
    CHECK(b.create(tiny, &detail) && b.quality() == kUltraPerformance);
    outside = false;
    for (const auto& l : log) outside |= has(l, "outside XeSS's range for every preset from Ultra Performance");
    CHECK(outside);
    b.device_lost(h<VkDevice>(0x33));
    CHECK(b.contexts_made() == 11);
}

int main() {
    presets();
    knobs();
    flags_and_create_checks();
    frame_mapping();
    render_scale_output();
    needs_lists();
    load_failures();
    needs_and_device();
    contexts();
    knobs_reach_xess();
    motion_copy();
    lifetime();
    ticks();
    ratios_of_the_game();
    fx::reset();
    std::printf("xess_test: %d checks passed\n", g_checks);
    return 0;
}
