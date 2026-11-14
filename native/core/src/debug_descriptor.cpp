// SPDX-License-Identifier: MIT
// Developer debugging: the descriptor and the control pipe (debug_descriptor.h). Above the _WIN32 block everything is
// plain C++ (host-tested by native/core/tests/test-debug-descriptor.sh); below it, the Windows pieces.
#include "debug_descriptor.h"

#include <utility>

#include "kiln_api.h"
#include "nlohmann/json.hpp"

namespace kiln::debug_descriptor {
namespace {

using ojson = nlohmann::ordered_json;

std::string dump(const ojson& j, int indent = -1) {
    return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string refusal(const char* code, std::string_view message) {
    ojson r;
    r["v"] = 1;
    r["ok"] = false;
    r["code"] = code;
    r["message"] = std::string(message);
    return dump(r);
}

// The caller's challenge: 8 to 128 characters of [0-9A-Za-z_-] (the CLI sends 32 hex digits).
bool good_challenge(const std::string& c) {
    if (c.size() < 8 || c.size() > 128) return false;
    for (char ch : c)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '-'))
            return false;
    return true;
}

}  // namespace

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Menu: return "menu";
        case Mode::Hosted: return "hosted";
        case Mode::Joined: return "joined";
        default: return "unknown";
    }
}

Mode mode_of(bool tracking, int session_result, uint32_t state) {
    if (!tracking || session_result != KILN_OK) return Mode::Unknown;
    switch (state) {
        case KILN_SESSION_NONE: return Mode::Menu;
        case KILN_SESSION_DEDICATED: return Mode::Hosted;  // a dedicated server runs its own server world
        case KILN_SESSION_HOST: return Mode::Hosted;
        case KILN_SESSION_REMOTE: return Mode::Unknown;    // a client world alone: no connection observer to say joined
        default: return Mode::Unknown;
    }
}

std::string to_hex(const uint8_t* bytes, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[bytes[i] >> 4]);
        s.push_back(d[bytes[i] & 15]);
    }
    return s;
}

std::string pipe_name(uint32_t pid, std::string_view nonce) {
    return "\\\\.\\pipe\\kiln-debug-" + std::to_string(pid) + "-" + std::string(nonce);
}

std::string descriptor_json(const Identity& id, bool managed_ready, std::string_view written_utc) {
    ojson d;
    d["format"] = 1;
    d["kiln"] = id.kiln;
    d["side"] = id.side;
    d["sessionNonce"] = id.nonce;
    d["windowsPid"] = id.pid;
    d["creationTime100ns"] = std::to_string(id.creation_100ns);  // a decimal string: no reader rounds it
    d["image"] = id.image;
    d["imageSha256"] = id.image_sha256;
    d["runtimeVersion"] = id.runtime_version;
    d["pipe"] = id.pipe;
    d["elevated"] = id.elevated;
    d["managedReady"] = managed_ready;
    d["written"] = std::string(written_utc);
    return dump(d, 2) + "\n";
}

std::string frame_too_large() {
    return refusal("frame_too_large", "a request line is at most 16384 bytes");
}

