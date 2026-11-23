// SPDX-License-Identifier: MIT
// kiln.staged (staged.h): staged updates applied at the start as one rename transaction per unit, with rollback.
// Folders and formats: docs/kiln/staged-updates.md. Everything here runs once per start on the boot thread, before
// anything else of Kiln reads the files it changes, so nothing is locked.
//
// A transaction (tx/<unit>/journal.json) is a list of steps {target, source, backup, had}, paths relative to the server
// folder. Applying a step: the target (if `had`) is renamed to its backup in tx/<unit>/prev/files, then the source (if
// any) is renamed to the target. Renames never replace: a target that is in the way is an error. Once every step ran the
// journal says "committed", and finish() (idempotent) makes tx/<unit>/prev the unit's kept version
// (previous/<unit>), removes what was staged and writes the probation and the result. A start that finds a journal:
// "committed" runs finish() again; "applying" puts every step back (undo_steps: each step's state is read from which of
// target, backup and source exist, so a crash while putting back is put back by the next start the same way). No step
// copies or writes a file in place, so a crash or a power cut leaves the old or the new version of every unit.
#include "staged.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#include "nlohmann/json.hpp"
#include "services/boot_gamedata.h"  // the portable SHA-256 (Sha256, file_sha256, lower_hex)

namespace kiln::staged {

void (*g_step_hook)(const char* step) = nullptr;
uint64_t (*g_free_bytes_hook)(const std::filesystem::path& dir) = nullptr;

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using boot_gd::file_sha256;
using boot_gd::lower_hex;

constexpr const char* kStaged = "kiln/staged";

fs::path p8(std::string_view s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
std::string s8(const fs::path& p) {
    const auto u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}
void hook(const char* what) {
    if (g_step_hook) g_step_hook(what);
}
std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}
bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

// ---- time ----
std::tm utc_tm(time_t t) {
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    return g;
}
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string utc_now() {
    const int64_t ms = now_ms();
    const std::tm g = utc_tm(static_cast<time_t>(ms / 1000));
    char b[40];
    std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour,
                  g.tm_min, g.tm_sec, static_cast<int>(ms % 1000));
    return b;
}
// "20261123T101500123Z-0007": sorts by time; the counter keeps two records of one millisecond apart.
std::string record_id() {
    static std::atomic<unsigned> seq{0};
    const int64_t ms = now_ms();
    const std::tm g = utc_tm(static_cast<time_t>(ms / 1000));
    char b[48];
    std::snprintf(b, sizeof b, "%04d%02d%02dT%02d%02d%02d%03dZ-%04u", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour,
                  g.tm_min, g.tm_sec, static_cast<int>(ms % 1000), seq.fetch_add(1) % 10000u);
    return b;
}

// ---- files ----
bool present(const fs::path& p) {
    std::error_code ec;
    return fs::exists(fs::symlink_status(p, ec));
}
bool is_link(const fs::path& p) {
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(p, ec))) return true;
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;  // junctions too
#else
    return false;
#endif
}
bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(fs::symlink_status(p, ec));
}
bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(fs::symlink_status(p, ec));
}

// A rename that never replaces what is at `to` (a file or a folder), flushed through to the disk on Windows.
bool move_path(const fs::path& from, const fs::path& to, std::string& err) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
    err = "renaming " + s8(from) + " to " + s8(to) + " failed (error " + std::to_string(GetLastError()) + ")";
    return false;
#else
    if (present(to)) {
        err = "renaming " + s8(from) + " to " + s8(to) + " failed: the target exists";
        return false;
    }
    if (::rename(from.c_str(), to.c_str()) == 0) return true;
    err = "renaming " + s8(from) + " to " + s8(to) + " failed: " + std::strerror(errno);
    return false;
#endif
}

// <file>.tmp written and flushed, then renamed over <file>: the file is the old or the new text, never a part.
bool write_atomic(const fs::path& file, const std::string& text, std::string& err) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = fs::path(file).concat(".tmp");
#ifdef _WIN32
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return err = "cannot write " + s8(tmp) + " (error " + std::to_string(GetLastError()) + ")", false;
    DWORD written = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size() &&
                    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = "cannot write " + s8(file) + " (error " + std::to_string(GetLastError()) + ")";
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
#else
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return err = "cannot write " + s8(tmp) + ": " + std::strerror(errno), false;
    size_t done = 0;
    bool ok = true;
    while (ok && done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n <= 0) ok = false;
        else done += static_cast<size_t>(n);
    }
    ok = ok && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), file.c_str()) != 0) {
        err = "cannot write " + s8(file) + ": " + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
#endif
}

bool remove_tree(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    return !ec && !present(p);
}
void remove_file(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
}
void remove_if_empty(const fs::path& dir) {
    std::error_code ec;
    if (fs::is_empty(dir, ec) && !ec) fs::remove(dir, ec);
}

bool read_text(const fs::path& file, uintmax_t max, std::string& out, std::string& why) {
    std::error_code ec;
    if (!is_file(file)) return why = s8(file.filename()) + " is missing or not a file", false;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec) return why = "cannot read " + s8(file.filename()) + ": " + ec.message(), false;
    if (size > max) return why = s8(file.filename()) + " is larger than " + std::to_string(max) + " bytes", false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return why = "cannot open " + s8(file.filename()), false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) return why = "cannot read " + s8(file.filename()), false;
    return true;
}
bool read_json(const fs::path& file, json& out, std::string& why, bool comments = false) {
    std::string text;
    if (!read_text(file, kMaxManifest, text, why)) return false;
    out = json::parse(text, nullptr, false, comments);
    if (out.is_discarded() || !out.is_object()) return why = s8(file.filename()) + " is not a JSON object", false;
    return true;
}
// Text that is not UTF-8 (a path on a POSIX host) is written with replacement characters instead of throwing.
std::string dump(const json& j, int indent = -1) { return j.dump(indent, ' ', false, json::error_handler_t::replace); }
bool write_json(const fs::path& file, const json& j, std::string& err) { return write_atomic(file, dump(j, 2) + "\n", err); }

std::string str(const json& o, const char* key) {
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}
bool is_one(const json& o, const char* key) {
    const auto it = o.find(key);
    return it != o.end() && it->is_number_unsigned() && it->get<uint64_t>() == static_cast<uint64_t>(kFormat);
}

