// SPDX-License-Identifier: MIT
// kiln.ui: the Intel XeSS backend of Kiln's upscaler swap (upscale_backend.h; docs/kiln/graphics.md "Upscaler behind the
// game's FSR 3"). XeSS Super Resolution 3.0.2 from Intel's unmodified libxess.dll, loaded from
// <game>/kiln/vendor/intel/3.0.2/ after vendor_file.h checked it. On a GPU that is not Intel's it runs its cross-vendor
// (DP4a) path.
//
// Life of it (the core calls in this order, all on the game's main thread):
//  load          libxess.dll's exports (Host::load_library), XeSS's version, the test knobs (kiln/control/xess; read
//                again at each create, so a knob changed during a test applies at the game's next FSR set-up).
//  instance_needs / device_needs
//                XeSS's own answers (xessVKGetRequiredInstanceExtensions / DeviceExtensions / DeviceFeatures) once the
//                DLL is loaded; the SDK guide's list before that (xess_map.h documented_*_needs). What XeSS lists is
//                all required: XeSS lists only what the device has.
//  device_ready  checks the created device has every required need (under either name of a promoted feature); a device
//                created without XeSS's needs (XeSS chosen after the game started) means Why::restart. Makes Kiln's
//                VkPipelineCache for XeSS's pipelines (so a new context reuses them).
//  create        a new XeSS context for each of the game's Fsr3Create calls: xessVKCreateContext, xessVKInit with the
//                output size, the preset for the game's ratio and the flags. A context is never re-initialised in place
//                (that needs the GPU idle): the old one is retired and destroyed once 4 more frames were recorded (the
//                game alternates two command buffers, so its last frame is done by then), or at device_lost.
//  record        xessVKExecute into the game's command buffer (velocity scale first when it changed).
//  destroy       retires the context; device_lost destroys everything (the device is idle then).
// The class is plain C++ over Vulkan's headers and the Host below, so the host test (tests/xess_test.cpp) runs it
// against a fake libxess; upscale_xess.cpp is the Windows host.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "device_merge.h"
#include "upscale_backend.h"
#include "xess_abi.h"
#include "xess_map.h"

namespace kui::xess {

// What the backend needs from Kiln around it.
class Host {
  public:
    virtual ~Host() = default;
    // Maps the verified libxess.dll at `path` (vendor_file.h load_verified) and fills `api` from its exports.
    // False: `detail` says why (English).
    virtual bool load_library(const std::wstring& path, Api* api, std::string* detail) = 0;
    // The text of <kiln>/control/xess (the test knobs), "" when there is none.
    virtual std::string read_tuning(const upscale::Files& files) = 0;
    // One line for kiln.log; level KILN_LOG_* (1 error, 2 warning, 3 info, 4 debug). Any thread.
    virtual void log(int level, const std::string& line) = 0;
};

inline constexpr uint64_t kRetireRecords = 4;  // a retired context is destroyed after this many more records

class XessBackend final : public upscale::Backend {
  public:
    explicit XessBackend(std::unique_ptr<Host> host) : host_(std::move(host)) {}
    ~XessBackend() override {
        // Contexts the GPU may still use are left alone (their memory goes with the device); idle ones go.
        if (device_ok_) {
            collect(false);
            if (ctx_ || !retired_.empty())
                host_->log(2, "the XeSS backend went while its last frames may still run on the GPU: their XeSS objects stay until the game's device goes");
        }
        Host* me = host_.get();
        log_host().compare_exchange_strong(me, nullptr);
    }

    upscale::Choice choice() const override { return upscale::Choice::xess; }
    const char* component() const override { return "intel-xess"; }