std::string answer(const Identity& id, std::string_view request, const Snapshot& snap) {
    const auto req = nlohmann::json::parse(request.begin(), request.end(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) return refusal("bad_request", "a request is one JSON object on one line");
    const auto v = req.find("v");
    if (v == req.end() || !v->is_number_integer() || v->get<int64_t>() != 1) return refusal("bad_request", "protocol version 1 only (\"v\": 1)");
    const auto op = req.find("op");
    if (op == req.end() || !op->is_string()) return refusal("bad_request", "no \"op\"");
    if (op->get<std::string>() != "identify") return refusal("unknown_op", "the only operation is identify");
    const auto nonce = req.find("nonce");
    if (nonce == req.end() || !nonce->is_string() || nonce->get<std::string>() != id.nonce)
        return refusal("identity_changed", "this process has another session nonce");
    const auto challenge = req.find("challenge");
    if (challenge == req.end() || !challenge->is_string() || !good_challenge(challenge->get<std::string>()))
        return refusal("bad_request", "\"challenge\" is 8 to 128 characters of [0-9A-Za-z_-]");
    ojson r;
    r["v"] = 1;
    r["ok"] = true;
    r["challenge"] = challenge->get<std::string>();
    r["identity"] = {{"windowsPid", id.pid},
                     {"creationTime100ns", std::to_string(id.creation_100ns)},
                     {"image", id.image},
                     {"imageSha256", id.image_sha256},
                     {"sessionNonce", id.nonce}};
    r["managedReady"] = snap.managed_ready ? snap.managed_ready() : false;
    r["sessionMode"] = mode_name(snap.mode ? snap.mode() : Mode::Unknown);
    r["runtimeVersion"] = id.runtime_version;
    r["side"] = id.side;
    r["kiln"] = id.kiln;
    return dump(r);
}

void serve(Listener& listener, const Identity& id, const Snapshot& snap) {
    while (auto c = listener.accept()) {
        LineReader lines;
        char buf[4096];
        bool open = true;
        while (open) {
            const int n = c->read(buf, sizeof buf, kIdleMs);
            if (n <= 0) break;  // closed, idle too long, or an error: next client
            const bool fits = lines.feed(buf, static_cast<size_t>(n), [&](std::string_view line) {
                if (open && !c->write(answer(id, line, snap) + "\n")) open = false;
            });
            if (!fits) {
                c->write(frame_too_large() + "\n");
                open = false;
            }
        }
    }
}

bool start_with(const Platform& p, bool enabled, bool managed_ready, std::string_view written_utc, const Snapshot& snap) {
    if (!enabled) return false;  // the default: nothing on disk, no pipe, no thread
    Identity id;
    if (!p.identity(&id)) {
        p.warn("debug: developer debugging is on, but this process's identity cannot be read; no descriptor, no control pipe");
        return false;
    }
    std::unique_ptr<Listener> listener = p.listen(id.pipe);
    if (!listener) {
        p.warn("debug: developer debugging is on, but the control pipe " + id.pipe + " cannot be made; no descriptor");
        return false;
    }
    if (!p.write_descriptor(id.pid, descriptor_json(id, managed_ready, written_utc))) {
        p.warn("debug: cannot write kiln/state/debug/" + std::to_string(id.pid) + ".json; developer debugging is off for this start");
        return false;  // the listener goes with this scope: the pipe closes
    }
    if (!p.spawn(std::move(listener), id, snap)) {
        p.remove_descriptor(id.pid);
        p.warn("debug: the control worker cannot start; developer debugging is off for this start");
        return false;
    }
    p.at_exit(id.pid);
    p.info("debug: developer debugging is on: kiln/state/debug/" + std::to_string(id.pid) + ".json, control pipe " + id.pipe +
           (managed_ready ? "" : " (the .NET host is not running: nothing to debug yet)"));
    return true;
}

}  // namespace kiln::debug_descriptor

#ifdef _WIN32
#include "common.h"  // windows.h first

#include <bcrypt.h>
#include <sddl.h>

#include <cstdlib>
#include <thread>
#include <vector>

#include "clr.h"
#include "config.h"
#include "image.h"
#include "log.h"
#include "tick.h"

#pragma comment(lib, "advapi32.lib")