// ---- path rules ----
bool utf8_ok(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        uint32_t cp = n == 1 ? c : c & (0xFFu >> (n + 1));
        for (size_t k = 1; k < n; ++k) {
            const auto d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3Fu);
        }
        static constexpr uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += n;
    }
    return true;
}
bool device_name(std::string_view segment) {
    std::string base = lower(segment.substr(0, segment.find('.')));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    for (const char* n : {"con", "prn", "aux", "nul", "conin$", "conout$"})
        if (base == n) return true;
    const bool port = starts_with(base, "com") || starts_with(base, "lpt");
    if (port && base.size() == 4 && base[3] >= '0' && base[3] <= '9') return true;
    // COM¹ COM² COM³ (U+00B9, U+00B2, U+00B3) are device names too
    return port && base.size() == 5 && base[3] == '\xc2' && (base[4] == '\xb9' || base[4] == '\xb2' || base[4] == '\xb3');
}

}  // namespace

bool rel_path_ok(const std::string& rel, std::string* why) {
    std::string w;
    auto fail = [&](std::string text) {
        if (why) *why = "path " + dump(json(utf8_ok(rel) ? rel : std::string("(not UTF-8)"))) + ": " + std::move(text);
        return false;
    };
    if (rel.empty()) return fail("empty");
    if (rel.size() > kMaxRelPath) return fail("longer than " + std::to_string(kMaxRelPath) + " bytes");
    if (!utf8_ok(rel)) return fail("not UTF-8");
    for (const char ch : rel) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) return fail("a control character");
        if (ch == '\\') return fail("a backslash (paths use /)");
        if (std::strchr(":*?\"<>|", ch)) return fail(std::string("the character ") + ch);
    }
    if (rel.front() == '/') return fail("absolute");
    size_t start = 0;
    while (start <= rel.size()) {
        const size_t end = std::min(rel.find('/', start), rel.size());
        const std::string_view seg(rel.data() + start, end - start);
        if (seg.empty()) return fail("an empty segment");
        if (seg == "." || seg == "..") return fail("a . or .. segment");
        if (seg.back() == '.' || seg.back() == ' ') return fail("a segment ending in a dot or a space");
        if (device_name(seg)) return fail("a device name");
        start = end + 1;
    }
    return true;
}

bool kiln_path_ok(const std::string& rel, std::string* why) {
    if (!rel_path_ok(rel, why)) return false;
    const std::string l = lower(rel);
    if (l == "dinput8.dll") return true;
    if (!starts_with(l, "kiln/")) {
        if (why) *why = "path " + dump(json(rel)) + ": outside kiln/ (a Kiln update writes dinput8.dll and kiln/... only)";
        return false;
    }
    for (const char* own : {"kiln/staged", "kiln/data", "kiln/config", "kiln/logs", "kiln/state", "kiln/backups", "kiln/control",
                            "kiln/crash-reports"}) {
        if (l == own || starts_with(l, std::string(own) + "/")) {
            if (why) *why = "path " + dump(json(rel)) + ": under " + own + ", which holds the server's own files";
            return false;
        }
    }
    return true;
}

bool mod_folder_ok(const std::string& folder) {
    return rel_path_ok(folder, nullptr) && folder.find('/') == std::string::npos && folder.size() <= 64 && folder.front() != '.';
}

namespace {

// ---- units ----
struct Unit {
    bool kiln = false;
    std::string folder;  // a mod's folder under kiln/mods
    std::string key() const { return kiln ? "kiln" : "mod." + folder; }
    std::string root() const { return kiln ? std::string() : "kiln/mods/" + folder; }  // what it changes
    std::string name() const { return kiln ? std::string("Kiln") : "mod folder " + folder; }
    std::string staged_dir() const { return std::string(kStaged) + (kiln ? "/kiln" : "/mods/" + folder); }
    std::string previous_dir() const { return std::string(kStaged) + "/previous" + (kiln ? "/kiln" : "/mods/" + folder); }
    std::string tx_dir() const { return std::string(kStaged) + "/tx/" + key(); }
    // where a target's copy lives inside a kept version (`base` = previous/<unit> or tx/<unit>/prev)
    std::string kept_path(const std::string& base, const std::string& target) const {
        return kiln ? base + "/files/" + target : base + "/files";
    }
};

struct Step {
    std::string target, source, backup;  // relative to the server folder; source empty: the target is removed
    bool had = false;                    // the target existed when the transaction began
};

struct Tx {
    std::string id, op, version, from, started, phase, note;  // op: update | rollback | auto-rollback
    std::vector<Step> steps;
    json probation;  // update: written on commit
};

constexpr const char* kReceipt = "kiln/.install.json";

struct FileEntry {
    std::string path, sha256;
    uint64_t size = 0;
};

struct Ctx {
    const Env& env;
    Summary sum;
    fs::path abs(const std::string& rel) const { return rel.empty() ? env.game : env.game / p8(rel); }
    void log(Level l, const std::string& text) const {
        if (env.log) env.log(l, "staged: " + text);
    }
};

std::string op_text(const std::string& op) {
    return op == "update" ? "update" : op == "auto-rollback" ? "automatic rollback" : "rollback";
}
std::string version_text(const std::string& v) { return v.empty() ? "(not installed)" : v; }

// ---- results, attempts ----
void prune_results(const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".json") files.push_back(it->path());
    if (files.size() <= kResultsKept) return;
    std::sort(files.begin(), files.end());
    for (size_t i = 0; i + kResultsKept < files.size(); ++i) remove_file(files[i]);
}
void record(Ctx& c, const Unit& u, const std::string& id, const std::string& op, bool ok, const std::string& version,
            const std::string& from, const std::string& error, const std::string& note = {}) {
    json r = {{"format", kFormat}, {"id", id}, {"unit", u.key()}, {"op", op}, {"ok", ok}, {"version", version},
              {"from", from},      {"finished", utc_now()}, {"by", c.env.by}, {"kiln", c.env.kiln}, {"pid", c.env.pid}};
    if (!error.empty()) r["error"] = error;
    if (!note.empty()) r["note"] = note;
    const fs::path dir = c.abs(std::string(kStaged) + "/results");
    std::string err;
    if (!write_json(dir / p8(id + ".json"), r, err)) c.log(Level::Warn, "the result record was not written: " + err);
    prune_results(dir);
}
fs::path attempts_file(const Ctx& c, const Unit& u) { return c.abs(std::string(kStaged) + "/attempts/" + u.key()); }
int bump_attempts(Ctx& c, const Unit& u) {
    const fs::path f = attempts_file(c, u);
    std::string text, why, err;
    int n = 0;
    if (read_text(f, 64, text, why)) n = std::atoi(text.c_str());
    n = std::max(n, 0) + 1;
    write_atomic(f, std::to_string(n) + "\n", err);
    return n;
}
fs::path probation_file(const Ctx& c, const Unit& u) { return c.abs(std::string(kStaged) + "/probation/" + u.key() + ".json"); }
fs::path rollback_file(const Ctx& c, const Unit& u) { return c.abs(std::string(kStaged) + "/rollback/" + u.key() + ".json"); }

