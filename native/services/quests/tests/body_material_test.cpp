// SPDX-License-Identifier: MIT
#include "bodypack.h"
#include "nlohmann/json.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>
using namespace kiln::qb;
using Json = nlohmann::ordered_json;
static int checks = 0, failed = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
namespace {
constexpr const char* kGuid = "9e932f9d-194f-511f-bf38-c9b16493e675";
constexpr const char* kOther = "12345678-1234-5678-9234-123456789abc";
constexpr const char* kSha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
Json materials() { return Json::array({{{"mod", "body-mod"}, {"name", "skin"}, {"guid", kGuid}, {"file", "blocks/skin.json"}, {"sha256", kSha}}}); }
struct Fixture {
    Json rows = materials();
    Json report = {{"ok", false}, {"errors", {"an unrelated VFX failed"}},
        {"materials", Json::array({{{"mod", "body-mod"}, {"name", "skin"}, {"guid", kGuid}}})}};
    std::string source = "abc";
    bool readable = true, resident = true;
    int reads = 0, statuses = 0, served = 0;
    BodyMaterialContext context() {
        BodyMaterialContext c;
        c.read_source = [&](const std::string& file, Bytes* bytes, std::string* why) {
            ++reads; CHECK(file == "blocks/skin.json");
            if (!readable) { *why = "source missing"; return false; }
            bytes->assign(source.begin(), source.end()); return true;
        };
        c.build_status = [&](Json* out, std::string*) { ++statuses; *out = report; return true; };
        c.served = [&](const Guid& guid, uint32_t type) {
            ++served; CHECK(guid_text(guid) == kGuid || guid_text(guid) == kOther);
            CHECK(type == fnv1a("keen::RenderMaterialResource", std::strlen("keen::RenderMaterialResource")));
            return resident;
        };
        return c;
    }
    bool check(std::string* why) { return check_body_materials("body-mod", rows, context(), why); }
};
void refusals() {
    for (const char* change : {"cache-mod", "cache-name", "cache-guid", "file", "traversal", "backslash", "hash", "changed-file", "missing-file", "status-root", "status-array", "status-empty", "status-mod", "status-name", "status-guid", "replacement", "not-resident", "duplicate-cache", "duplicate-status"}) {
        Fixture f;
        const std::string what = change;
        if (what == "cache-mod") f.rows[0]["mod"] = "foreign";
        if (what == "cache-name") f.rows[0]["name"] = "";
        if (what == "cache-guid") f.rows[0]["guid"] = "00000000-0000-0000-0000-000000000000";
        if (what == "file") f.rows[0]["file"] = "";
        if (what == "traversal") f.rows[0]["file"] = "blocks/../skin.json";
        if (what == "backslash") f.rows[0]["file"] = "blocks\\skin.json";
        if (what == "hash") f.rows[0]["sha256"] = "not-sha256";
        if (what == "changed-file") f.source = "changed";
        if (what == "missing-file") f.readable = false;
        if (what == "status-mod") f.report["materials"][0]["mod"] = "foreign";
        if (what == "status-name") f.report["materials"][0]["name"] = "another";
        if (what == "status-guid") f.report["materials"][0]["guid"] = "12345678-1234-5678-9234-123456789abc";
        if (what == "replacement") f.report["materials"][0]["replaces"] = true;
        if (what == "not-resident") f.resident = false;
        if (what == "duplicate-cache") f.rows.push_back(f.rows[0]);
        if (what == "duplicate-status") f.report["materials"].push_back(f.report["materials"][0]);
        if (what == "status-root") f.report = Json::array();
        if (what == "status-array") f.report["materials"] = "invalid";
        if (what == "status-empty") { f.report["ok"] = true; f.report["materials"] = Json::array(); }
        std::string why;
        const bool ok = f.check(&why);
        if (ok) std::printf("unexpected admission: %s\n", change);
        CHECK(!ok); CHECK(why.find("body.materials") != std::string::npos);
    }
}
}
int main() {
    Fixture f; std::string why;
    CHECK(f.check(&why));  // build_status.ok is not the evidence: unrelated failures do not invalidate this row
    CHECK(f.reads == 1 && f.statuses == 1 && f.served == 1);
    Fixture shared;
    Json second = shared.rows[0]; second["name"] = "tint"; second["guid"] = kOther;
    shared.rows.push_back(second);
    shared.report["materials"].push_back({{"mod", "body-mod"}, {"name", "tint"}, {"guid", kOther}});
    CHECK(shared.check(&why));
    CHECK(shared.reads == 1 && shared.statuses == 1 && shared.served == 2);
    Fixture no_renderer;
    auto context = no_renderer.context(); context.server = true;
    CHECK(check_body_materials("body-mod", no_renderer.rows, context, &why));
    CHECK(no_renderer.reads == 0 && no_renderer.statuses == 0 && no_renderer.served == 0);
    CHECK(check_body_materials("body-mod", Json::array(), BodyMaterialContext{}, &why));
    CHECK(!check_body_materials("body-mod", materials(), BodyMaterialContext{}, &why));
    refusals();
    std::printf("body materials: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
