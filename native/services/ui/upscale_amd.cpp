// SPDX-License-Identifier: MIT
// kiln.ui upscaler swap: the AMD FSR backend (upscale_backend.h; docs/kiln/graphics.md). AMD's own FSR API through its
// signed Vulkan DLLs from FidelityFX SDK 2.4.0, downloaded by the launcher into <game>/kiln/vendor/amd/2.4.0/:
// amd_fidelityfx_loader_vk.dll (the five API functions) and amd_fidelityfx_upscaler_vk.dll (FSR 4.1.2, FSR 3.1.6 and
// FSR 2.3.4). AMD's selection runs the newest the GPU supports (FSR 4 on recent AMD GPUs, else FSR 3.1); with
// graphics.amdUpscaler "auto" or "fsr3" Kiln pins the newest FSR 3.x through AMD's version override; "fsr4" (experimental)
// leaves it to AMD's selection.
//  - The DLLs are mapped when the game's device exists (device_ready), from Kiln's folder only: the upscaler DLL first,
//    so AMD's loader finds it already mapped and never looks in the game's folder (its first place) for another copy.
//  - The context is made for the colour texture's size (max render) and the output's, with HDR, inverted depth and
//    dynamic resolution; the game's six textures go in as they are (reactive = the pass's first extra texture,
//    transparency & composition = its second, B0), the motion vectors through Kiln's R16G16_SFLOAT copy (upscale_mv.h).
//  - Old contexts are destroyed once the GPU is done with them (upscale_logic.h retire_due), or at the device's end.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "ui.h"
#include "upscale_amd_map.h"
#include "upscale_backend.h"
#include "upscale_logic.h"
#include "upscale_mv.h"
#include "vendor_file.h"

namespace kui::upscale {
namespace {

constexpr const wchar_t* kLoader = L"amd_fidelityfx_loader_vk.dll";
constexpr const wchar_t* kUpscaler = L"amd_fidelityfx_upscaler_vk.dll";

bool ends_with_i(const std::wstring& s, const wchar_t* tail) {
    const size_t n = wcslen(tail);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}

std::atomic<int> g_msgs{0};
void ffx_message(uint32_t type, const wchar_t* message) {
    if (!message || g_msgs.fetch_add(1) >= 40) return;
    logf(type == FFX_API_MESSAGE_TYPE_ERROR ? KILN_LOG_WARN : KILN_LOG_INFO, "graphics: upscaler: AMD FSR says: %s",
         vendor::narrow(message).c_str());
}

class AmdBackend final : public Backend {
  public:
    Choice choice() const override { return Choice::amd; }
    const char* component() const override { return "amd-fsr"; }

    bool load(const Files& files, Why* why, std::string* detail) override {
        files_ = files;
        for (const auto& p : files.paths) {
            if (ends_with_i(p, kLoader)) loader_path_ = p;
            if (ends_with_i(p, kUpscaler)) upscaler_path_ = p;
        }
        if (loader_path_.empty() || upscaler_path_.empty()) {
            *why = Why::files_missing;
            *detail = "the AMD FSR component lacks amd_fidelityfx_loader_vk.dll or amd_fidelityfx_upscaler_vk.dll";
            return false;
        }
        // a copy another tool mapped already would be the one AMD's loader takes: never run next to it
        for (const wchar_t* n : {kLoader, kUpscaler}) {
            if (HMODULE m = GetModuleHandleW(n)) {
                wchar_t got[MAX_PATH] = {};
                GetModuleFileNameW(m, got, MAX_PATH);
                *why = Why::aside;
                *detail = std::string("another copy of ") + vendor::narrow(n) + " is already loaded (" + vendor::narrow(got) + ")";
                return false;
            }
        }
        *detail = "AMD FidelityFX SDK " + files.version + " (mapped when the game's device exists)";
        return true;
    }

