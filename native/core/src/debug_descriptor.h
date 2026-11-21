// SPDX-License-Identifier: MIT
// Developer debugging (chapter 12a §4 steps 1 and 7): with kiln.json "developer_debugging": true the core writes a
// descriptor of this process after the .NET host started and serves a local control pipe that proves which process
// it is. Off (the default): no descriptor, no pipe, no thread.
//
//   <game>/kiln/state/debug/<windowsPid>.json   (schema/state-debug.schema.json), removed at a clean exit
//   \\.\pipe\kiln-debug-<windowsPid>-<sessionNonce>  same-user DACL, remote clients refused, one client at a time
//
// The pipe speaks one JSON object per line, at most 16 KiB a line (a longer one is refused and the connection closed):
//   -> {"v":1,"op":"identify","nonce":"<sessionNonce>","challenge":"<the caller's random>"}
//   <- {"v":1,"ok":true,"challenge":<echoed>,"identity":{windowsPid,creationTime100ns,image,imageSha256,sessionNonce},
//       "managedReady":bool,"sessionMode":"menu"|"hosted"|"joined"|"unknown","runtimeVersion","side","kiln"}
//   <- {"v":1,"ok":false,"code":"identity_changed"|"bad_request"|"unknown_op"|"frame_too_large","message":...}
// A wrong nonce gets no identity. The worker never enters the CLR and never waits for a game thread: what it answers
// comes from this process's identity (fixed at start), clr::running() and tick::session()'s atomics. The ACL and the
// nonce prevent accidents (attaching to the wrong process); they are not a sandbox against code of the same user.
// Readers: kiln_manage::debug (crates/kiln-manage/src/debug), the `kiln debug-inspect` helper.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace kiln::debug_descriptor {

inline constexpr size_t kMaxFrame = 16 * 1024;
inline constexpr int kIdleMs = 5000;  // a connected client that sends nothing for this long is dropped

// This process, as the descriptor and the pipe state it.
struct Identity {
    uint32_t pid = 0;
    uint64_t creation_100ns = 0;  // GetProcessTimes' creation FILETIME
    std::string image;            // QueryFullProcessImageNameW (Win32 form), UTF-8
    std::string image_sha256;
    std::string nonce;            // 32 lower-case hex digits
    std::string runtime_version;  // the loaded coreclr.dll's folder name, "" without .NET
    std::string pipe;             // pipe_name(pid, nonce)
    std::string side;             // "server" | "client"
    std::string kiln;             // Kiln's version
    bool elevated = false;
};

// The advisory session mode (kiln_manage::debug::SessionMode).
enum class Mode { Menu, Hosted, Joined, Unknown };
const char* mode_name(Mode m);
// From tick::session(): its result, KILN_SESSION_* state, and whether the tick tracks worlds at all. A client world
// without a server world is "unknown", never "joined": there is no connection observer.
Mode mode_of(bool tracking, int session_result, uint32_t state);

// What the worker reads while it answers; both must be lock-free and must not call into the CLR.
struct Snapshot {
    bool (*managed_ready)() = nullptr;
    Mode (*mode)() = nullptr;
};

std::string pipe_name(uint32_t pid, std::string_view nonce);
std::string to_hex(const uint8_t* bytes, size_t n);
// Descriptor format 1 (one JSON document and a newline).
std::string descriptor_json(const Identity& id, bool managed_ready, std::string_view written_utc);
// One request line (no newline) -> one response line (no newline).
std::string answer(const Identity& id, std::string_view request, const Snapshot& snap);
std::string frame_too_large();

// Splits a byte stream into lines; a line longer than kMaxFrame is an error (the connection is then closed).
class LineReader {
public:
    // Calls on_line for every complete line. False once a line is too long (nothing more is read from this stream).
    template <class F>
    bool feed(const char* p, size_t n, F&& on_line) {
        for (size_t i = 0; i < n; ++i) {
            if (p[i] == '\n') {
                if (!buf_.empty() && buf_.back() == '\r') buf_.pop_back();
                on_line(std::string_view(buf_));
                buf_.clear();
            } else if (buf_.size() >= kMaxFrame) {
                return false;
            } else {
                buf_.push_back(p[i]);
            }
        }
        return true;
    }

private:
    std::string buf_;
};

// The worker's transport: production is the named pipe, the host test an in-memory one.
struct Conn {
    virtual ~Conn() = default;
    // > 0: bytes read; 0: the client closed; < 0: an error or kIdleMs without data.
    virtual int read(char* buf, size_t n, int timeout_ms) = 0;
    virtual bool write(std::string_view data) = 0;
};
struct Listener {
    virtual ~Listener() = default;
    // The next client; nullptr ends the worker.
    virtual std::unique_ptr<Conn> accept() = 0;
};

// The worker loop: every client's requests answered in order, one line each, until accept() returns nullptr.
void serve(Listener& listener, const Identity& id, const Snapshot& snap);

// What start() does to the outside world, so the host test can count it (production: Windows, below in the .cpp).
struct Platform {
    bool (*identity)(Identity* out) = nullptr;                                   // this process; false: not available
    std::unique_ptr<Listener> (*listen)(const std::string& pipe) = nullptr;     // nullptr: the pipe cannot be made
    bool (*write_descriptor)(uint32_t pid, const std::string& text) = nullptr;  // <state>/debug/<pid>.json
    void (*remove_descriptor)(uint32_t pid) = nullptr;
    bool (*spawn)(std::unique_ptr<Listener> listener, const Identity& id, const Snapshot& snap) = nullptr;  // the worker
    void (*at_exit)(uint32_t pid) = nullptr;                                     // remove the descriptor at a clean exit
    void (*info)(const std::string& line) = nullptr;
    void (*warn)(const std::string& line) = nullptr;
};

// The decision and its order: off -> nothing; on -> identity, the pipe, the descriptor, the worker (a failure undoes
// what was made before it). True when the worker runs.
bool start_with(const Platform& p, bool enabled, bool managed_ready, std::string_view written_utc, const Snapshot& snap);

// boot.cpp, once, right after the .NET host started (or failed to): config().developer_debugging decides.
void start(bool clr_started);

}  // namespace kiln::debug_descriptor
