// SPDX-License-Identifier: MIT
// kiln.ui upscaler swap: the AMD FSR backend (upscale_backend.h; docs/kiln/graphics.md). AMD's own FSR API through its
// signed Vulkan DLLs from FidelityFX SDK 2.4.0, downloaded by the launcher into <game>/kiln/vendor/amd/2.4.0/:
// amd_fidelityfx_loader_vk.dll (the five API functions) and amd_fidelityfx_upscaler_vk.dll (FSR 4.1.2, FSR 3.1.6 and
// FSR 2.3.4). AMD's selection runs the newest the GPU supports (FSR 4 on recent AMD GPUs, else FSR 3.1); with
// graphics.amdUpscaler "fsr3" Kiln pins the newest FSR 3.x through AMD's version override.
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
    DeviceNeeds device_needs(const PhysicalDevice&) override { return amd::device_needs(); }

    bool device_ready(const VkContext& vk, Why* why, std::string* detail) override {
        vk_ = vk;
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
        *detail = "AMD FSR versions for this GPU: " + list;
        return true;
    }

    bool create(const CreateInfo& ci, std::string* detail) override {
        destroy();  // the core calls it first; once more is harmless
        const int fsr3 = amd::pick_fsr3(version_names_);
        bool pin = ci.amd == AmdPin::fsr3 && fsr3 >= 0;
        uint32_t r = make(ci, pin ? version_ids_[static_cast<size_t>(fsr3)] : 0);
        std::string note;
        if (r != FFX_API_RETURN_OK && !pin && fsr3 >= 0) {
            // AMD's choice (FSR 4 where it runs) could not start: FSR 3.1 instead
            note = std::string("AMD's own choice could not start (") + amd::code_name(r) + "), FSR " + version_names_[static_cast<size_t>(fsr3)] + " runs";
            r = make(ci, version_ids_[static_cast<size_t>(fsr3)]);
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
        ci_ = ci;
        reset_ = true;
        *detail = "FSR " + version_name_ + (note.empty() ? "" : "; " + note);
        return true;
    }

    bool record(const Frame& f, std::string* detail) override {
        if (!ctx_) {
            *detail = "no AMD FSR context";
            return false;
        }
        frame_ = f.frame;
        reap(false);
        Texture motion = f.motion;
        if (f.motion.format == VK_FORMAT_R16G16_SNORM && !mv_.record(f.cb, f.motion, f.frame, &motion, detail)) return false;
        ffxDispatchDescUpscale d = amd::dispatch(f, motion, reset_);
        reset_ = false;
        const uint32_t r = fx_.Dispatch(&ctx_, &d.header);
        if (r != FFX_API_RETURN_OK) {
            *detail = std::string("ffxDispatch: ") + amd::code_name(r);
            return false;
        }
        return true;
    }

    void reset() override { reset_ = true; }

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
    uint32_t make(const CreateInfo& ci, uint64_t version_id) {
        ffxCreateContextDescUpscale c{};
        c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        c.flags = amd::create_flags(ci);
        c.maxRenderSize = {ci.max_render_width, ci.max_render_height};
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