namespace kiln::debug_descriptor {
namespace {

// One overlapped operation on the pipe, bounded by `timeout_ms` (INFINITE allowed); a timed-out one is cancelled and
// waited for, so `ov` never outlives its I/O.
bool finish(HANDLE pipe, OVERLAPPED& ov, DWORD timeout_ms, DWORD* got) {
    if (WaitForSingleObject(ov.hEvent, timeout_ms) != WAIT_OBJECT_0) {
        CancelIo(pipe);
        GetOverlappedResult(pipe, &ov, got, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, got, FALSE) != 0;
}

class PipeConn final : public Conn {
public:
    PipeConn(HANDLE pipe, HANDLE ev) : pipe_(pipe), ev_(ev) {}
    ~PipeConn() override { DisconnectNamedPipe(pipe_); }  // the instance is reused for the next client

    int read(char* buf, size_t n, int timeout_ms) override {
        OVERLAPPED ov{};
        ov.hEvent = ev_;
        ResetEvent(ev_);
        DWORD got = 0;
        if (ReadFile(pipe_, buf, static_cast<DWORD>(n), &got, &ov)) return static_cast<int>(got);
        const DWORD e = GetLastError();
        if (e == ERROR_BROKEN_PIPE) return 0;
        if (e != ERROR_IO_PENDING) return -1;
        if (!finish(pipe_, ov, static_cast<DWORD>(timeout_ms), &got)) return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
        return static_cast<int>(got);
    }

    bool write(std::string_view data) override {
        OVERLAPPED ov{};
        ov.hEvent = ev_;
        ResetEvent(ev_);
        DWORD put = 0;
        if (!WriteFile(pipe_, data.data(), static_cast<DWORD>(data.size()), &put, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            if (!finish(pipe_, ov, kIdleMs, &put)) return false;
        }
        return put == data.size();
    }

private:
    HANDLE pipe_;
    HANDLE ev_;
};

class PipeListener final : public Listener {
public:
    PipeListener(HANDLE pipe, HANDLE ev) : pipe_(pipe), ev_(ev) {}
    ~PipeListener() override {
        CloseHandle(pipe_);
        CloseHandle(ev_);
    }

    std::unique_ptr<Conn> accept() override {
        for (int fails = 0; fails < 8;) {
            OVERLAPPED ov{};
            ov.hEvent = ev_;
            ResetEvent(ev_);
            if (!ConnectNamedPipe(pipe_, &ov)) {
                const DWORD e = GetLastError();
                DWORD x = 0;
                if (e == ERROR_PIPE_CONNECTED) return std::make_unique<PipeConn>(pipe_, ev_);
                if (e == ERROR_NO_DATA) {  // connected and gone again
                    DisconnectNamedPipe(pipe_);
                    continue;
                }
                if (e != ERROR_IO_PENDING || !finish(pipe_, ov, INFINITE, &x)) {
                    ++fails;
                    DisconnectNamedPipe(pipe_);
                    Sleep(200);
                    continue;
                }
            }
            return std::make_unique<PipeConn>(pipe_, ev_);
        }
        KLOG_W("debug: the control pipe keeps failing to accept a client; the control worker stops");
        return nullptr;
    }

private:
    HANDLE pipe_;
    HANDLE ev_;
};

// A DACL that lets this process's user, and nobody else, open the pipe.
struct SameUser {
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    ~SameUser() {
        if (sd) LocalFree(sd);
    }
    bool make() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        DWORD len = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &len);
        std::vector<BYTE> buf(len ? len : 1);
        const bool got = len && GetTokenInformation(token, TokenUser, buf.data(), len, &len);
        CloseHandle(token);
        if (!got) return false;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) return false;
        const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
        LocalFree(sid);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return false;
        sa.nLength = sizeof sa;
        sa.lpSecurityDescriptor = sd;
        sa.bInheritHandle = FALSE;
        return true;
    }
};

std::unique_ptr<Listener> win_listen(const std::string& name) {
    SameUser acl;
    if (!acl.make()) {
        KLOG_W("debug: cannot build the control pipe's same-user DACL (error %lu)", GetLastError());
        return nullptr;
    }
    // FIRST_PIPE_INSTANCE: a pipe of that name made before us (by anyone) makes this fail instead of joining it
    const HANDLE pipe = CreateNamedPipeW(to_wide(name).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                         static_cast<DWORD>(kMaxFrame), static_cast<DWORD>(kMaxFrame), 0, &acl.sa);
    if (pipe == INVALID_HANDLE_VALUE) {
        KLOG_W("debug: CreateNamedPipe(%s) failed (error %lu)", name.c_str(), GetLastError());
        return nullptr;
    }
    const HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) {
        CloseHandle(pipe);
        return nullptr;
    }
    return std::make_unique<PipeListener>(pipe, ev);
}

bool elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD len = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &len) != 0;
    CloseHandle(token);
    return ok && e.TokenIsElevated;
}