// Removes the unit's staged update (staged.json first, so a half-removed folder is never taken for an update).
void drop_staged(Ctx& c, const Unit& u) {
    const fs::path dir = c.abs(u.staged_dir());
    remove_file(dir / "staged.json");
    if (!remove_tree(dir)) c.log(Level::Warn, "could not remove " + u.staged_dir() + " (it is ignored without staged.json)");
}

// ---- checks ----
// Windows also reaches an existing file or folder by another name: its 8.3 short name (CRASH-~1 for crash-reports, where
// the volume makes them, as on most system drives). The path rules look at names (kiln/crash-reports, a folder Kiln's
// receipt lists), so an existing part of a path must be named by its own name; letter case does not count. True on
// POSIX, where a name has no other spelling.
bool own_name(const fs::path& p, std::string& real) {
#ifdef _WIN32
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW(p.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return real = "(its name cannot be read)", false;
    FindClose(h);
    real = s8(fs::path(fd.cFileName));
    return lower(real) == lower(s8(p.filename()));
#else
    (void)p;
    (void)real;
    return true;
#endif
}

// Every folder that exists from the server folder down to `rel`'s parent is a real folder, named by its own name: no
// link or junction leads a rename somewhere else, and no short name stands for a folder the rules keep out.
bool ancestors_plain(const Ctx& c, const std::string& rel, std::string& why) {
    fs::path p = c.env.game;
    const fs::path parent = p8(rel).parent_path();
    for (const auto& seg : parent) {
        p /= seg;
        if (!present(p)) return true;
        if (is_link(p) || !is_dir(p)) return why = s8(p8(rel).parent_path()) + " leads through a link or a file (" + s8(p) + ")", false;
        std::string real;
        if (!own_name(p, real))
            return why = rel + ": " + s8(seg) + " is the folder " + real + " by another name (a Windows short name): use its own name", false;
    }
    return true;
}

// The regular files under `root` (relative, '/'), refusing links and anything that is not a file or a folder.
bool walk_files(const fs::path& root, std::set<std::string>& found, std::string& why) {
    if (!is_dir(root) || is_link(root)) return why = "the files folder is missing or a link", false;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
    if (ec) return why = "cannot list the files: " + ec.message(), false;
    for (; it != end; it.increment(ec)) {
        if (ec) return why = "cannot list the files: " + ec.message(), false;
        const fs::path& p = it->path();
        const std::string rel = s8(p.lexically_relative(root));
        if (is_link(p)) return why = "the staged files hold a link: " + rel, false;
        if (is_dir(p)) continue;
        if (!is_file(p)) return why = "the staged files hold something that is not a file: " + rel, false;
        if (found.size() >= kMaxFiles) return why = "more than " + std::to_string(kMaxFiles) + " files", false;
        found.insert(rel);
    }
    return true;
}

// The files under `root` are exactly `files` (plus `extra`, when present), each with its size and SHA-256.
bool verify_files(const fs::path& root, const std::vector<FileEntry>& files, const std::set<std::string>& extra, std::string& why) {
    std::set<std::string> found;
    if (!walk_files(root, found, why)) return false;
    std::set<std::string> listed;
    for (const auto& f : files) listed.insert(f.path);
    for (const auto& f : found)
        if (!listed.count(f) && !extra.count(f)) return why = f + " is in the staged files but not in staged.json", false;
    for (const auto& f : files) {
        const fs::path p = root / p8(f.path);
        if (!found.count(f.path)) return why = f.path + " is missing from the staged files", false;
        std::error_code ec;
        const uintmax_t size = fs::file_size(p, ec);
        if (ec || size != f.size)
            return why = f.path + ": " + std::to_string(size) + " bytes, staged.json says " + std::to_string(f.size), false;
        const std::string sum = file_sha256(p);
        if (sum != f.sha256)
            return why = f.path + ": checksum mismatch (staged.json says " + f.sha256 + ", the file is " + (sum.empty() ? "unreadable" : sum) + ")", false;
    }
    return true;
}

bool read_entries(const json& doc, const char* key, bool kiln, bool need_size, std::vector<FileEntry>& out,
                  std::set<std::string>& seen, std::string& why) {
    const auto it = doc.find(key);
    if (it == doc.end()) return true;
    if (!it->is_array()) return why = std::string(key) + " is not an array", false;
    if (it->size() > kMaxFiles) return why = std::string(key) + ": more than " + std::to_string(kMaxFiles) + " entries", false;
    for (const auto& e : *it) {
        if (!e.is_object()) return why = std::string(key) + ": an entry is not an object", false;
        FileEntry f;
        f.path = str(e, "path");
        f.sha256 = str(e, "sha256");
        if (!(kiln ? kiln_path_ok(f.path, &why) : rel_path_ok(f.path, &why))) return false;
        if (!lower_hex(f.sha256, 64)) return why = f.path + ": sha256 is not 64 lower-case hex digits", false;
        if (need_size) {
            const auto s = e.find("size");
            if (s == e.end() || !s->is_number_unsigned()) return why = f.path + ": size is not a byte count", false;
            f.size = s->get<uint64_t>();
        }
        if (!seen.insert(lower(f.path)).second) return why = f.path + " is listed twice (letter case does not count on Windows)", false;
        out.push_back(std::move(f));
    }
    return true;
}

std::string mod_version(const fs::path& dir) {
    json m;
    std::string why;
    return read_json(dir / "kiln.mod.json", m, why, true) ? str(m, "version") : std::string();
}

struct Staged {
    std::string id, version, from;
    std::vector<FileEntry> files, removes;
};

// kiln/staged/<unit>/staged.json and its files, checked now: the format, every path, the receipt it was planned on (a
// Kiln update), the mod's own manifest (a mod) and every file's size and checksum.
bool load_staged(Ctx& c, const Unit& u, Staged& s, std::string& why) {
    const fs::path dir = c.abs(u.staged_dir());
    json doc;
    if (!read_json(dir / "staged.json", doc, why)) return false;
    if (!is_one(doc, "format")) return why = "staged.json: format is not 1", false;
    if (str(doc, "kind") != (u.kiln ? "kiln" : "mod")) return why = std::string("staged.json: kind is not ") + (u.kiln ? "kiln" : "mod"), false;
    s.version = str(doc, "version");
    s.from = str(doc, "from");
    if (s.version.empty()) return why = "staged.json: no version", false;
    if (!u.kiln) {
        s.id = str(doc, "id");
        if (str(doc, "folder") != u.folder) return why = "staged.json: folder is not " + u.folder, false;
        if (s.id.empty()) return why = "staged.json: no id", false;
    }
    std::set<std::string> seen;
    if (!doc.contains("files")) return why = "staged.json: no files", false;
    if (!read_entries(doc, "files", u.kiln, true, s.files, seen, why)) return false;
    if (s.files.empty()) return why = "staged.json: no files", false;
    if (u.kiln) {
        if (!read_entries(doc, "remove", true, false, s.removes, seen, why)) return false;
        // the plan is only good for the install it was made from: kilnctl or the launcher may have changed Kiln since
        const auto base = doc.find("base");
        if (base == doc.end() || !base->is_object() || !base->contains("receipt"))
            return why = "staged.json: no base receipt (the install it was planned for)", false;
        const json& r = (*base)["receipt"];
        const fs::path receipt = c.abs("kiln/.install.json");
        const std::string now = present(receipt) ? file_sha256(receipt) : std::string();
        const std::string want = r.is_string() ? r.get<std::string>() : std::string();
        if (!(r.is_null() || r.is_string()) || now != want)
            return why = "Kiln changed since this update was staged (kiln/.install.json is not the one it was planned for): stage it again", false;
    } else {
        if (std::none_of(s.files.begin(), s.files.end(), [](const FileEntry& f) { return f.path == "kiln.mod.json"; }))
            return why = "the staged files have no kiln.mod.json", false;
    }
    if (!verify_files(dir / "files", s.files, u.kiln ? std::set<std::string>{} : std::set<std::string>{".disabled"}, why)) return false;
    if (!u.kiln) {
        json m;
        if (!read_json(dir / "files" / "kiln.mod.json", m, why, true)) return why = "the staged kiln.mod.json: " + why, false;
        if (str(m, "id") != s.id) return why = "the staged kiln.mod.json says id " + dump(json(str(m, "id"))) + ", staged.json says " + s.id, false;
    }
    return true;
}

// Kiln's files in the order they are put in: dinput8.dll second to last (the proxy that runs from the next start), the
// receipt kiln/.install.json last (kilnctl and the launcher read the install from it).
int kiln_rank(const std::string& path) {
    const std::string l = lower(path);
    return l == "kiln/.install.json" ? 2 : l == "dinput8.dll" ? 1 : 0;
}

bool target_ok(const Ctx& c, const Unit& u, const std::string& target, std::string& why) {
    if (!ancestors_plain(c, target, why)) return false;
    const fs::path t = c.abs(target);
    if (!present(t)) return true;
    if (is_link(t)) return why = target + " is a link", false;
    if (u.kiln ? !is_file(t) : !is_dir(t)) return why = target + (u.kiln ? " is not a file" : " is not a folder"), false;
    std::string real;
    if (!own_name(t, real)) return why = target + " is " + real + " by another name (a Windows short name): use its own name", false;
    return true;
}

bool plan_update(Ctx& c, const Unit& u, const Staged& s, Tx& tx, std::string& why) {
    tx.op = "update";
    tx.version = s.version;
    const std::string base = u.tx_dir() + "/prev";
    if (!u.kiln) {
        Step st{u.root(), u.staged_dir() + "/files", u.kept_path(base, u.root()), false};
        if (!target_ok(c, u, st.target, why)) return false;
        st.had = present(c.abs(st.target));
        tx.from = st.had ? mod_version(c.abs(st.target)) : std::string();
        // A folder switched off with .disabled stays switched off.
        const fs::path flag = c.abs(st.source) / ".disabled";
        if (st.had && present(c.abs(st.target) / ".disabled") && !present(flag)) {
            std::string err;
            if (!write_atomic(flag, "", err)) return why = "cannot carry .disabled over: " + err, false;
        }
        tx.steps.push_back(std::move(st));
    } else {
        tx.from = s.from;
        std::vector<Step> files, removes;
        for (const auto& f : s.files) {
            Step st{f.path, u.staged_dir() + "/files/" + f.path, u.kept_path(base, f.path), false};
            if (!target_ok(c, u, st.target, why)) return false;
            st.had = present(c.abs(st.target));
            files.push_back(std::move(st));
        }
        for (const auto& r : s.removes) {
            const fs::path t = c.abs(r.path);
            std::string skip;
            if (!present(t)) continue;  // gone already
            if (!target_ok(c, u, r.path, skip)) {
                c.log(Level::Warn, "Kiln update: " + r.path + " is not removed: " + skip);
                continue;
            }
            if (file_sha256(t) != r.sha256) {
                c.log(Level::Info, "Kiln update: " + r.path + " is kept: it changed since Kiln installed it");
                continue;
            }
            removes.push_back(Step{r.path, "", u.kept_path(base, r.path), true});
        }
        std::stable_sort(files.begin(), files.end(), [](const Step& a, const Step& b) { return kiln_rank(a.target) < kiln_rank(b.target); });
        for (auto& st : files)
            if (kiln_rank(st.target) == 0) tx.steps.push_back(st);
        for (auto& st : removes) tx.steps.push_back(st);
        for (auto& st : files)
            if (kiln_rank(st.target) != 0) tx.steps.push_back(st);
    }
    tx.probation = {{"format", kFormat}, {"unit", u.key()}, {"version", tx.version}, {"from", tx.from},
                    {"state", u.kiln ? "applied" : "pending"}};
    if (!u.kiln) tx.probation["id"] = s.id;
    return true;
}

struct Kept {
    std::string version, replaced_by;
    std::vector<std::pair<std::string, bool>> entries;  // target, present before
    bool has_receipt = false;  // Kiln: the change that kept it said which receipt it left in place
    std::string receipt;       // its SHA-256, "" when there was none
};

// The SHA-256 of kiln/.install.json now, "" when there is none.
std::string receipt_now(const Ctx& c) {
    const fs::path r = c.abs(kReceipt);
    return present(r) ? file_sha256(r) : std::string();
}

bool read_kept(Ctx& c, const Unit& u, const fs::path& file, Kept& k, std::string& why) {
    json doc;
    if (!read_json(file, doc, why)) return false;
    if (!is_one(doc, "format") || str(doc, "unit") != u.key()) return why = "kept.json is not this unit's", false;
    k.version = str(doc, "version");
    k.replaced_by = str(doc, "replacedBy");
    if (const auto r = doc.find("receipt"); u.kiln && r != doc.end()) {
        if (!r->is_string()) return why = "kept.json: receipt is not text", false;
        k.has_receipt = true;
        k.receipt = r->get<std::string>();
    }
    const auto e = doc.find("entries");
    if (e == doc.end() || !e->is_array() || e->empty() || e->size() > kMaxFiles) return why = "kept.json: no entries", false;
    std::set<std::string> seen;
    for (const auto& x : *e) {
        const std::string path = x.is_object() ? str(x, "path") : std::string();
        const std::string before = x.is_object() ? str(x, "before") : std::string();
        if (u.kiln ? !kiln_path_ok(path, &why) : path != u.root()) return why = "kept.json: " + (u.kiln ? why : "an entry is not " + u.root()), false;
        if (before != "present" && before != "absent") return why = "kept.json: before is not present or absent", false;
        if (!seen.insert(lower(path)).second) return why = "kept.json: " + path + " twice", false;
        k.entries.emplace_back(path, before == "present");
    }
    (void)c;
    return true;
}

bool plan_rollback(Ctx& c, const Unit& u, const std::string& op, Tx& tx, std::string& why) {
    const std::string prev = u.previous_dir();
    if (!present(c.abs(prev) / "kept.json")) return why = "no earlier version is kept (" + prev + ")", false;
    Kept k;
    if (!read_kept(c, u, c.abs(prev) / "kept.json", k, why)) return false;
    // The kept version holds only the files its change changed: put back over a Kiln that kilnctl or the launcher
    // installed since (a new receipt), it would make a mix of two versions.
    if (u.kiln && k.has_receipt && receipt_now(c) != k.receipt)
        return why = "Kiln changed since this version was kept (kiln/.install.json is not the one the change to " + version_text(k.replaced_by) +
                     " left; kilnctl or the launcher installed Kiln since): a rollback would mix two versions", false;
    tx.op = op;
    tx.version = k.version;
    tx.from = k.replaced_by;
    const std::string base = u.tx_dir() + "/prev";
    std::vector<Step> steps;
    for (const auto& [path, was] : k.entries) {
        Step st{path, was ? u.kept_path(prev, path) : std::string(), u.kept_path(base, path), false};
        if (was) {
            const fs::path src = c.abs(st.source);
            if (!present(src) || is_link(src) || (u.kiln ? !is_file(src) : !is_dir(src)))
                return why = "the kept version is incomplete: " + st.source + " is missing", false;
        }
        if (!target_ok(c, u, st.target, why)) return false;
        st.had = present(c.abs(st.target));
        if (!st.had && st.source.empty()) continue;  // absent before and absent now
        steps.push_back(std::move(st));
    }
    std::stable_sort(steps.begin(), steps.end(), [](const Step& a, const Step& b) { return kiln_rank(a.target) < kiln_rank(b.target); });
    tx.steps = std::move(steps);
    return true;
}

// ---- the journal ----
json journal_of(const Unit& u, const Tx& tx) {
    json steps = json::array();
    for (const auto& s : tx.steps)
        steps.push_back({{"target", s.target}, {"source", s.source}, {"backup", s.backup}, {"had", s.had}});
    json j = {{"format", kFormat}, {"id", tx.id},        {"unit", u.key()}, {"op", tx.op},       {"version", tx.version},
              {"from", tx.from},   {"started", tx.started}, {"phase", tx.phase}, {"steps", steps}, {"note", tx.note}};
    if (!tx.probation.is_null()) j["probation"] = tx.probation;
    return j;
}

// A journal is only taken when every path in it is where this unit's transactions put things, so a damaged or hand-made
// journal can never move anything else.
bool read_journal(Ctx& c, const Unit& u, const fs::path& file, Tx& tx, std::string& why) {
    json j;
    if (!read_json(file, j, why)) return false;
    if (!is_one(j, "format") || str(j, "unit") != u.key()) return why = "it is not this unit's", false;
    tx.id = str(j, "id");
    tx.op = str(j, "op");
    tx.version = str(j, "version");
    tx.from = str(j, "from");
    tx.started = str(j, "started");
    tx.phase = str(j, "phase");
    tx.note = str(j, "note");
    if (const auto p = j.find("probation"); p != j.end() && p->is_object()) tx.probation = *p;
    if (tx.op != "update" && tx.op != "rollback" && tx.op != "auto-rollback") return why = "unknown op", false;
    if (tx.phase != "applying" && tx.phase != "committed") return why = "unknown phase", false;
    if (tx.id.empty()) tx.id = record_id();
    const auto steps = j.find("steps");
    if (steps == j.end() || !steps->is_array() || steps->size() > kMaxFiles * 2) return why = "no steps", false;
    const std::string backup_root = u.tx_dir() + "/prev/files";
    const std::string source_root = tx.op == "update" ? u.staged_dir() + "/files" : u.previous_dir() + "/files";
    auto under = [](const std::string& p, const std::string& root) { return p == root || starts_with(p, root + "/"); };
    for (const auto& s : *steps) {
        if (!s.is_object() || !s.contains("had") || !s["had"].is_boolean()) return why = "a step is not an object", false;
        Step st{str(s, "target"), str(s, "source"), str(s, "backup"), s["had"].get<bool>()};
        const bool target_fits = u.kiln ? kiln_path_ok(st.target, nullptr) : st.target == u.root();
        if (!target_fits || !rel_path_ok(st.backup, nullptr) || !under(st.backup, backup_root) ||
            (!st.source.empty() && (!rel_path_ok(st.source, nullptr) || !under(st.source, source_root))))
            return why = "a step's paths are not this unit's (" + st.target + ")", false;
        tx.steps.push_back(std::move(st));
    }
    (void)c;
    return true;
}

// Puts every step back, last first. Each step's state comes from what exists now, so this may run after any crash,
// including one in an earlier run of itself.
bool undo_steps(Ctx& c, const Tx& tx, std::string& err) {
    for (auto it = tx.steps.rbegin(); it != tx.steps.rend(); ++it) {
        const Step& s = *it;
        const fs::path t = c.abs(s.target), b = c.abs(s.backup);
        bool tp = present(t);
        const bool bp = present(b);
        if (!s.source.empty()) {
            const fs::path src = c.abs(s.source);
            const bool sp = present(src);
            const bool placed = s.had ? (bp && tp) : (tp && !sp);
            if (placed) {
                if (!move_path(t, src, err)) return false;
                hook("undo-place");
                tp = false;
            }
        }
        if (s.had && bp) {
            if (tp) return err = s.target + " and its kept copy both exist", false;
            if (!move_path(b, t, err)) return false;
            hook("undo-backup");
        }
    }
    return true;
}

// After the commit: tx/<unit>/prev becomes the kept version, what was staged (or the rollback request) goes, the
// probation and the result are written, then the transaction folder. Every part checks before it acts, so a start that
// finds a committed journal runs it again from the top.
bool finish(Ctx& c, const Unit& u, const Tx& tx, std::string& err) {
    const fs::path txd = c.abs(u.tx_dir()), prev = txd / "prev", old = txd / "old-prev", slot = c.abs(u.previous_dir());
    if (present(prev)) {
        if (present(slot)) {
            if (present(old) && !remove_tree(old)) return err = "cannot clear " + s8(old), false;
            if (!move_path(slot, old, err)) return false;
            hook("finish-old");
        }
        if (!move_path(prev, slot, err)) return false;
        hook("finish-prev");
    }
    if (tx.op == "update") {
        drop_staged(c, u);
        hook("finish-staged");
        if (!tx.probation.is_null()) {
            json p = tx.probation;
            p["applied"] = tx.started;
            std::string werr;
            if (!write_json(probation_file(c, u), p, werr)) c.log(Level::Warn, u.name() + ": the probation record was not written: " + werr);
        }
    } else {
        remove_file(rollback_file(c, u));
        remove_file(probation_file(c, u));
        if (present(c.abs(u.staged_dir()))) {  // the ops refuse this pair; a hand-made one ends here
            c.log(Level::Warn, u.name() + ": the update staged next to the rollback was dropped (stage it again if it is still wanted)");
            drop_staged(c, u);
        }
    }
    remove_file(attempts_file(c, u));
    remove_if_empty(attempts_file(c, u).parent_path());
    record(c, u, tx.id, tx.op, true, tx.version, tx.from, "", tx.note);
    remove_file(txd / "journal.json");
    hook("finish-journal");
    remove_tree(txd);
    remove_if_empty(txd.parent_path());
    return true;
}

enum class Outcome { Done, Failed, Stuck };

// Runs a planned transaction. Failed: nothing changed (or all of it was put back); Stuck: the journal stays for the
// next start, which puts it back.
Outcome execute(Ctx& c, const Unit& u, Tx& tx, std::string& why) {
    const fs::path txd = c.abs(u.tx_dir());
    // An open journal (a commit whose tidying-up failed) is the next start's to finish; never cleared from here.
    if (present(txd / "journal.json")) return why = "an earlier change of this unit is still open (" + u.tx_dir() + ")", Outcome::Failed;
    if (present(txd) && !remove_tree(txd)) return why = "cannot clear " + u.tx_dir(), Outcome::Failed;
    tx.id = record_id() + "-" + u.key();
    tx.started = utc_now();
    tx.phase = "applying";
    json entries = json::array();
    for (const auto& s : tx.steps) entries.push_back({{"path", s.target}, {"before", s.had ? "present" : "absent"}});
    json kept = {{"format", kFormat}, {"unit", u.key()},  {"version", tx.from}, {"replacedBy", tx.version},
                 {"kept", tx.started}, {"reason", tx.op}, {"entries", entries}};
    if (u.kiln) {  // the receipt this change leaves in place: a later rollback checks Kiln is still that install
        std::string after = receipt_now(c);
        for (const auto& s : tx.steps)
            if (lower(s.target) == kReceipt) after = s.source.empty() ? std::string() : file_sha256(c.abs(s.source));
        kept["receipt"] = after;
    }
    std::string err;
    if (!write_json(txd / "prev" / "kept.json", kept, err) || !write_json(txd / "journal.json", journal_of(u, tx), err)) {
        remove_tree(txd);
        return why = err, Outcome::Failed;
    }
    hook("journal");
    bool ok = true;
    for (const auto& s : tx.steps) {
        if (s.had) {
            if (!move_path(c.abs(s.target), c.abs(s.backup), err)) {
                ok = false;
                break;
            }
            hook("backup");
        }
        if (!s.source.empty()) {
            if (!move_path(c.abs(s.source), c.abs(s.target), err)) {
                ok = false;
                break;
            }
            hook("place");
        }
    }
    if (ok) {
        tx.phase = "committed";
        ok = write_json(txd / "journal.json", journal_of(u, tx), err);
    }
    if (!ok) {
        why = err;
        std::string undo;
        if (!undo_steps(c, tx, undo)) {
            why += "; putting it back failed too: " + undo + " (the next start tries again)";
            return Outcome::Stuck;
        }
        remove_tree(txd);
        remove_if_empty(txd.parent_path());
        return Outcome::Failed;
    }
    hook("commit");
    if (!finish(c, u, tx, err)) c.log(Level::Warn, u.name() + ": the " + op_text(tx.op) + " is in place; tidying up stopped (" + err + "), the next start finishes it");
    return Outcome::Done;
}

// A staged change that failed or was interrupted kMaxAttempts times is dropped, so no start keeps trying it for ever.
void give_up(Ctx& c, const Unit& u, const std::string& op) {
    if (op == "update") drop_staged(c, u);
    else {
        remove_file(rollback_file(c, u));
        remove_file(probation_file(c, u));
    }
    remove_file(attempts_file(c, u));
    remove_if_empty(attempts_file(c, u).parent_path());
}

// ---- recovery ----
// False: this unit is left alone for this start (a journal that cannot be read or put back).
bool recover(Ctx& c, const Unit& u) {
    const fs::path txd = c.abs(u.tx_dir());
    if (!present(txd)) return true;
    const fs::path j = txd / "journal.json";
    if (!present(j)) {  // ended before its journal (nothing was renamed) or right after the last tidying-up step
        remove_tree(txd);
        remove_if_empty(txd.parent_path());
        return true;
    }
    Tx tx;
    std::string why;
    if (!read_journal(c, u, j, tx, why)) {
        c.log(Level::Error, u.name() + ": the journal " + u.tx_dir() + "/journal.json cannot be used (" + why +
                                "); nothing of this unit is touched: put its files right and remove that folder by hand");
        ++c.sum.failed;
        return false;
    }
    ++c.sum.recovered;
    std::string err;
    if (tx.phase == "committed") {
        if (!finish(c, u, tx, err)) {
            c.log(Level::Error, u.name() + ": finishing the interrupted " + op_text(tx.op) + " to " + version_text(tx.version) + " failed: " + err);
            return false;
        }
        c.log(Level::Info, u.name() + ": the " + op_text(tx.op) + " to " + version_text(tx.version) +
                               " an earlier start was interrupted in had already committed; it is finished now");
        ++(tx.op == "update" ? c.sum.applied : c.sum.rolled_back);
        return true;
    }
    if (!undo_steps(c, tx, err)) {
        c.log(Level::Error, u.name() + ": the interrupted " + op_text(tx.op) + " to " + version_text(tx.version) +
                                " could not be put back (" + err + "); it is left as it is for the next start");
        return false;
    }
    remove_tree(txd);
    remove_if_empty(txd.parent_path());
    const int n = bump_attempts(c, u);
    c.log(Level::Warn, u.name() + ": the " + op_text(tx.op) + " to " + version_text(tx.version) +
                           " was interrupted (a crash or a power cut); it was put back to " + version_text(tx.from) + " (attempt " +
                           std::to_string(n) + " of " + std::to_string(kMaxAttempts) + ")");
    if (n >= kMaxAttempts) {
        c.log(Level::Error, u.name() + ": the " + op_text(tx.op) + " to " + version_text(tx.version) + " was interrupted " +
                                std::to_string(n) + " times and is dropped");
        record(c, u, record_id() + "-" + u.key(), tx.op, false, tx.version, tx.from, "interrupted " + std::to_string(n) + " times");
        give_up(c, u, tx.op);
        ++c.sum.failed;
    }
    return true;
}

// ---- the three kinds of change ----
// Returns true when the transaction committed.
bool run_tx(Ctx& c, const Unit& u, Tx& tx) {
    std::string why;
    const Outcome o = execute(c, u, tx, why);
    if (o == Outcome::Done) return true;
    ++c.sum.failed;
    if (o == Outcome::Stuck) {
        c.log(Level::Error, u.name() + ": the " + op_text(tx.op) + " to " + version_text(tx.version) + " failed: " + why);
        return false;
    }
    const int n = bump_attempts(c, u);
    c.log(Level::Error, u.name() + ": the " + op_text(tx.op) + " to " + version_text(tx.version) + " failed and nothing changed: " + why +
                            " (attempt " + std::to_string(n) + " of " + std::to_string(kMaxAttempts) + ")");
    if (n >= kMaxAttempts) {
        record(c, u, record_id() + "-" + u.key(), tx.op, false, tx.version, tx.from, why);
        give_up(c, u, tx.op);
    }
    return false;
}

bool auto_rollback(Ctx& c, const Unit& u, const json& p) {
    const std::string version = str(p, "version");
    std::string reason;
    if (u.kiln)
        reason = "the start after the update to " + version_text(version) +
                 " did not get as far as loading the mods (it crashed, its boot failed, or it was stopped before then)";
    else if (str(p, "state") == "failed")
        reason = "it failed to load right after its update to " + version_text(version) + ": " + str(p, "error");
    else
        reason = "the start after its update to " + version_text(version) + " ended while it was loading";
    Tx tx;
    std::string why;
    Kept k;
    const fs::path kept = c.abs(u.previous_dir()) / "kept.json";
    if (!present(kept)) why = "no earlier version is kept";
    else if (!read_kept(c, u, kept, k, why)) {
    } else if (k.replaced_by != version) why = "the kept version did not come before " + version_text(version);
    else plan_rollback(c, u, "auto-rollback", tx, why);
    if (!why.empty()) {
        c.log(Level::Error, u.name() + ": " + reason + "; it cannot be rolled back (" + why + ") and stays");
        record(c, u, record_id() + "-" + u.key(), "auto-rollback", false, k.version, version, why, reason);
        remove_file(probation_file(c, u));  // once: never again for this update
        ++c.sum.failed;
        return false;
    }
    tx.note = reason;
    if (!run_tx(c, u, tx)) return false;
    c.log(Level::Warn, u.name() + ": " + reason + "; rolled back to " + version_text(tx.version) +
                           " (once: Kiln does not roll back the version it went back to)");
    ++c.sum.rolled_back;
    return true;
}

bool requested_rollback(Ctx& c, const Unit& u) {
    json req;
    std::string why;
    std::string note = "requested";
    if (read_json(rollback_file(c, u), req, why) && !str(req, "by").empty()) note = "requested by " + str(req, "by");
    why.clear();
    Tx tx;
    if (!plan_rollback(c, u, "rollback", tx, why)) {
        c.log(Level::Error, u.name() + ": the requested rollback is refused: " + why);
        record(c, u, record_id() + "-" + u.key(), "rollback", false, "", "", why, note);
        remove_file(rollback_file(c, u));
        ++c.sum.failed;
        return false;
    }
    tx.note = note;
    if (!run_tx(c, u, tx)) return false;
    c.log(Level::Info, u.name() + ": rolled back to " + version_text(tx.version) + " (from " + version_text(tx.from) + ", " + note + ")");
    ++c.sum.rolled_back;
    return true;
}

// ---- free space ----
constexpr uint64_t kUnknown = UINT64_MAX;
// The bytes free to this process on the volume holding `dir` (the nearest folder of it that exists); kUnknown when the
// volume cannot be asked.
uint64_t free_bytes(fs::path dir) {
    while (!dir.empty() && !is_dir(dir) && dir.has_parent_path() && dir.parent_path() != dir) dir = dir.parent_path();
    if (g_free_bytes_hook) return g_free_bytes_hook(dir);
#ifdef _WIN32
    ULARGE_INTEGER avail{}, total{}, all{};
    return GetDiskFreeSpaceExW(dir.c_str(), &avail, &total, &all) ? avail.QuadPart : kUnknown;
#else
    struct statvfs v{};
    return ::statvfs(dir.c_str(), &v) == 0 ? static_cast<uint64_t>(v.f_bavail) * v.f_frsize : kUnknown;
#endif
}
// The world's savegame folder, as the game and Kiln.Saves find it: enshrouded_server.json saveDirectory, else
// <game>/savegame.
fs::path save_dir(const Ctx& c) {
    fs::path dir = c.env.game / "savegame";
    json cfg;
    std::string why;
    if (read_json(c.env.game / "enshrouded_server.json", cfg, why) && cfg.contains("saveDirectory") && cfg["saveDirectory"].is_string()) {
        const fs::path d = p8(cfg["saveDirectory"].get<std::string>());
        dir = d.is_absolute() ? d : c.env.game / d;
    }
    return dir.lexically_normal();
}
uint64_t tree_bytes(const fs::path& dir) {
    uint64_t n = 0;
    std::error_code ec;
    if (!is_dir(dir)) return 0;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!is_file(it->path())) continue;
        std::error_code e2;
        const uintmax_t size = fs::file_size(it->path(), e2);
        if (!e2) n += size;
    }
    return n;
}
std::string bytes_text(uint64_t n) {
    char b[64];
    if (n >= (uint64_t{1} << 30)) std::snprintf(b, sizeof b, "%llu bytes (%.2f GiB)", static_cast<unsigned long long>(n), n / 1073741824.0);
    else std::snprintf(b, sizeof b, "%llu bytes (%.1f MiB)", static_cast<unsigned long long>(n), n / 1048576.0);
    return b;
}
// A staged update waits while the volume of kiln/staged has less free than 2 GiB plus the world's savegame folder: the
// apply itself needs no room (renames), but on a volume that full the world's next save is at risk, and an apply whose
// journal could not be written would fail three starts in a row and be dropped. The update stays staged; kiln.log says
// so. A volume that cannot be asked holds nothing back.
bool room_to_apply(Ctx& c, const Unit& u, const std::string& version) {
    const uint64_t free = free_bytes(c.abs(kStaged));
    if (free == kUnknown) return true;
    const uint64_t saves = tree_bytes(save_dir(c));
    const uint64_t need = kFreeHeadroom + saves;
    if (free >= need) return true;
    c.log(Level::Warn, u.name() + ": the staged update to " + version + " waits for free space: the volume of kiln/staged has " +
                           bytes_text(free) + " free; a start applies an update only with 2 GiB plus the world's savegame folder (" +
                           bytes_text(saves) + ") free, " + bytes_text(need) + " in all. It stays staged; the next start tries again");
    return false;
}