    // ---- 1. needs -----------------------------------------------------------------------------------------------
    upscale::InstanceNeeds instance_needs() override {
        if (!loaded_) return documented_instance_needs();
        uint32_t n = 0, api = 0;
        const char* const* names = nullptr;
        const int32_t r = api_.vk_instance_extensions(&n, &names, &api);
        if (r < 0) {
            needs_error_ = std::string("XeSS cannot run with this Vulkan driver (it reports: ") + result_name(r) + ")";
            host_->log(2, needs_error_);
            return {};
        }
        upscale::InstanceNeeds out;
        out.min_api_version = api;
        for (uint32_t i = 0; i < n && names; ++i)
            if (names[i] && *names[i]) out.extensions.push_back({names[i], true});
        std::string list;
        for (const auto& e : out.extensions) list += (list.empty() ? "" : ", ") + e.name;
        host_->log(3, "XeSS asks the Vulkan instance for API " + version_text(api) + (list.empty() ? " and no extension" : " and " + list));
        return out;
    }

    upscale::DeviceNeeds device_needs(const upscale::PhysicalDevice& gpu) override {
        needs_asked_ = true;
        upscale::DeviceNeeds out;
        if (!loaded_) {
            needs_documented_ = true;
            device_needs_ = documented_device_needs();
            return device_needs_;
        }
        if (!query_device_needs(gpu.instance, gpu.physical, &out)) return {};
        std::string list;
        for (const auto& e : out.extensions) list += (list.empty() ? "" : ", ") + e.name;
        host_->log(3, "XeSS asks the device (" + std::string(gpu.props.deviceName) + ") for " + std::to_string(out.extensions.size()) +
                          " extension(s)" + (list.empty() ? "" : " (" + list + ")") + " and " + std::to_string(out.features.size()) + " feature struct(s)");
        needs_documented_ = false;
        device_needs_ = out;
        return out;
    }

    // ---- 2. load --------------------------------------------------------------------------------------------------
    bool load(const upscale::Files& files, upscale::Why* why, std::string* detail) override {
        if (loaded_) return true;
        auto fail = [&](upscale::Why w, const std::string& d) {
            if (why) *why = w;
            if (detail) *detail = d;
            host_->log(2, d);
            return false;
        };
        std::wstring path;
        for (const auto& p : files.paths) {
            const size_t s = p.find_last_of(L"\\/");
            std::wstring name = p.substr(s == std::wstring::npos ? 0 : s + 1);
            for (auto& c : name) c = static_cast<wchar_t>(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c);
            if (name == L"libxess.dll") path = p;
        }
        if (path.empty()) return fail(upscale::Why::files_missing, "libxess.dll is not among the verified files of intel-xess");
        Api api;
        std::string d;
        if (!host_->load_library(path, &api, &d)) return fail(upscale::Why::load_failed, d.empty() ? "libxess.dll did not load" : d);
        if (const char* m = api.missing()) return fail(upscale::Why::load_failed, std::string("libxess.dll has no export ") + m);
        Version v{};
        const int32_t r = api.get_version(&v);
        if (r < 0) return fail(upscale::Why::load_failed, std::string("xessGetVersion failed: ") + result_name(r));
        if (v.major != 3) {
            return fail(upscale::Why::load_failed, "libxess.dll is XeSS " + std::to_string(v.major) + "." + std::to_string(v.minor) + "." +
                                                       std::to_string(v.patch) + "; this Kiln is made for XeSS 3");
        }
        api_ = api;
        version_ = v;
        loaded_ = true;
        files_ = files;
        read_tuning();
        log_host().store(host_.get());
        host_->log(3, "libxess.dll loaded: XeSS " + name_version());
        return true;
    }

