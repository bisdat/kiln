// SPDX-License-Identifier: MIT
// Calls only the pure item-record codec in mapped read-only executable copies. No game process or DLL loader.
#include "../storage_engine.cpp"
#include "nlohmann/json.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>
namespace kitems {
Engine E;
}
using namespace kitems::storage;
using nlohmann::json;
namespace {
unsigned checks = 0, failed = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        ++failed;
        std::printf("FAIL %s\n", message);
    }
}
struct Image {
    uint8_t* base = nullptr;
    PRUNTIME_FUNCTION functions = nullptr;
    ~Image() {
        if (functions)
            RtlDeleteFunctionTable(functions);
        if (base)
            VirtualFree(base, 0, MEM_RELEASE);
    }
    bool open(const char* path) {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {});
        if (file.size() < sizeof(IMAGE_DOS_HEADER))
            return false;
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
            size_t(dos->e_lfanew) > file.size() - sizeof(IMAGE_NT_HEADERS64))
            return false;
        auto pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
        if (pe->Signature != IMAGE_NT_SIGNATURE || pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            pe->FileHeader.NumberOfSections > 64 || pe->OptionalHeader.SizeOfImage > 256u * 1024u * 1024u)
            return false;
        auto sections = IMAGE_FIRST_SECTION(pe);
        if (reinterpret_cast<const uint8_t*>(sections + pe->FileHeader.NumberOfSections) > file.data() + file.size())
            return false;
        base = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, pe->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!base)
            return false;
        for (unsigned i = 0; i < pe->FileHeader.NumberOfSections; ++i) {
            auto& s = sections[i];
            if (s.PointerToRawData > file.size() || s.SizeOfRawData > file.size() - s.PointerToRawData ||
                s.VirtualAddress > pe->OptionalHeader.SizeOfImage ||
                s.SizeOfRawData > pe->OptionalHeader.SizeOfImage - s.VirtualAddress)
                return false;
            std::memcpy(base + s.VirtualAddress, file.data() + s.PointerToRawData, s.SizeOfRawData);
        }
        DWORD previous = 0;
        if (!VirtualProtect(base, pe->OptionalHeader.SizeOfImage, PAGE_EXECUTE_READ, &previous))
            return false;
        const auto& unwind = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!unwind.Size || unwind.VirtualAddress > pe->OptionalHeader.SizeOfImage ||
            unwind.Size > pe->OptionalHeader.SizeOfImage - unwind.VirtualAddress)
            return false;
        functions = reinterpret_cast<PRUNTIME_FUNCTION>(base + unwind.VirtualAddress);
        return RtlAddFunctionTable(functions, unwind.Size / sizeof(RUNTIME_FUNCTION),
                                   reinterpret_cast<DWORD64>(base)) != 0;
    }
};
Record from_fields(const json& e) {
    Record r;
    r.slot = e.at("slot");
    r.item = e.at("itemId");
    r.count = e.at("count");
    const char* names[][2] = {{"level", nullptr},      {"durability", "maxDurability"}, {"liquid", "maxLiquid"},
                              {"appearance", nullptr}, {"maxUpgrades", "upgrades"},     {"socket", nullptr}};
    const unsigned widths[][2] = {{1, 0}, {4, 4}, {4, 4}, {4, 0}, {1, 1}, {5, 0}};
    for (unsigned i = 0; i < 6; ++i)
        if (e.contains(names[i][0])) {
            r.present[i] = true;
            for (unsigned j = 0; j < 2 && names[i][j]; ++j) {
                auto& v = e.at(names[i][j]);
                if (v.is_string()) {
                    auto s = v.get<std::string>();
                    for (size_t n = 0; n < s.size(); n += 2)
                        r.options[i].push_back(static_cast<uint8_t>(std::stoul(s.substr(n, 2), nullptr, 16)));
                } else
                    append_le(r.options[i], v.get<uint64_t>(), widths[i][j]);
            }
        }
    return r;
}
void run(const char* path, uint32_t read_rva, uint32_t write_rva, const json& vectors) {
    Image image;
    if (!image.open(path)) {
        check(false, "map executable codec fixture");
        return;
    }
    S.read = reinterpret_cast<ReadFn>(image.base + read_rva);
    S.write = reinterpret_cast<WriteFn>(image.base + write_rva);
    check(true, "map executable codec fixture");
    for (const auto& v : vectors) {
        std::vector<Record> records;
        const auto& f = v.at("fields");
        for (const auto& e : f.at("equipment"))
            if (!e.is_null())
                records.push_back(from_fields(e));
        for (const char* list : {"main", "extra"})
            for (const auto& e : f.at(list))
                records.push_back(from_fields(e));
        for (const auto& r : records) {
            uint8_t memory[56]{}, decoded[56]{}, bytes[64]{};
            uint8_t length = 0;
            auto expected = wire_record(r);
            check(memory_from_record(r, memory), "construct Record56");
            bool written = write_record(memory, bytes, &length);
            check(written, "game record writer returns without fault");
            if (!written)
                continue;
            check(length == expected.size() && !std::memcmp(bytes, expected.data(), length),
                  "game writer matches exact record bytes");
            bool read = read_record(expected.data(), expected.size(), decoded);
            check(read, "game record reader consumes exact validated bytes");
            if (!read)
                continue;
            Record actual;
            check(record_from_memory(decoded, &actual) && wire_record(actual) == expected,
                  "game reader preserves every option and zero presence");
        }
    }
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: storage-codec server-copy client-copy witness-vectors\n");
        return 2;
    }
    json fixture;
    std::ifstream(argv[3]) >> fixture;
    run(argv[1], 0x71bd50, 0x741dc0, fixture.at("vectors"));
    run(argv[2], 0xe071b0, 0xe353c0, fixture.at("vectors"));
    std::printf("storage codec: %u checks, %u failed\n", checks, failed);
    return failed ? 1 : 0;
}