    InstanceNeeds instance_needs() override { return {}; }
    DeviceNeeds device_needs(const PhysicalDevice& gpu) override {
        // which of FSR 4's sets the game's device gets: the one this GPU's FSR 4 model needs (upscale_amd_map.h
        // choose_fsr4_set), or kiln/control/amd "fsr4ext off|on" (read here, when the game makes its device)
        const amd::GpuFacts g = gpu_facts(gpu);
        const amd::Fsr4SetChoice c = amd::choose_fsr4_set(g, amd::parse_tuning(read_control()).fsr4ext);
        set_ = c.set;
        std::string six;
        for (const char* e : amd::fsr4_native_extensions()) six += (six.empty() ? "" : ", ") + std::string(e);
        const char* gpu_name = gpu.props.deviceName[0] ? gpu.props.deviceName : "the GPU";
        if (c.set == amd::Fsr4Set::int8)
            logf(KILN_LOG_INFO, "graphics: upscaler: AMD FSR on %s: the game's device also gets FSR 4's INT8 set, "
                 VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME " (computeDerivativeGroupLinear), not the rest of AMD's sample's set: %s",
                 gpu_name, c.why.c_str());
        else if (c.set == amd::Fsr4Set::native)
            logf(KILN_LOG_INFO, "graphics: upscaler: AMD FSR on %s: the game's device also gets %s (%s)", gpu_name, six.c_str(), c.why.c_str());
        else
            logf(KILN_LOG_INFO, "graphics: upscaler: AMD FSR on %s: the game's device gets none of %s: %s; FSR 3.1 needs none of them "
                 "(kiln/control/amd \"fsr4ext on\" adds them all from the next game start)", gpu_name, six.c_str(), c.why.c_str());
        return amd::device_needs(c.set);
    }