    // ---- 3. the device ----------------------------------------------------------------------------------------------
    bool device_ready(const upscale::VkContext& vk, upscale::Why* why, std::string* detail) override {
        auto fail = [&](upscale::Why w, const std::string& d) {
            if (why) *why = w;
            if (detail) *detail = d;
            host_->log(2, d);
            return false;
        };
        device_ok_ = false;
        if (!loaded_) return fail(upscale::Why::load_failed, "libxess.dll is not loaded");
        if (!needs_error_.empty()) return fail(upscale::Why::device, needs_error_);
        if (!vk.device || !vk.instance || !vk.physical) return fail(upscale::Why::no_vulkan, "Kiln has no Vulkan device for XeSS");
        // What XeSS itself asks of this device (again when the device was created on the guide's list or without any)
        upscale::DeviceNeeds needs = device_needs_;
        if (!needs_asked_ || needs_documented_) {
            if (!query_device_needs(vk.instance, vk.physical, &needs)) return fail(upscale::Why::device, needs_error_);
        }
        const auto missing = missing_on_device(vk, needs);
        if (!missing.empty()) {
            if (!needs_asked_)
                return fail(upscale::Why::restart, "XeSS was chosen after the game's start: it runs from the next start (the device lacks " + join(missing) + ")");
            return fail(upscale::Why::device, "the game's Vulkan device lacks what XeSS needs: " + join(missing));
        }
        vk_ = vk;
        create_cache_ = vk.gdpa ? reinterpret_cast<PFN_vkCreatePipelineCache>(vk.gdpa(vk.device, "vkCreatePipelineCache")) : nullptr;
        destroy_cache_ = vk.gdpa ? reinterpret_cast<PFN_vkDestroyPipelineCache>(vk.gdpa(vk.device, "vkDestroyPipelineCache")) : nullptr;
        if (create_cache_ && destroy_cache_ && !cache_) {
            VkPipelineCacheCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
            if (create_cache_(vk.device, &ci, nullptr, &cache_) != VK_SUCCESS) cache_ = VK_NULL_HANDLE;
        }
        device_ok_ = true;
        host_->log(3, std::string("XeSS is ready on ") + vk.props.deviceName + (vendor_is_intel(vk) ? " (Intel: XMX path where the GPU has it)" : " (its cross-vendor DP4a path)"));
        return true;
    }

    // ---- 4. a context ---------------------------------------------------------------------------------------------
    bool create(const upscale::CreateInfo& ci, std::string* detail) override {
        auto fail = [&](const std::string& d) {
            if (detail) *detail = d;
            host_->log(2, d);
            return false;
        };
        if (!device_ok_) return fail("XeSS has no device to run on");
        if (ctx_) destroy();
        collect(false);
        std::string why;
        if (!check_create(ci, &why)) return fail(why);
        read_tuning();  // a knob changed in a test applies at the game's next FSR set-up (a quality change)
        bool beyond = false;
        const float ratio = ratio_of(ci);
        const Quality q = quality_for_ratio(ratio, &beyond);
        const uint32_t flags = init_flags(ci, tuning_);
        Context ctx = nullptr;
        int32_t r = api_.vk_create_context(vk_.instance, vk_.physical, vk_.device, &ctx);
        if (r < 0 || !ctx) return fail(std::string("xessVKCreateContext failed: ") + result_name(r));
        if (api_.set_logging_callback) api_.set_logging_callback(ctx, tuning_.log_level, &XessBackend::on_xess_log);
        if (api_.is_optimal_driver && api_.is_optimal_driver(ctx) == kWarnOldDriver && !warned_driver_) {
            warned_driver_ = true;
            host_->log(2, "XeSS says the graphics driver is old: a newer one may run it faster or better (also seen with Vulkan's validation layer on)");
        }
        InitParams ip{};
        ip.output = {ci.output_width, ci.output_height};
        ip.quality = q;
        ip.flags = flags;
        ip.pipeline_cache = cache_;
        r = api_.vk_init(ctx, &ip);
        if (r < 0) {
            api_.destroy_context(ctx);  // never used in a command buffer: safe at once
            return fail("xessVKInit failed for " + size_text(ci.output_width, ci.output_height) + " (" + preset_of(q).name + "): " + result_name(r));
        }
        Dim out{ci.output_width, ci.output_height}, opt{}, lo{}, hi{};
        std::string range;
        if (api_.optimal_input_resolution(ctx, &out, q, &opt, &lo, &hi) >= 0 && hi.x) {
            range = "; XeSS takes " + size_text(lo.x, lo.y) + " to " + size_text(hi.x, hi.y) + " for it (best " + size_text(opt.x, opt.y) + ")";
            if (ci.max_render_width < lo.x || ci.max_render_height < lo.y || ci.max_render_width > hi.x || ci.max_render_height > hi.y)
                host_->log(2, "the game renders " + size_text(ci.max_render_width, ci.max_render_height) + ", outside XeSS's range for " +
                                  preset_of(q).name + range.substr(1));
        }
        if (beyond) host_->log(2, "the game's ratio " + ratio_text(ratio) + " is past XeSS's Ultra Performance (3.0): XeSS runs outside its range");
        ctx_ = ctx;
        ctx_w_ = ci.output_width;
        ctx_h_ = ci.output_height;
        ctx_flags_ = flags;
        ctx_quality_ = q;
        vel_x_ = vel_y_ = NAN;
        pending_reset_ = true;
        ++contexts_;
        host_->log(3, "XeSS " + name_version() + " set up: " + size_text(ci.max_render_width, ci.max_render_height) + " -> " +
                          size_text(ci.output_width, ci.output_height) + " (ratio " + ratio_text(ratio) + ", " + preset_of(q).name + "), flags " +
                          flags_text(flags) + range);
        return true;
    }