void staged_update(Ctx& c, const Unit& u) {
    Staged s;
    Tx tx;
    std::string why;
    const bool loaded = load_staged(c, u, s, why);
    if (loaded && !room_to_apply(c, u, s.version)) {
        ++c.sum.held;
        return;
    }
    if (!loaded || !plan_update(c, u, s, tx, why)) {
        c.log(Level::Error, u.name() + ": the staged update" + (s.version.empty() ? "" : " to " + s.version) + " is refused and removed: " + why);
        record(c, u, record_id() + "-" + u.key(), "update", false, s.version, s.from, why);
        drop_staged(c, u);
        remove_file(attempts_file(c, u));
        ++c.sum.failed;
        return;
    }
    if (!run_tx(c, u, tx)) return;
    c.log(Level::Info, u.name() + ": updated to " + tx.version + " (from " + version_text(tx.from) +
                           "); the replaced version is kept for one rollback" +
                           (u.kiln ? "" : "; Kiln.Host confirms it when it loads"));
    ++c.sum.applied;
}

void run_unit(Ctx& c, const Unit& u) {
    if (!recover(c, u)) return;
    bool rolled = false;
    const fs::path pf = probation_file(c, u);
    if (present(pf)) {
        json p;
        std::string why;
        if (!read_json(pf, p, why) || str(p, "unit") != u.key()) {
            c.log(Level::Warn, u.name() + ": the probation record cannot be read (" + why + "); removed");
            remove_file(pf);
        } else if (u.kiln ? str(p, "state") == "booting" : str(p, "state") == "loading" || str(p, "state") == "failed") {
            // Kiln: "booting" = an earlier start loaded the new kiln-core and Kiln.Host never confirmed it ("applied" =
            // it never got that far: no verdict yet). A mod: Kiln.Host's own verdict, or "loading" left by a start that
            // ended while the mod loaded ("pending" = not tried yet: the server stopped first, or the mod is off).
            rolled = auto_rollback(c, u, p);
        }
    }
    if (!rolled && present(rollback_file(c, u))) rolled = requested_rollback(c, u);
    if (present(c.abs(u.tx_dir()) / "journal.json")) return;  // committed, tidying up waits for the next start
    const fs::path dir = c.abs(u.staged_dir());
    if (present(dir / "staged.json")) staged_update(c, u);
    else if (present(dir)) remove_tree(dir);  // a folder without its manifest: a leftover of a removal
}

