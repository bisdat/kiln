// SPDX-License-Identifier: MIT
// kiln.launch at boot (launch.h): what decide() is handed, read once on the boot thread, then the freeze.
//   kiln.json             <game>/kiln/kiln.json, top level "shroudforge" (comments allowed, as config.cpp reads it)
//   environment           KILN_SHROUDFORGE_RUNTIME, as kiln-shroudforge reads it (1 to 15 characters)
//   profile               kiln.profile's text (ACTIVE / ONCE), as compat's early rules left it
//   held row              kiln-sfi.interpreter in the client gate (gates.h), after kiln.json "preview", with the release
//                         policy's rows (sfi_release_policy.h)
//   sf18 veto             the release policy: closed in every release and preview build
//   launch plan           <game>/kiln/launch-plans/<plan id>.json, only when kiln is selected and the profile names it
//   native.policy         kiln.json native.policy as config() read it (the value native mods follow), for the record
//   build verified        boot.cpp's own build_verified answer, handed in (the value build_info serves later), for the
//                         record
// Then kiln/state/launch.json, once, for the launcher (no code in the process reads it).
#include <random>

#include "common.h"
#include "config.h"
#include "gates.h"
#include "image.h"
#include "launch.h"
#include "log.h"
#include "profile.h"
#include "sfi_release_policy.h"

namespace kiln {
const char* core_build_stamp();  // api.cpp: this core's build identity ("" without build metadata)
}

namespace kiln::launch {
namespace {

// <game>/kiln/launch-plans/<name>: a plain file (no folder, no link) of at most kPlanMax bytes, never another path.
// The handle is opened without following a link, and the file is checked through that handle.
PlanFile read_plan(std::string_view name) {
    PlanFile f;
    const std::filesystem::path dir = paths().kiln / std::wstring(kPlanFolder.begin(), kPlanFolder.end());
    const DWORD dir_attributes = GetFileAttributesW(dir.c_str());
    if (dir_attributes == INVALID_FILE_ATTRIBUTES) {
        f.error = "kiln/launch-plans is not there";
        return f;
    }
    if (!(dir_attributes & FILE_ATTRIBUTE_DIRECTORY) || (dir_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        f.state = PlanFile::Unreadable;
        f.error = "kiln/launch-plans is not a plain folder";
        return f;
    }
    const std::filesystem::path file = dir / to_wide(name);
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            f.error = "no such file";
        } else {
            f.state = PlanFile::Unreadable;
            f.error = "it cannot be opened (error " + std::to_string(error) + ")";
        }
        return f;
    }
    f.state = PlanFile::Unreadable;
    BY_HANDLE_FILE_INFORMATION info{};
    const uint64_t size = GetFileInformationByHandle(h, &info)
                              ? (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow
                              : 0;
    if (!info.dwFileAttributes || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        f.error = "it is not a plain file";
    } else if (size > kPlanMax) {
        f.error = "it is larger than 8 MiB";
    } else {
        f.bytes.resize(static_cast<size_t>(size));
        DWORD got = 0;
        if (f.bytes.empty() || (ReadFile(h, f.bytes.data(), static_cast<DWORD>(f.bytes.size()), &got, nullptr) && got == size)) {
            f.state = PlanFile::Read;
        } else {
            f.bytes.clear();
            f.error = "it cannot be read (error " + std::to_string(GetLastError()) + ")";
        }
    }
    CloseHandle(h);
    return f;
}

// This start's id: 64 random bits, never 0. std::random_device may throw (MSVC's when rand_s fails): then the
// performance counter and the process id.
uint64_t new_launch_id() {
    try {
        std::random_device random;
        for (int i = 0; i < 4; ++i)
            if (const uint64_t id = (static_cast<uint64_t>(random()) << 32) ^ random()) return id;
    } catch (...) {
    }
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    const uint64_t id = (static_cast<uint64_t>(t.QuadPart) << 20) ^ GetCurrentProcessId() ^ 1;
    return id ? id : 1;
}

// UTC milliseconds since 1970-01-01.
uint64_t utc_ms() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return t > 116444736000000000ull ? (t - 116444736000000000ull) / 10000 : 0;
}

// kiln.json native.policy as config() read it, side section included: the value native_mods.cpp follows this start.
// Not an input of the decision: it rides along in the record for kiln-sfi, which gates ShroudForge package DLLs on it.
uint32_t frozen_native_policy() {
    switch (config().native_policy) {
        case NativePolicy::Always: return KILN_LAUNCH_NATIVE_POLICY_ALWAYS;
        case NativePolicy::VerifiedOnly: return KILN_LAUNCH_NATIVE_POLICY_VERIFIED_ONLY;
        case NativePolicy::Never: return KILN_LAUNCH_NATIVE_POLICY_NEVER;
    }
    return KILN_LAUNCH_NATIVE_POLICY_NEVER;
}

// What decide() is handed at boot, and its decision.
Decision decide_at_boot(uint32_t side, const std::string& safe_idle) {
    const auto& prof = profile::state();
    Inputs in;
    set_idle(in, safe_idle, prof.mode, config().plugins);
    std::string kiln_json, env;
    if (std::string text; read_file(paths().kiln / L"kiln.json", &text)) {
        kiln_json = jsonc_to_json(text);
        in.kiln_json = &kiln_json;
    }
    wchar_t value[16] = L"";
    const DWORD n = GetEnvironmentVariableW(L"KILN_SHROUDFORGE_RUNTIME", value, 16);
    if (n > 0 && n < 16) {
        env = to_utf8(std::wstring(value, n));
        in.env_runtime = &env;
    }
    if (prof.mode == KILN_PROFILE_ACTIVE || prof.mode == KILN_PROFILE_ONCE) in.profile_text = prof.text;
    in.side = side;
    in.exe_sha256 = image().sha256;
    bool gate_held = false;
    std::string gate_text;
    for (const gates::Held& h : gates::held())
        if (h.name == kInterpreterGate) gate_held = true, gate_text = h.text;
    const Hold hold = interpreter_hold(gate_held, gate_text);
    in.interpreter_held = hold.held;
    in.held_text = hold.text;
    // The kiln-sfi.sf18 veto: the release policy alone (closed in every release and preview build).
    in.release_allows_180 = sfi_release_policy::release_allows_180();
    in.read_plan = &read_plan;
    return decide(in);
}

}  // namespace