    // ---- 5. a frame -----------------------------------------------------------------------------------------------
    bool record(const upscale::Frame& f, std::string* detail) override {
        auto fail = [&](const std::string& d) {
            if (detail) *detail = d;
            host_->log(1, d);
            return false;
        };
        if (!ctx_) return fail("XeSS has no context to record with");
        std::string why;
        if (!check_frame(f, ctx_w_, ctx_h_, ctx_flags_, &why)) return fail("XeSS cannot take this frame: " + why);
        float vx = 0, vy = 0;
        velocity_scale(f, tuning_, &vx, &vy);
        if (vx != vel_x_ || vy != vel_y_) {
            const int32_t r = api_.set_velocity_scale(ctx_, vx, vy);
            if (r < 0) return fail(std::string("xessSetVelocityScale failed: ") + result_name(r));
            vel_x_ = vx;
            vel_y_ = vy;
        }
        const ExecuteParams p = execute_params(f, tuning_, ctx_flags_, pending_reset_);
        const int32_t r = api_.vk_execute(ctx_, f.cb, &p);
        if (r < 0) return fail(std::string("xessVKExecute failed: ") + result_name(r));
        pending_reset_ = false;
        ++records_;
        if (!logged_first_) {
            logged_first_ = true;
            host_->log(3, "XeSS upscales the game's frames: " + size_text(f.render_width, f.render_height) + " -> " +
                              size_text(f.output_width ? f.output_width : ctx_w_, f.output_height ? f.output_height : ctx_h_) + ", velocity scale " +
                              num_text(vx) + " x " + num_text(vy));
        }
        collect(false);
        return true;
    }

    void reset() override { pending_reset_ = true; }

    // ---- 6. the end -----------------------------------------------------------------------------------------------
    void destroy() override {
        if (!ctx_) return;
        retired_.push_back({ctx_, records_});
        ctx_ = nullptr;
        logged_first_ = false;
    }

    void device_lost(VkDevice device) override {
        if (vk_.device && device && device != vk_.device) return;
        destroy();
        collect(true);
        if (cache_ && destroy_cache_) destroy_cache_(vk_.device, cache_, nullptr);
        cache_ = VK_NULL_HANDLE;
        device_ok_ = false;
        vk_ = upscale::VkContext{};
    }

    upscale::Running running() const override { return upscale::Running::xess; }
    uint32_t version() const override { return loaded_ ? upscale::make_version(version_.major, version_.minor, version_.patch) : 0; }
    std::string name() const override { return "Intel XeSS " + (loaded_ ? name_version() : std::string("3")); }

    // For the tests and the log.
    bool loaded() const { return loaded_; }
    bool has_context() const { return ctx_ != nullptr; }
    size_t retired() const { return retired_.size(); }
    uint32_t contexts_made() const { return contexts_; }
    Quality quality() const { return ctx_quality_; }
    uint32_t flags() const { return ctx_flags_; }
    const Tuning& tuning() const { return tuning_; }

  private:
    struct Retired {
        Context ctx;
        uint64_t at;  // records_ when it was retired
    };

    static std::atomic<Host*>& log_host() {
        static std::atomic<Host*> h{nullptr};
        return h;
    }
    // XeSS's log callback (any thread): its lines in kiln.log.
    static void on_xess_log(const char* message, int32_t level) {
        Host* h = log_host().load();
        if (!h || !message) return;
        const int k = level >= kLogError ? 1 : level == kLogWarning ? 2 : level == kLogInfo ? 3 : 4;
        h->log(k, std::string("XeSS says: ") + message);
    }