std::set<std::string> unit_names(const fs::path& dir, const std::string& prefix, const std::string& suffix, bool dirs) {
    std::set<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = s8(it->path().filename());
        if (dirs != is_dir(it->path())) continue;
        if (n.size() <= prefix.size() + suffix.size() || !starts_with(n, prefix) || n.compare(n.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        out.insert(n.substr(prefix.size(), n.size() - prefix.size() - suffix.size()));
    }
    return out;
}

}  // namespace

Summary boot_kiln(const Env& env) {
    Ctx c{env, {}};
    const fs::path root = c.abs(kStaged);
    if (!present(root)) return {};
    if (!env.server) {
        if (kiln_update_waiting(env.game)) c.log(Level::Warn, "a Kiln update is staged; staged updates are for dedicated servers, not applied here");
        return {};
    }
    const Unit kiln{true, {}};
    run_unit(c, kiln);
    // This start loads the kiln-core that is in place now: a probation not yet started begins here, so a start that
    // ends before Kiln.Host confirms it (after the mods loaded) is rolled back by the next one.
    const fs::path pf = probation_file(c, kiln);
    json p;
    std::string why, err;
    if (present(pf) && read_json(pf, p, why) && str(p, "state") == "applied") {
        p["state"] = "booting";
        p["booted"] = utc_now();
        p["pid"] = env.pid;
        if (!write_json(pf, p, err)) c.log(Level::Warn, "Kiln: the probation record was not updated: " + err);
    }
    return c.sum;
}

