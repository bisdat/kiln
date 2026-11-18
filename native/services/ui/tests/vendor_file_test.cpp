// SPDX-License-Identifier: MIT
// Host test of the vendor file check (vendor_file.h) and its SHA-256 (sha256.h): the standard's vectors and the padding
// boundaries, streaming in any piece sizes, every way a component's files can be wrong (missing, no receipt or not this
// component's, a link on the way, the wrong size, the wrong bytes, unreadable), the handles kept open on success and closed on failure. With the folder of
// the real vendor files as the first argument (dev29/vendor-files), also their hashes against Kiln's pins, through the
// same check the game runs. Linux files stand in for Windows': a Unix link is what Wine shows as a reparse point.
//   tests/vendor_file_test <out dir> [<vendor-files dir>]   (built with -I native/third_party, for nlohmann/json)
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "../vendor_file.h"

using namespace kui;
using namespace kui::vendor;
namespace fs = std::filesystem;

static int g_checks = 0;
#define CHECK(...)                                                                                   \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                        \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #__VA_ARGS__);     \
            std::exit(1);                                                                            \
        }                                                                                            \
    } while (0)

static std::string sha(const std::string& s) {
    Sha256 h;
    h.update(s.data(), s.size());
    return h.hex();
}

static void sha256_vectors() {
    CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // the padding's boundaries: 55 bytes fit one block with the length, 56..63 need a second one
    const std::map<size_t, const char*> a = {
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
        {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
        {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
        {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"},
    };
    for (const auto& [n, want] : a) {
        CHECK(sha(std::string(n, 'a')) == want);
        // the same bytes in pieces of every size from 1 to 70
        for (size_t piece = 1; piece <= 70; ++piece) {
            Sha256 h;
            const std::string s(n, 'a');
            for (size_t at = 0; at < s.size(); at += piece) h.update(s.data() + at, std::min(piece, s.size() - at));
            CHECK(h.hex() == want);
        }
    }
    // a million 'a's, fed in uneven pieces
    Sha256 h;
    const std::string block(9973, 'a');
    size_t left = 1000000;
    while (left) {
        const size_t n = std::min(left, block.size());
        h.update(block.data(), n);
        left -= n;
    }
    CHECK(h.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void utf_round_trip() {
    CHECK(widen("kiln/vendor") == L"kiln/vendor");
    const std::string s = "C:\\Spiele\\Enshrouded \xc3\xa4\xe2\x82\xac\xf0\x9f\x94\xa5";  // a, euro sign, a 4-byte one
    CHECK(narrow(widen(s)) == s);
}

// Linux files standing in for Windows': "\\" in the game's paths is "/" here.
class PosixFs final : public Fs {
  public:
    static std::string p(const std::wstring& w) {
        std::string s = narrow(w);
        for (auto& c : s)
            if (c == '\\') c = '/';
        return s;
    }
    bool exists(const std::wstring& path) override {
        std::error_code ec;
        return fs::exists(fs::symlink_status(p(path), ec));
    }
    bool is_link(const std::wstring& path) override {
        std::error_code ec;
        return fs::is_symlink(fs::symlink_status(p(path), ec));
    }
    Handle open_locked(const std::wstring& path, uint32_t* error) override {
        FILE* f = std::fopen(p(path).c_str(), "rb");
        if (!f && error) *error = 5;  // ERROR_ACCESS_DENIED
        if (f) ++open;
        return f;
    }
    bool handle_is_link(Handle) override { return false; }
    bool size(Handle h, uint64_t* out) override {
        struct stat st{};
        if (fstat(fileno(static_cast<FILE*>(h)), &st) != 0) return false;
        *out = static_cast<uint64_t>(st.st_size);
        return true;
    }
    bool read(Handle h, void* buf, uint32_t want, uint32_t* got) override {
        *got = static_cast<uint32_t>(std::fread(buf, 1, want, static_cast<FILE*>(h)));
        return !std::ferror(static_cast<FILE*>(h));
    }
    void close(Handle h) override {
        std::fclose(static_cast<FILE*>(h));
        --open;
    }
    int open = 0;
};

static void write_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
static void sparse_file(const fs::path& path, uint64_t size) {  // `size` zero bytes, without writing them
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).close();
    fs::resize_file(path, size);
}
// The launcher's receipt (vendor-receipt.schema.json); Kiln reads only `format` and `component`.
static void receipt(const fs::path& dir, const std::string& component, int format = 1) {
    write_file(dir / "kiln-vendor.json", "{\n  \"format\": " + std::to_string(format) + ",\n  \"component\": \"" + component +
                                            "\",\n  \"vendor\": \"x\",\n  \"version\": \"1\",\n  \"files\": [{\"name\": \"../../evil.dll\"}]\n}\n");
}
static void place(const fs::path& from, const fs::path& to) {  // the real file, without copying 78 MB when it can
    fs::create_directories(to.parent_path());
    std::error_code ec;
    fs::remove(to, ec);
    fs::create_hard_link(from, to, ec);
    if (ec) fs::copy_file(from, to);
}

static void pins_are_sane() {
    CHECK(component_pins("intel-xess").size() == 1);
    CHECK(component_pins("amd-fsr").size() == 3);
    CHECK(component_pins("nobody").empty());
    const Pin* x = component_pins("intel-xess")[0];
    CHECK(std::string(x->vendor) == "intel" && std::string(x->version) == "3.0.2" && std::string(x->name) == "libxess.dll");
    CHECK(x->size == 77795704u && std::string(x->sha256) == "251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7");
    for (const Pin& p : kPins) {
        CHECK(std::strlen(p.sha256) == 64);
        CHECK(std::strchr(p.name, '/') == nullptr && std::strchr(p.name, '\\') == nullptr);
    }
    CHECK(check_why(Check::ok) == 0 && check_why(Check::missing) == 2 && check_why(Check::hash) == 3 && check_why(Check::size) == 3 &&
          check_why(Check::link) == 3 && check_why(Check::unreadable) == 3 && check_why(Check::no_pins) == 12);
}

static void wrong_files(const fs::path& out) {
    PosixFs pfs;
    const fs::path root = out / "vendor-file-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path kiln = root / "game" / "kiln";
    const std::wstring kw = widen(kiln.string());
    const fs::path dll = kiln / "vendor" / "intel" / "3.0.2" / "libxess.dll";

    // nothing downloaded
    fs::create_directories(kiln);
    Verified v = verify_component(pfs, kw, "intel-xess");
    CHECK(v.check == Check::missing && !v.ok() && v.handles.empty() && v.paths.empty());
    CHECK(v.detail.find("kiln/vendor/intel/3.0.2 is not there") != std::string::npos);
    // no such component
    v = verify_component(pfs, kw, "nvidia-dlss");
    CHECK(v.check == Check::no_pins);
    // the folder, but no receipt (a download that did not finish), even with the right file in it
    fs::create_directories(dll.parent_path());
    v = verify_component(pfs, kw, "intel-xess");
    CHECK(v.check == Check::no_receipt && v.detail.find("has no receipt") != std::string::npos && check_why(v.check) == 2);
    // a receipt of another component, of another format, broken, too big, not an object
    const fs::path ver = dll.parent_path();
    receipt(ver, "amd-fsr");
    v = verify_component(pfs, kw, "intel-xess");
    CHECK(v.check == Check::no_receipt && v.detail.find("is not intel-xess's receipt") != std::string::npos && pfs.open == 0);
    receipt(ver, "intel-xess", 2);
    v = verify_component(pfs, kw, "intel-xess");
    CHECK(v.check == Check::no_receipt && v.detail.find("format 1") != std::string::npos);
    write_file(ver / "kiln-vendor.json", "{\"format\": 1, \"component\": \"intel-xe");
    v = verify_component(pfs, kw, "intel-xess");
    CHECK(v.check == Check::no_receipt && pfs.open == 0);
    write_file(ver / "kiln-vendor.json", "{\"format\": \"1\", \"component\": \"intel-xess\"}");
    CHECK(verify_component(pfs, kw, "intel-xess").check == Check::no_receipt);
    write_file(ver / "kiln-vendor.json", "[1, \"intel-xess\"]");
    CHECK(verify_component(pfs, kw, "intel-xess").check == Check::no_receipt);
    write_file(ver / "kiln-vendor.json", "{\"format\": 1, \"component\": \"intel-xess\", \"pad\": \"" + std::string(70000, 'x') + "\"}");
    v = verify_component(pfs, kw, "intel-xess");
    CHECK(v.check == Check::no_receipt && v.detail.find("could not be read") != std::string::npos);
    // a receipt that is a link
    fs::remove(ver / "kiln-vendor.json");
    receipt(root / "elsewhere", "intel-xess");
    fs::create_symlink(root / "elsewhere" / "kiln-vendor.json", ver / "kiln-vendor.json");
    v = verify_component(pfs, kw, "intel-xess");
    CHECK(v.check == Check::link && v.detail.find("kiln-vendor.json is a link") != std::string::npos);
    fs::remove(ver / "kiln-vendor.json");
    // the right receipt (its file list, with a path in it, is never used), but not the file
    receipt(ver, "intel-xess");
    v = verify_component(pfs, kw, "intel-xess");
    CHECK(v.check == Check::missing && v.detail.find("libxess.dll is missing") != std::string::npos && pfs.open == 0);
    // the wrong size
    write_file(dll, "not xess");
    v = verify_component(pfs, kw, "intel-xess");
    CHECK(v.check == Check::size && v.handles.empty() && pfs.open == 0);
    CHECK(v.detail.find("has 8 bytes, not the 77795704") != std::string::npos);
    // the right size, other bytes (78 MB of zeros, sparse)
    sparse_file(dll, 77795704u);
    v = verify_component(pfs, kw, "intel-xess");
    CHECK(v.check == Check::hash && v.handles.empty() && pfs.open == 0);
    CHECK(v.detail.find("is not the file Kiln pinned") != std::string::npos);
    // a link instead of the file
    fs::remove(dll);
    write_file(root / "elsewhere" / "libxess.dll", "x");
    fs::create_symlink(root / "elsewhere" / "libxess.dll", dll);
    v = verify_component(pfs, kw, "intel-xess");
    CHECK(v.check == Check::link && v.detail.find("libxess.dll is a link") != std::string::npos);
    fs::remove(dll);
    // a link on the way: the version folder, the vendor folder, kiln/vendor
    fs::remove_all(kiln / "vendor");
    fs::create_directories(root / "other" / "3.0.2");
    fs::create_directories(kiln / "vendor" / "intel");
    fs::create_directory_symlink(root / "other" / "3.0.2", kiln / "vendor" / "intel" / "3.0.2");
    receipt(root / "other" / "3.0.2", "intel-xess");
    v = verify_component(pfs, kw, "intel-xess");
    CHECK(v.check == Check::link && v.detail.find("kiln/vendor/intel/3.0.2 is a link") != std::string::npos);
    fs::remove_all(kiln / "vendor");
    fs::create_directories(kiln / "vendor");
    fs::create_directory_symlink(root / "other", kiln / "vendor" / "intel");
    v = verify_component(pfs, kw, "intel-xess");
    CHECK(v.check == Check::link && v.detail.find("kiln/vendor/intel is a link") != std::string::npos);
    fs::remove_all(kiln / "vendor");
    fs::create_directory_symlink(root / "other", kiln / "vendor");
    v = verify_component(pfs, kw, "intel-xess");
    CHECK(v.check == Check::link && v.detail.find("kiln/vendor is a link") != std::string::npos);
    fs::remove(kiln / "vendor");
    // unreadable (not as root, who reads anything)
    if (geteuid() != 0) {
        receipt(dll.parent_path(), "intel-xess");
        write_file(dll, "x");
        fs::permissions(dll, fs::perms::none);
        v = verify_component(pfs, kw, "intel-xess");
        CHECK(v.check == Check::unreadable && v.detail.find("could not be opened") != std::string::npos);
        fs::permissions(dll, fs::perms::owner_all);
    }
    // a component of several files: the first wrong one is reported, and every handle opened before it is closed again
    const fs::path amd = kiln / "vendor" / "amd" / "2.4.0";
    receipt(amd, "amd-fsr");
    write_file(amd / "amd_fidelityfx_loader_vk.dll", "short");
    v = verify_component(pfs, kw, "amd-fsr");
    CHECK(v.check == Check::size && v.detail.find("amd_fidelityfx_loader_vk.dll") != std::string::npos && pfs.open == 0);
    fs::remove_all(root, ec);
}

static void real_files(const fs::path& out, const fs::path& vendor_files) {
    const fs::path xess = vendor_files / "intel-xess-3.0.2" / "libxess.dll";
    const fs::path amd = vendor_files / "amd-fsr-2.4.0";
    if (!fs::is_regular_file(xess) || !fs::is_directory(amd)) {
        std::printf("vendor_file_test: no real vendor files in %s; their pins are not checked\n", vendor_files.string().c_str());
        return;
    }
    PosixFs pfs;
    const fs::path root = out / "vendor-file-real";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path kiln = root / "kiln";
    place(xess, kiln / "vendor" / "intel" / "3.0.2" / "libxess.dll");
    for (const Pin* p : component_pins("amd-fsr")) place(amd / p->name, kiln / "vendor" / "amd" / "2.4.0" / p->name);
    // receipts as the launcher writes them (schema/examples/vendor-receipt.json is intel-xess's)
    const fs::path example = fs::path(__FILE__).parent_path() / ".." / ".." / ".." / ".." / "schema" / "examples" / "vendor-receipt.json";
    if (fs::is_regular_file(example)) fs::copy_file(example, kiln / "vendor" / "intel" / "3.0.2" / "kiln-vendor.json");
    else receipt(kiln / "vendor" / "intel" / "3.0.2", "intel-xess");
    receipt(kiln / "vendor" / "amd" / "2.4.0", "amd-fsr");
    Verified x = verify_component(pfs, widen(kiln.string()), "intel-xess");
    CHECK(x.ok() && x.handles.size() == 1 && x.paths.size() == 1 && pfs.open == 1);
    CHECK(x.vendor == "intel" && x.version == "3.0.2" && PosixFs::p(x.paths[0]).ends_with("/kiln/vendor/intel/3.0.2/libxess.dll"));
    CHECK(PosixFs::p(x.dir).ends_with("/kiln/vendor/intel/3.0.2"));
    Verified a = verify_component(pfs, widen(kiln.string()), "amd-fsr");
    CHECK(a.ok() && a.handles.size() == 3 && pfs.open == 4);
    release(pfs, &x);
    release(pfs, &a);
    CHECK(pfs.open == 0 && x.handles.empty());
    fs::remove_all(root, ec);
    std::printf("vendor_file_test: the real libxess.dll and AMD's three DLLs match Kiln's pins\n");
}

int main(int argc, char** argv) {
    const fs::path out = argc > 1 ? argv[1] : fs::temp_directory_path();
    sha256_vectors();
    utf_round_trip();
    pins_are_sane();
    wrong_files(out);
    if (argc > 2) real_files(out, argv[2]);
    std::printf("vendor_file_test: %d checks passed\n", g_checks);
    return 0;
}