void freeze_at_boot(uint32_t side, bool verified, const std::string& safe_idle) {
    // Nothing in here may leave this function: kiln_boot's catch would end Kiln's whole boot, before any native mod,
    // plugin, the .NET host or the chain's [late] DLLs. An exception while reading the inputs or deciding freezes
    // backend none with KILN_LAUNCH_REASON_UNDECIDED (idle class: every loader keeps its rules from before this table).
    Decision d;
    try {
        d = decide_at_boot(side, safe_idle);
    } catch (const std::exception& e) {
        d = undecided(e.what());
    } catch (...) {
        d = undecided("an unknown error");
    }
    d.native_policy = frozen_native_policy();  // for every decision, undecided() included; reading config() cannot throw
    d.build_verified = verified ? 1u : 0u;     // boot's own answer (the record keeps 0 for backend none)
    const uint64_t id = new_launch_id(), stamp = utc_ms();
    // kiln/state/launch.json: written by the freeze that took (once per boot), after the record is served, atomically
    // (write_file_atomic: <file>.tmp, then a replacing rename), retried for about half a second while a reader holds
    // the old file open (the rename is refused then).
    // A failure is only logged (freeze() also swallows anything this throws).
    const auto write_state = [&] {
        try {
            const char* build = core_build_stamp();
            const std::string text = state_json(d, id, stamp, GetCurrentProcessId(), *build ? std::string_view(build) : kVersion);
            const std::filesystem::path file = paths().state / L"launch.json";
            if (!write_retrying([&] { return write_file_atomic(file, text); }, [](uint32_t ms) { Sleep(ms); }))
                KLOG_W("state: cannot write %s (it keeps an earlier start's record)", u8(file).c_str());
        } catch (const std::exception& e) {
            KLOG_W("state: cannot write launch.json (%s)", e.what());
        }
    };
    bool took = false;
    try {
        took = freeze(d, id, stamp, write_state);
    } catch (const std::exception& e) {  // only an allocation before the record is claimed
        KLOG_E("launch: the ShroudForge selection could not be frozen (%s)", e.what());
        return;
    }
    if (!took) {
        KLOG_E("launch: the ShroudForge selection was frozen already; this boot's is not used");
        return;
    }
    if (d.reason_code == KILN_LAUNCH_REASON_UNDECIDED)
        KLOG_E("launch %016llx: %s", static_cast<unsigned long long>(id), d.reason.c_str());
    else if (KILN_LAUNCH_REASON_REFUSED(d.reason_code))
        KLOG_W("launch %016llx: ShroudForge mods are off this start: %s", static_cast<unsigned long long>(id), d.reason.c_str());
    else if (d.backend == KILN_LAUNCH_BACKEND_NONE)
        KLOG_I("launch %016llx: no ShroudForge backend decided: %s", static_cast<unsigned long long>(id), d.reason.c_str());
    else
        KLOG_I("launch %016llx: ShroudForge backend %s (%s)", static_cast<unsigned long long>(id), backend_name(d.backend),
               d.reason.c_str());
}

}  // namespace kiln::launch