Summary boot_mods(const Env& env) {
    Ctx c{env, {}};
    const fs::path root = c.abs(kStaged);
    if (!present(root)) return {};
    if (!env.server) {
        if (!unit_names(root / "mods", "", "", true).empty())
            c.log(Level::Warn, "mod updates are staged; staged updates are for dedicated servers, not applied here");
        return {};
    }
    std::set<std::string> folders = unit_names(root / "mods", "", "", true);
    for (const auto& n : unit_names(root / "tx", "mod.", "", true)) folders.insert(n);
    for (const auto& n : unit_names(root / "rollback", "mod.", ".json", false)) folders.insert(n);
    for (const auto& n : unit_names(root / "probation", "mod.", ".json", false)) folders.insert(n);
    for (const auto& f : folders) {
        if (!mod_folder_ok(f)) {
            c.log(Level::Warn, "ignored " + dump(json(f)) + ": not a mod folder name a staged update may use");
            continue;
        }
        run_unit(c, Unit{false, f});
    }
    // Kiln.Admin stages into kiln/staged/.stage-<random> and renames it into place: one left here was interrupted.
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (starts_with(s8(it->path().filename()), ".stage-")) remove_tree(it->path());
    return c.sum;
}

bool confirm_kiln(const std::filesystem::path& game) {
    const fs::path f = game / "kiln" / "staged" / "probation" / "kiln.json";
    if (!present(f)) return false;
    remove_file(f);
    return !present(f);
}