    bool device_ready(const VkContext& vk, Why* why, std::string* detail) override {
        vk_ = vk;
        const bool derivatives_on =
            vk.has_extension(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME) &&
            vk.has_feature(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR,
                           offsetof(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR, computeDerivativeGroupLinear));
        device_path_ = amd::fsr4_device_text(set_, derivatives_on);
        version_ids_.clear();
        version_names_.clear();
        if (!vk.needs_merged) {
            *why = Why::restart;
            *detail = "the game's device was made before AMD FSR was chosen (or without its Vulkan extensions)";
            return false;
        }
        // AMD's backend uses these whenever the GPU offers them: they must be on the device
        std::vector<std::string> offered;
        if (auto enum_ext = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(vk.gipa(vk.instance, "vkEnumerateDeviceExtensionProperties"))) {
            uint32_t n = 0;
            if (enum_ext(vk.physical, nullptr, &n, nullptr) == VK_SUCCESS && n) {
                std::vector<VkExtensionProperties> e(n);
                if (enum_ext(vk.physical, nullptr, &n, e.data()) == VK_SUCCESS)
                    for (uint32_t i = 0; i < n; ++i) offered.emplace_back(e[i].extensionName);
            }
        }
        auto feats2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(vk.gipa(vk.instance, "vkGetPhysicalDeviceFeatures2"));
        auto supported = [&](VkStructureType type, uint32_t offset) {
            if (!feats2) return false;
            alignas(8) uint8_t s[128] = {};
            std::memcpy(s, &type, sizeof type);
            VkPhysicalDeviceFeatures2 f2{}; f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            f2.pNext = s;
            feats2(vk.physical, &f2);
            VkBool32 v = VK_FALSE;
            std::memcpy(&v, s + offset, sizeof v);
            return v == VK_TRUE;
        };
        if (const std::string m = amd::missing_used_if_offered(vk, offered, supported); !m.empty()) {
            *why = Why::device;
            *detail = "the GPU offers " + m + ", which AMD FSR uses, but it could not be turned on for the game";
            return false;
        }
        // another tool may have mapped its own copy since load() checked (AMD's loader would take an already mapped one):
        // step aside, as load() does. Kiln's own copies (a device the game makes again) are fine.
        for (const wchar_t* n : {kLoader, kUpscaler}) {
            HMODULE m = GetModuleHandleW(n);
            if (!m || m == up_ || m == loader_) continue;
            wchar_t got[MAX_PATH] = {};
            GetModuleFileNameW(m, got, MAX_PATH);
            *why = Why::aside;
            *detail = std::string("another copy of ") + vendor::narrow(n) + " is already loaded (" + vendor::narrow(got) + ")";
            return false;
        }
        // AMD's DLLs: the upscaler first, then the loader (which then finds it mapped)
        // AMD's DLLs need Microsoft's Visual C++ runtime (msvcp140, vcruntime140, vcruntime140_1) in System32
        constexpr const char* kRuntime = ". AMD's DLLs need the Microsoft Visual C++ Redistributable 14.40 or later (x64) installed";
        std::string d;
        up_ = vendor::load_verified(upscaler_path_, &d);
        if (!up_) {
            *why = Why::load_failed;
            *detail = (d.empty() ? std::string("amd_fidelityfx_upscaler_vk.dll did not load") : d) + kRuntime;
            return false;
        }
        loader_ = vendor::load_verified(loader_path_, &d);
        if (!loader_) {
            *why = Why::load_failed;
            *detail = (d.empty() ? std::string("amd_fidelityfx_loader_vk.dll did not load") : d) + kRuntime;
            return false;
        }
        fx_.CreateContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(loader_, "ffxCreateContext"));
        fx_.DestroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(loader_, "ffxDestroyContext"));
        fx_.Configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(loader_, "ffxConfigure"));
        fx_.Query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(loader_, "ffxQuery"));
        fx_.Dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(loader_, "ffxDispatch"));
        if (!fx_.CreateContext || !fx_.DestroyContext || !fx_.Query || !fx_.Dispatch) {
            *why = Why::load_failed;
            *detail = "amd_fidelityfx_loader_vk.dll lacks an FSR API function";
            return false;
        }
        dc_ = VkDeviceContext{vk.device, vk.physical, vk.gdpa};
        // the versions AMD's selection may run on this GPU (the newest first in AMD's own order)
        uint64_t count = 0;
        ffxQueryDescGetVersions q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device = &dc_;
        q.outputCount = &count;
        if (fx_.Query(nullptr, &q.header) == FFX_API_RETURN_OK && count > 0 && count < 64) {
            std::vector<uint64_t> ids(count);
            std::vector<const char*> names(count);
            q.versionIds = ids.data();
            q.versionNames = names.data();
            if (fx_.Query(nullptr, &q.header) == FFX_API_RETURN_OK) {
                for (uint64_t i = 0; i < count; ++i) {
                    version_ids_.push_back(ids[i]);
                    version_names_.emplace_back(names[i] ? names[i] : "?");
                }
            }
        }
        std::string list;
        for (const auto& n : version_names_) list += (list.empty() ? "" : ", ") + n;
        if (version_names_.empty()) {
            *why = Why::device;
            *detail = "AMD FSR offers no upscaler version for this GPU";
            return false;
        }
        // Kiln's motion-vector pass writes its image without a format in the shader (upscale_mv.h)
        if (!vk.has_feature(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                            offsetof(VkPhysicalDeviceFeatures2, features) + offsetof(VkPhysicalDeviceFeatures, shaderStorageImageWriteWithoutFormat))) {
            *why = Why::device;
            *detail = "the game's device lacks shaderStorageImageWriteWithoutFormat (Kiln's motion-vector pass)";
            return false;
        }
        std::string mv;
        if (!mv_.init(vk, &mv)) {
            *why = Why::device;
            *detail = mv;
            return false;
        }
        *detail = "AMD FSR versions for this GPU: " + list + "; FSR 4 on the game's device: " + device_path_;
        return true;
    }

    bool create(const CreateInfo& ci, std::string* detail) override {
        destroy();  // the core calls it first; once more is harmless
        tuning_ = amd::parse_tuning(read_control());  // a knob changed in a test applies at the next set-up
        const int fsr3 = amd::pick_fsr3(version_names_);
        // FSR 3.1 when the player pins it, or for the rest of the game once FSR 4's picture came out black (step_down)
        bool pin = (amd_pins_fsr3(ci.amd) || fsr4_off_) && fsr3 >= 0;
        // the create flags depend on what runs (FSR 4: no DYNAMIC_RESOLUTION, as OptiScaler's working FSR 4): what AMD's
        // selection will run, checked below against what it ran
        Running expect = amd::expected_running(version_names_, pin);
        uint32_t r = make(ci, pin ? version_ids_[static_cast<size_t>(fsr3)] : 0, expect);
        std::string note;
        if (r != FFX_API_RETURN_OK && !pin && fsr3 >= 0) {
            // AMD's choice (FSR 4 where it runs) could not start: FSR 3.1 instead
            note = std::string("AMD's own choice could not start (") + amd::code_name(r) + "), FSR " + version_names_[static_cast<size_t>(fsr3)] + " runs";
            expect = Running::amd_fsr3;
            r = make(ci, version_ids_[static_cast<size_t>(fsr3)], expect);
            pin = true;
        }
        if (r != FFX_API_RETURN_OK) {
            *detail = std::string("ffxCreateContext: ") + amd::code_name(r);
            return false;
        }
        ffxQueryGetProviderVersion pv{};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (fx_.Query(&ctx_, &pv.header) == FFX_API_RETURN_OK && pv.versionName) version_name_ = pv.versionName;
        else version_name_ = pin && fsr3 >= 0 ? version_names_[static_cast<size_t>(fsr3)] : "?";
        running_ = amd::running_of(version_name_.c_str());
        version_ = parse_version(version_name_.c_str());
        if (running_ == Running::off) {
            *detail = "AMD's selection gave FSR " + version_name_ + " (older than FSR 3.1): Kiln does not run it";
            destroy();
            return false;
        }
        if (running_ != expect && running_ == Running::amd_fsr3 && fsr3 >= 0) {
            // AMD's selection ran FSR 3.1 where Kiln expected FSR 4: once more with FSR 3.1's flags (the context is unused)
            fx_.DestroyContext(&ctx_, nullptr);
            ctx_ = nullptr;
            expect = Running::amd_fsr3;
            if ((r = make(ci, version_ids_[static_cast<size_t>(fsr3)], expect)) != FFX_API_RETURN_OK) {
                *detail = std::string("ffxCreateContext (FSR 3.1 flags): ") + amd::code_name(r);
                running_ = Running::off;
                return false;
            }
        }
        ci_ = ci;
        reset_ = true;
        std::string kv_note;
        if (tuning_.keyvalues) {
            // kiln/control/amd "keyvalues optiscaler": OptiScaler's five FFX key-values for this ratio
            kv_note = fx_.Configure ? "" : "; keyvalues: ffxConfigure missing";
            for (auto [key, value] : amd::optiscaler_keyvalues(ci.ratio)) {
                if (!fx_.Configure) break;
                float v = value;
                ffxConfigureDescUpscaleKeyValue kv{};
                kv.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
                kv.key = key;
                kv.ptr = &v;
                if (const uint32_t cr = fx_.Configure(&ctx_, &kv.header); cr != FFX_API_RETURN_OK)
                    kv_note += "; keyvalue " + std::to_string(key) + ": " + amd::code_name(cr);
            }
        }
        // what AMD FSR was told about the game's picture (kiln.log, for reports: the dark-picture fix of 29.19)
        *detail = "FSR " + version_name_ + " (" + amd::flags_text(amd::create_flags(ci, tuning_, expect)) +
                  (tuning_.changed() ? ", from kiln/control/amd" : "") + "; " + amd::tuning_text(running_, tuning_) + ", no exposure texture; " + device_path_ + ")" +
                  (note.empty() ? "" : "; " + note) +
                  (fsr4_off_ && !amd_pins_fsr3(ci.amd) ? "; FSR 4's picture came out black on this GPU earlier in this game: FSR 3.1 until it restarts" : "") +
                  kv_note;
        for (const auto& n : tuning_.notes) *detail += "; kiln/control/amd: not understood: " + n;
        return true;
    }

    bool record(const Frame& f, std::string* detail) override {
        if (!ctx_) {
            *detail = "no AMD FSR context";
            return false;
        }
        frame_ = f.frame;
        reap(false);
        // never a frame larger than the context it was made for (AMD's DLL is not known to check: the GPU would read and
        // write past its resources); the core makes a new context first, so this only catches a frame that slipped past
        if (!frame_fits(ci_, f) || f.render_width > ci_.max_render_width || f.render_height > ci_.max_render_height) {
            *detail = "a frame of " + std::to_string(f.color.width) + "x" + std::to_string(f.color.height) + " -> " +
                      std::to_string(f.output_width) + "x" + std::to_string(f.output_height) + " for a context made for " +
                      std::to_string(ci_.max_render_width) + "x" + std::to_string(ci_.max_render_height) + " -> " +
                      std::to_string(ci_.output_width) + "x" + std::to_string(ci_.output_height);
            return false;
        }
        Texture motion = f.motion;
        if (f.motion.format == VK_FORMAT_R16G16_SNORM && !mv_.record(f.cb, f.motion, f.frame, &motion, detail)) return false;
        ffxDispatchDescUpscale d = amd::dispatch(f, motion, reset_, running_, tuning_);
        reset_ = false;
        const uint32_t r = fx_.Dispatch(&ctx_, &d.header);
        if (r != FFX_API_RETURN_OK) {
            *detail = std::string("ffxDispatch: ") + amd::code_name(r);
            return false;
        }
        return true;
    }

    void reset() override { reset_ = true; }

    bool step_down(std::string* detail) override {
        // FSR 4's picture came out black or NaN (the core's output check): FSR 3.1 for the rest of the game, made now for
        // the same sizes (create() retires the FSR 4 context first: the GPU may still run its last frames)
        if (running_ != Running::amd_fsr4 || amd::pick_fsr3(version_names_) < 0) return false;
        fsr4_off_ = true;
        const CreateInfo ci = ci_;
        std::string d;
        const bool ok = create(ci, &d);
        if (detail) *detail = d;
        return ok && running_ == Running::amd_fsr3;
    }
    bool output_check() const override { return !tuning_.check_off; }

    void tick(uint64_t frame) override {
        if (frame > frame_) frame_ = frame;
        reap(false);
        mv_.tick(frame);
    }

    void destroy() override {
        if (ctx_) retired_.push_back({ctx_, frame_});
        ctx_ = nullptr;
        running_ = Running::off;
    }

    void device_lost(VkDevice) override {
        destroy();
        reap(true);
        mv_.destroy();
    }

    Running running() const override { return running_; }
    uint32_t version() const override { return version_; }
    std::string name() const override {
        if (running_ == Running::off) return "AMD FSR";
        return "AMD FSR " + version_name_;
    }

  private:
    // What Kiln reads of the GPU before the game's device exists (upscale_amd_map.h GpuFacts): AMD's DLL is not mapped
    // yet, so the GPU's own answers, as AMD's selection reads them.
    static amd::GpuFacts gpu_facts(const PhysicalDevice& gpu) {
        amd::GpuFacts g;
        g.vendor = gpu.props.vendorID;
        g.type = gpu.props.deviceType;
        if (!gpu.gipa || !gpu.physical) return g;
        std::vector<std::string> offered;
        if (auto enum_ext = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gpu.gipa(gpu.instance, "vkEnumerateDeviceExtensionProperties"))) {
            uint32_t n = 0;
            if (enum_ext(gpu.physical, nullptr, &n, nullptr) == VK_SUCCESS && n && n < 4096) {
                std::vector<VkExtensionProperties> e(n);
                if (enum_ext(gpu.physical, nullptr, &n, e.data()) == VK_SUCCESS)
                    for (uint32_t i = 0; i < n; ++i) offered.emplace_back(e[i].extensionName);
            }
        }
        auto has = [&](const char* name) { return std::find(offered.begin(), offered.end(), name) != offered.end(); };
        g.coop_matrix_ext = has(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
        g.float8_ext = has(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
        g.derivatives_ext = has(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
        auto feats2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gpu.gipa(gpu.instance, "vkGetPhysicalDeviceFeatures2"));
        if (!feats2) feats2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gpu.gipa(gpu.instance, "vkGetPhysicalDeviceFeatures2KHR"));
        if (feats2 && (g.coop_matrix_ext || g.derivatives_ext)) {
            // only the structs of extensions the GPU offers go into the chain
            VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{};
            cm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
            VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR dv{};
            dv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR;
            void* chain = nullptr;
            if (g.derivatives_ext) dv.pNext = chain, chain = &dv;
            if (g.coop_matrix_ext) cm.pNext = chain, chain = &cm;
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            f2.pNext = chain;
            feats2(gpu.physical, &f2);
            g.coop_matrix = g.coop_matrix_ext && cm.cooperativeMatrix == VK_TRUE;
            g.derivative_linear = g.derivatives_ext && dv.computeDerivativeGroupLinear == VK_TRUE;
        }
        return g;
    }

    // <kiln>\control\amd (files_.dir is <kiln>\vendor\amd\<version>); empty when there is none
    std::string read_control() const {
        std::wstring kiln = files_.dir;
        for (int i = 0; i < 3; ++i) {
            const size_t s = kiln.find_last_of(L"\\/");
            if (s == std::wstring::npos) return {};
            kiln.resize(s);
        }
        const std::wstring path = kiln + L"\\control\\amd";
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        char buf[4096];
        DWORD got = 0;
        const BOOL ok = ReadFile(h, buf, sizeof buf, &got, nullptr);
        CloseHandle(h);
        return ok ? std::string(buf, got) : std::string();
    }

    uint32_t make(const CreateInfo& ci, uint64_t version_id, Running running) {
        ffxCreateContextDescUpscale c{};
        c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        c.flags = amd::create_flags(ci, tuning_, running);
        c.maxRenderSize = amd::max_render(ci, tuning_);
        c.maxUpscaleSize = {ci.output_width, ci.output_height};
        c.fpMessage = &ffx_message;
        ffxCreateBackendVKDesc b{};
        b.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        b.deviceContext = dc_;
        ffxCreateContextDescUpscaleVersion v{};
        v.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        v.version = FFX_UPSCALER_VERSION;
        ffxOverrideVersion o{};
        o.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        o.versionId = version_id;
        c.header.pNext = &b.header;
        b.header.pNext = &v.header;
        v.header.pNext = version_id ? &o.header : nullptr;
        ffxContext ctx = nullptr;
        const uint32_t r = fx_.CreateContext(&ctx, &c.header, nullptr);
        if (r == FFX_API_RETURN_OK) ctx_ = ctx;
        return r;
    }

    // Destroys the retired contexts the GPU is done with (all of them: the device is idle).
    void reap(bool all) {
        for (size_t i = 0; i < retired_.size();) {
            if (all || retire_due(retired_[i].frame, frame_)) {
                fx_.DestroyContext(&retired_[i].ctx, nullptr);
                retired_.erase(retired_.begin() + static_cast<ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    struct Retired {
        ffxContext ctx;
        uint64_t frame;
    };
    Files files_;
    std::wstring loader_path_, upscaler_path_;
    HMODULE loader_ = nullptr, up_ = nullptr;  // never unloaded (a context may live until the process ends)
    struct {
        PfnFfxCreateContext CreateContext = nullptr;
        PfnFfxDestroyContext DestroyContext = nullptr;
        PfnFfxConfigure Configure = nullptr;
        PfnFfxQuery Query = nullptr;
        PfnFfxDispatch Dispatch = nullptr;
    } fx_;
    VkContext vk_;
    VkDeviceContext dc_{};
    std::vector<uint64_t> version_ids_;
    std::vector<std::string> version_names_;
    ffxContext ctx_ = nullptr;
    std::vector<Retired> retired_;
    CreateInfo ci_;
    amd::Tuning tuning_;
    std::string device_path_ = "device not seen";
    amd::Fsr4Set set_ = amd::Fsr4Set::none;  // FSR 4's set the game's device was asked for (device_needs)
    bool fsr4_off_ = false;                  // FSR 4's picture came out black: FSR 3.1 for the rest of the game
    bool reset_ = true;
    uint64_t frame_ = 0;
    Running running_ = Running::off;
    uint32_t version_ = 0;
    std::string version_name_;
    MotionConverter mv_;
};

}  // namespace

std::unique_ptr<Backend> make_amd_backend() { return std::make_unique<AmdBackend>(); }

}  // namespace kui::upscale