// The runtime the core loaded: <kiln/dotnet>/shared/Microsoft.NETCore.App/<version>/coreclr.dll.
std::string runtime_version() {
    const HMODULE m = GetModuleHandleW(L"coreclr.dll");
    if (!m) return {};
    std::wstring p(32768, L'\0');
    const DWORD n = GetModuleFileNameW(m, p.data(), static_cast<DWORD>(p.size()));
    if (!n || n >= p.size()) return {};
    p.resize(n);
    return u8(std::filesystem::path(p).parent_path().filename());
}

bool win_identity(Identity* out) {
    uint8_t random[16];
    if (BCryptGenRandom(nullptr, random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
    out->nonce = to_hex(random, sizeof random);
    out->pid = GetCurrentProcessId();
    FILETIME c{}, x{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u)) return false;
    out->creation_100ns = (static_cast<uint64_t>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    std::wstring img(32768, L'\0');
    DWORD len = static_cast<DWORD>(img.size());
    if (!QueryFullProcessImageNameW(GetCurrentProcess(), 0, img.data(), &len)) return false;
    img.resize(len);
    out->image = to_utf8(img);
    out->image_sha256 = image().sha256;
    out->runtime_version = runtime_version();
    out->pipe = pipe_name(out->pid, out->nonce);
    out->side = side_name(image().side);
    out->kiln = std::string(kVersion);
    out->elevated = elevated();
    return out->creation_100ns != 0 && !out->image.empty() && out->image_sha256.size() == 64;
}

std::filesystem::path descriptor_path(uint32_t pid) {
    return paths().state / L"debug" / to_wide(std::to_string(pid) + ".json");
}

bool win_write(uint32_t pid, const std::string& text) {
    return write_file_atomic(descriptor_path(pid), text);
}

void win_remove(uint32_t pid) {
    DeleteFileW(descriptor_path(pid).c_str());
}

// Held for the process's life (never freed: the worker may still run while statics are torn down at exit).
struct Worker {
    std::unique_ptr<Listener> listener;
    Identity id;
    Snapshot snap;
};

bool win_spawn(std::unique_ptr<Listener> listener, const Identity& id, const Snapshot& snap) {
    auto* w = new Worker{std::move(listener), id, snap};
    try {
        std::thread([w] {
            try {
                serve(*w->listener, w->id, w->snap);
            } catch (...) {
                KLOG_E("debug: the control worker failed; it stops (the game is not affected)");
            }
        }).detach();
        return true;
    } catch (...) {
        delete w;
        return false;
    }
}

uint32_t g_exit_pid = 0;
void remove_at_exit() {
    if (g_exit_pid) win_remove(g_exit_pid);
}
void win_at_exit(uint32_t pid) {
    g_exit_pid = pid;
    std::atexit(&remove_at_exit);  // kiln-core's CRT runs it at the process's clean exit (DLL_PROCESS_DETACH)
}

void win_info(const std::string& line) { KLOG_I("%s", line.c_str()); }
void win_warn(const std::string& line) { KLOG_W("%s", line.c_str()); }

// The worker's reads: plain atomics of the core, never the CLR, never a lock a game thread holds.
bool managed_ready_now() { return clr::running(); }
Mode mode_now() {
    KilnSessionInfo s{};
    s.size = sizeof s;
    const KilnResult rc = tick::session(&s);
    return mode_of(tick::installed(), rc, s.state);
}

}  // namespace

void start(bool clr_started) {
    Platform p;
    p.identity = &win_identity;
    p.listen = &win_listen;
    p.write_descriptor = &win_write;
    p.remove_descriptor = &win_remove;
    p.spawn = &win_spawn;
    p.at_exit = &win_at_exit;
    p.info = &win_info;
    p.warn = &win_warn;
    Snapshot snap;
    snap.managed_ready = &managed_ready_now;
    snap.mode = &mode_now;
    start_with(p, config().developer_debugging, clr_started, utc_now(), snap);
}

}  // namespace kiln::debug_descriptor
#endif  // _WIN32
