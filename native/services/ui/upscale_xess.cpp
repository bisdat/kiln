// SPDX-License-Identifier: MIT
// kiln.ui: the Intel XeSS backend's Windows side (xess_backend.h is the backend; upscale_backend.h the interface).
// libxess.dll is mapped from the path vendor_file.h verified (its folder searched for what it imports, then the
// system's), its exports read with GetProcAddress, and nothing of it is ever patched or hooked (Intel's licence: the
// software unmodified, also while it runs).
#include <windows.h>

#include <memory>
#include <string>

#include "ui.h"
#include "upscale_backend.h"
#include "vendor_file.h"
#include "xess_backend.h"
#if __has_include("upscale_mv.h")
#include "upscale_mv.h"  // the core's motion-vector copy (B1): XeSS gets R16G16_SFLOAT, the format Intel's guide names
#define KUI_XESS_MOTION_COPY 1
#endif

namespace kui::upscale {
namespace {

#ifdef KUI_XESS_MOTION_COPY
class CoreMotionCopy final : public xess::MotionCopy {
  public:
    void want(DeviceNeeds* needs) override { want_mv_needs(needs); }
    bool init(const VkContext& vk, std::string* detail) override { return c_.init(vk, detail); }
    bool record(VkCommandBuffer cb, const Texture& in, uint64_t frame, Texture* out, std::string* detail) override {
        return c_.record(cb, in, frame, out, detail);
    }
    void destroy() override { c_.destroy(); }

  private:
    MotionConverter c_;
};
#endif

template <class T>
T proc(HMODULE m, const char* name) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

class WinHost final : public xess::Host {
  public:
    bool load_library(const std::wstring& path, xess::Api* api, std::string* detail) override {
        std::string why;
        HMODULE m = vendor::load_verified(path, &why);
        if (!m) {
            // libxess.dll imports Microsoft's Visual C++ runtime (14.40 or newer, Intel's guide): the usual reason
            if (why.find("error 126") != std::string::npos)
                why += ". XeSS needs Microsoft's Visual C++ runtime 14.40 or newer (msvcp140.dll, vcruntime140.dll, vcruntime140_1.dll)";
            if (detail) *detail = why;
            return false;
        }
        api->get_version = proc<xess::PFN_GetVersion>(m, "xessGetVersion");
        api->vk_instance_extensions = proc<xess::PFN_VKGetRequiredInstanceExtensions>(m, "xessVKGetRequiredInstanceExtensions");
        api->vk_device_extensions = proc<xess::PFN_VKGetRequiredDeviceExtensions>(m, "xessVKGetRequiredDeviceExtensions");
        api->vk_device_features = proc<xess::PFN_VKGetRequiredDeviceFeatures>(m, "xessVKGetRequiredDeviceFeatures");
        api->vk_create_context = proc<xess::PFN_VKCreateContext>(m, "xessVKCreateContext");
        api->vk_init = proc<xess::PFN_VKInit>(m, "xessVKInit");
        api->vk_execute = proc<xess::PFN_VKExecute>(m, "xessVKExecute");
        api->destroy_context = proc<xess::PFN_DestroyContext>(m, "xessDestroyContext");
        api->optimal_input_resolution = proc<xess::PFN_GetOptimalInputResolution>(m, "xessGetOptimalInputResolution");
        api->set_velocity_scale = proc<xess::PFN_SetVelocityScale>(m, "xessSetVelocityScale");
        api->set_logging_callback = proc<xess::PFN_SetLoggingCallback>(m, "xessSetLoggingCallback");
        api->is_optimal_driver = proc<xess::PFN_IsOptimalDriver>(m, "xessIsOptimalDriver");
        api->intel_xefx_version = proc<xess::PFN_GetIntelXeFXVersion>(m, "xessGetIntelXeFXVersion");
        api->set_max_responsive_mask = proc<xess::PFN_SetMaxResponsiveMaskValue>(m, "xessSetMaxResponsiveMaskValue");
        // The module stays mapped for the rest of the session: XeSS's objects and its log callback live in it.
        return true;
    }

    std::string read_tuning(const Files& files) override {
        // files.dir is <kiln>\vendor\intel\<version>: the knobs are in <kiln>\control\xess
        std::wstring kiln = files.dir;
        for (int i = 0; i < 3; ++i) {
            const size_t s = kiln.find_last_of(L"\\/");
            if (s == std::wstring::npos) return {};
            kiln.resize(s);
        }
        const std::wstring path = kiln + L"\\control\\xess";
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        char buf[4096];
        DWORD got = 0;
        const BOOL ok = ReadFile(h, buf, sizeof buf, &got, nullptr);
        CloseHandle(h);
        return ok ? std::string(buf, got) : std::string();
    }

    void log(int level, const std::string& line) override { logf(level, "upscale: xess: %s", line.c_str()); }

    std::unique_ptr<xess::MotionCopy> motion_copy() override {
#ifdef KUI_XESS_MOTION_COPY
        return std::make_unique<CoreMotionCopy>();
#else
        return nullptr;
#endif
    }
};

}  // namespace

std::unique_ptr<Backend> make_xess_backend() { return std::make_unique<xess::XessBackend>(std::make_unique<WinHost>()); }

}  // namespace kui::upscale