    // kiln/control/xess, read at load and at each create; said in kiln.log when it changes.
    void read_tuning() {
        const std::string text = host_->read_tuning(files_);
        if (text == tuning_text_ && tuning_read_) return;
        tuning_read_ = true;
        tuning_text_ = text;
        tuning_ = parse_tuning(text);
        for (const auto& n : tuning_.notes) host_->log(2, "kiln/control/xess: not understood: " + n);
        if (tuning_.tuned()) host_->log(2, "kiln/control/xess test knobs in use: " + tuning_.describe());
        else if (!text.empty()) host_->log(3, "kiln/control/xess: every knob at its default");
    }

    bool query_device_needs(VkInstance inst, VkPhysicalDevice phys, upscale::DeviceNeeds* out) {
        uint32_t n = 0;
        const char* const* names = nullptr;
        int32_t r = api_.vk_device_extensions(inst, phys, &n, &names);
        if (r < 0) {
            needs_error_ = std::string("XeSS cannot run on this GPU or driver (it reports: ") + result_name(r) + ")";
            host_->log(2, needs_error_);
            return false;
        }
        void* chain = nullptr;
        r = api_.vk_device_features(inst, phys, &chain);
        if (r < 0) {
            needs_error_ = std::string("XeSS cannot run on this GPU or driver (it reports: ") + result_name(r) + ")";
            host_->log(2, needs_error_);
            return false;
        }
        upscale::DeviceNeeds needs;
        std::string why;
        if (!needs_from_xess(n, names, chain, &needs, &why)) {
            needs_error_ = why;
            host_->log(2, why);
            return false;
        }
        *out = std::move(needs);
        return true;
    }

    // Destroys retired contexts the GPU is done with (all with `all`: the device is idle).
    void collect(bool all) {
        for (size_t i = 0; i < retired_.size();) {
            if (all || records_ >= retired_[i].at + kRetireRecords) {
                api_.destroy_context(retired_[i].ctx);
                retired_.erase(retired_.begin() + static_cast<ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    static bool vendor_is_intel(const upscale::VkContext& vk) { return vk.props.vendorID == 0x8086; }
    std::string name_version() const {
        return std::to_string(version_.major) + "." + std::to_string(version_.minor) + "." + std::to_string(version_.patch);
    }
    static std::string version_text(uint32_t v) {
        return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v));
    }
    static std::string size_text(uint32_t w, uint32_t h) { return std::to_string(w) + "x" + std::to_string(h); }
    static std::string ratio_text(float r) {
        char b[32];
        std::snprintf(b, sizeof b, "%.2f", static_cast<double>(r));
        return b;
    }
    static std::string num_text(float v) {
        char b[32];
        std::snprintf(b, sizeof b, "%g", static_cast<double>(v));
        return b;
    }

    std::unique_ptr<Host> host_;
    Api api_{};
    Version version_{};
    bool loaded_ = false;
    upscale::Files files_;
    Tuning tuning_;
    std::string tuning_text_;
    bool tuning_read_ = false;
    // needs
    upscale::DeviceNeeds device_needs_;
    bool needs_asked_ = false, needs_documented_ = false;
    std::string needs_error_;
    // the device
    upscale::VkContext vk_;
    bool device_ok_ = false;
    PFN_vkCreatePipelineCache create_cache_ = nullptr;
    PFN_vkDestroyPipelineCache destroy_cache_ = nullptr;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    bool warned_driver_ = false;
    // the context
    Context ctx_ = nullptr;
    uint32_t ctx_w_ = 0, ctx_h_ = 0, ctx_flags_ = 0;
    Quality ctx_quality_ = kNativeAA;
    float vel_x_ = NAN, vel_y_ = NAN;
    bool pending_reset_ = true, logged_first_ = false;
    uint64_t records_ = 0;
    uint32_t contexts_ = 0;
    std::vector<Retired> retired_;
};

}  // namespace kui::xess