bool kiln_update_waiting(const std::filesystem::path& game) { return present(game / "kiln" / "staged" / "kiln" / "staged.json"); }

void save_proxy_lines(const std::filesystem::path& game, uint64_t pid, const std::vector<std::pair<Level, std::string>>& lines) {
    json arr = json::array();
    for (const auto& [level, text] : lines)
        arr.push_back({{"level", level == Level::Error ? "error" : level == Level::Warn ? "warn" : "info"}, {"text", text}});
    std::string err;
    write_json(game / "kiln" / "staged" / "proxy-lines.json", {{"format", kFormat}, {"pid", pid}, {"lines", arr}}, err);
}

std::vector<std::pair<Level, std::string>> take_proxy_lines(const std::filesystem::path& game, uint64_t pid) {
    const fs::path file = game / "kiln" / "staged" / "proxy-lines.json";
    std::vector<std::pair<Level, std::string>> out;
    if (!present(file)) return out;
    json j;
    std::string why;
    if (read_json(file, j, why) && j.value("pid", uint64_t{0}) == pid && j.contains("lines") && j["lines"].is_array()) {
        for (const auto& l : j["lines"]) {
            if (!l.is_object()) continue;
            const std::string level = str(l, "level");
            out.emplace_back(level == "error" ? Level::Error : level == "warn" ? Level::Warn : Level::Info, str(l, "text"));
        }
    }
    remove_file(file);
    return out;
}

}  // namespace kiln::staged
