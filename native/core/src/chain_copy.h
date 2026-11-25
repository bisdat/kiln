// SPDX-License-Identifier: MIT
// A native mod the profile has off whose DLL the player also put in as a proxy (0.2.0-dev.31.12): the installer moved a
// dinput8.dll to kiln/chain/ and listed it in kiln/chain.ini [chain] (or EMF's dinput8-chain.ini lists it). Kiln's proxy
// loads those lines in its DllMain, before kiln-core reads the profile, so that copy runs whatever the profile says;
// state/native.json said "disabled in the profile" all the same. native_mods.cpp resolve() finds such a copy (the same
// bytes as one of the mod's DLLs) and says that it loads as a chain proxy and how to take it out.
// Header only and free of Windows calls (tests/chain_copy_test.cpp, tests/test-chain-copy.sh).
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace kiln::chain_copy {

// One DLL the proxy loaded (or will load) from a list the profile does not decide.
struct Row {
    std::filesystem::path file;  // the DLL as a path to read
    std::string path_text;       // the same as the proxy reports it (state/boot.json "path")
    bool emf_list = false;       // from <game>/dinput8-chain.ini (EMF's list), else kiln/chain.ini [chain]
    std::string status;          // chain_status_name: "loaded", "duplicate", "late" count; anything else did not load
};

inline bool loads(const Row& r) { return r.status == "loaded" || r.status == "duplicate" || r.status == "late"; }

// The two files hold the same bytes (a DLL copied under another name). False when either cannot be read.
inline bool same_bytes(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    const auto na = std::filesystem::file_size(a, ec);
    if (ec) return false;
    const auto nb = std::filesystem::file_size(b, ec);
    if (ec || na != nb || na == 0) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    char ba[65536], bb[65536];
    for (uintmax_t left = na; left > 0;) {
        const auto n = static_cast<std::streamsize>(left < sizeof ba ? left : sizeof ba);
        if (!fa.read(ba, n) || !fb.read(bb, n)) return false;
        if (std::char_traits<char>::compare(ba, bb, static_cast<size_t>(n)) != 0) return false;
        left -= static_cast<uintmax_t>(n);
    }
    return true;
}

// The first row that loads and holds the same bytes as `dll`; -1 when none.
inline int find(const std::filesystem::path& dll, const std::vector<Row>& rows) {
    for (size_t i = 0; i < rows.size(); ++i)
        if (loads(rows[i]) && same_bytes(dll, rows[i].file)) return static_cast<int>(i);
    return -1;
}

// Where the line is and how to take it out (a sentence ending without a full stop).
inline std::string how_to_remove(const Row& r) {
    if (r.emf_list)
        return "to stop it, remove its line from <game>\\dinput8-chain.ini (EMF's list) or move " + r.path_text +
               " out of the game folder, then restart the game";
    return "to stop it, put ';' in front of its line in kiln\\chain.ini [chain] or move " + r.path_text +
           " out of the game folder, then restart the game";
}

inline std::string list_text(const Row& r) { return r.emf_list ? "dinput8-chain.ini" : "kiln\\chain.ini [chain]"; }

// native.json's reason (and the kiln.log line) for a mod that is off (`off_reason`: "disabled in the profile" ...) whose
// DLL `dll_file` loads as the chain proxy `r`. `dllmain`: the mod is phase dllmain, so that load is the whole mod.
inline std::string reason(const std::string& off_reason, const std::string& dll_file, const Row& r, bool dllmain) {
    std::string s = off_reason + ", but its DLL " + dll_file + " loads all the same as a chain proxy: " + list_text(r) +
                    " names " + r.path_text + " (the same file), which Kiln's proxy loads " +
                    (r.status == "late" ? "after the plugins ([late])" : "before kiln-core starts") +
                    ", whatever the profile says. ";
    s += dllmain ? std::string("The mod runs (phase dllmain: loading its DLL starts it)")
                 : std::string("Its DllMain runs; Kiln does not call its init, so what it starts there does not start");
    return s + "; " + how_to_remove(r);
}

}  // namespace kiln::chain_copy
