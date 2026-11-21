// SPDX-License-Identifier: MIT
// kiln.ui's pointer, drag_source and drop_target slots (draw_slots_pointer.inl, 0.2.0-dev.30.3, GitHub issue 113) on a
// headless Dear ImGui context under ASan/UBSan: nothing outside a frame, the pointer and held buttons as ImGui's io has
// them, a drag from one button dropped on another (delivered once, on release, with the payload's bytes), a kind that
// does not match, a payload cut to the caller's buffer or no buffer at all, a source without an id, a drag whose source
// is no longer drawn (it goes on with its last payload until the release), refused kinds and payloads, and the table's
// append-only layout.
#include "../draw_slots.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace kui {
bool drawing = false;
bool in_frame() { return drawing; }
void style_push(uint32_t) {}
void style_pop() {}
bool style_push_font(uint32_t) { return false; }
void image_draw(KilnHandle, float, float) {}
bool image_button(const char*, KilnHandle, float, float) { return false; }
}  // namespace kui

static_assert(offsetof(KilnUiApi, pointer) == offsetof(KilnUiApi, chat_search) + sizeof(void*));
static_assert(offsetof(KilnUiApi, pointer) == 1056 && offsetof(KilnUiApi, drag_source) == 1064 &&
              offsetof(KilnUiApi, drop_target) == 1072 && sizeof(KilnUiApi) == 1080);
static_assert(sizeof(KilnUiPointer) == 24 && offsetof(KilnUiPointer, flags) == 4 && offsetof(KilnUiPointer, x) == 8 &&
              offsetof(KilnUiPointer, y) == 12 && offsetof(KilnUiPointer, held) == 16);
static_assert(KILN_UI_DRAG_KIND_MAX == 32 && KILN_UI_DRAG_PAYLOAD_MAX == 4096);

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

static KilnStr S(const char* s) { return KilnStr{s, std::strlen(s)}; }
static KilnStr S(const std::string& s) { return KilnStr{s.data(), s.size()}; }

// What one frame saw: the source's and the target's answers, and the pointer.
struct Seen {
    bool dragging = false, dropped = false, other = false, pointer = false;
    KilnUiPointer p{};
    std::string payload;
    size_t len = 0;
};
static std::string g_payload = "item:42";
static size_t g_capacity = 64;
static bool g_id_less = false;    // the source is a widget without an id (ImGui::Dummy, as text or an image): SourceAllowNullID
static bool g_source_on = true;   // the source widget is drawn this frame (a mod that stops drawing it cancels the drag)
static bool g_no_buffer = false;  // the target passes no buffer (NULL, capacity 0): only the length

// One overlay frame: a window with a source button and a target button (and the same target for another kind).
static Seen frame() {
    Seen s;
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    kui::drawing = true;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(400, 300));
    ImGui::Begin("panes", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetCursorScreenPos(ImVec2(20, 40));
    if (g_source_on) {
        if (g_id_less) ImGui::Dummy(ImVec2(100, 40));  // 20..120 x 40..80, no id
        else ImGui::Button("source", ImVec2(100, 40));  // 20..120 x 40..80
        s.dragging = kui_draw::t_drag_source(S("kiln.test/item"), g_payload.data(), g_payload.size(), S("Iron bar"));
    }
    ImGui::SetCursorScreenPos(ImVec2(220, 40));
    ImGui::Button("target", ImVec2(100, 40));  // 220..320 x 40..80
    char buf[128] = {};
    s.dropped = g_no_buffer ? kui_draw::t_drop_target(S("kiln.test/item"), nullptr, 0, &s.len)
                            : kui_draw::t_drop_target(S("kiln.test/item"), buf, g_capacity, &s.len);
    if (s.dropped) s.payload.assign(buf, std::min(s.len, g_capacity));
    ImGui::SetCursorScreenPos(ImVec2(220, 140));
    ImGui::Button("other", ImVec2(100, 40));  // 220..320 x 140..180: another kind
    s.other = kui_draw::t_drop_target(S("kiln.test/other"), buf, sizeof buf, nullptr);
    s.p.size = sizeof s.p;
    s.pointer = kui_draw::t_pointer(&s.p);
    ImGui::End();
    kui::drawing = false;
    ImGui::Render();
    return s;
}
static void move(float x, float y) { ImGui::GetIO().AddMousePosEvent(x, y); }
static void button(int b, bool down) { ImGui::GetIO().AddMouseButtonEvent(b, down); }

// Drags from the source to (x, y) and releases there; how many frames reported a drop on each target.
static void drag_to(float x, float y, int* drops, int* others, Seen* last) {
    *drops = *others = 0;
    move(70, 60);
    frame();
    button(0, true);
    frame();
    for (int i = 1; i <= 6; ++i) {  // move in steps (past ImGui's drag threshold)
        move(70 + (x - 70) * i / 6, 60 + (y - 60) * i / 6);
        Seen s = frame();
        *drops += s.dropped;
        *others += s.other;
    }
    button(0, false);
    *last = frame();
    *drops += last->dropped;
    *others += last->other;
    for (int i = 0; i < 2; ++i) {
        Seen s = frame();
        *drops += s.dropped;
        *others += s.other;
    }
}

int main() {
    // ---- outside a frame (and before any ImGui context): nothing, and never into ImGui ----
    KilnUiPointer p{};
    p.size = sizeof p;
    CHECK(!kui_draw::t_pointer(&p));
    CHECK(p.flags == 0 && p.held == 0);
    CHECK(!kui_draw::t_drag_source(S("k"), "x", 1, S("")));
    char out[8];
    size_t n = 99;
    CHECK(!kui_draw::t_drop_target(S("k"), out, sizeof out, &n));
    CHECK(n == 99);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    unsigned char* pixels = nullptr;
    int fw = 0, fh = 0;
    io.Fonts->AddFontDefault();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);

    // ---- the pointer: unknown before the first mouse move, then ImGui's position and held buttons ----
    Seen s = frame();
    CHECK(s.pointer);
    CHECK((s.p.flags & KILN_UI_POINTER_VALID) == 0 && s.p.x == 0 && s.p.y == 0 && s.p.held == 0);
    move(123.6f, 456.25f);  // ImGui keeps whole pixels (the window's messages carry whole pixels anyway)
    s = frame();
    CHECK(s.pointer && (s.p.flags & KILN_UI_POINTER_VALID) && s.p.x == 123.0f && s.p.y == 456.0f);
    CHECK(s.p.held == 0 && (s.p.flags & KILN_UI_POINTER_DRAGGING) == 0);
    button(1, true);
    s = frame();
    CHECK(s.p.held == 2u);
    button(2, true);
    button(0, true);
    s = frame();
    CHECK(s.p.held == 7u);
    button(0, false);
    button(1, false);
    button(2, false);
    s = frame();
    CHECK(s.p.held == 0);
    // a too small record: refused, untouched
    KilnUiPointer small{};
    small.size = 8;
    small.flags = 77;
    kui::drawing = true;
    CHECK(!kui_draw::t_pointer(&small));
    CHECK(!kui_draw::t_pointer(nullptr));
    kui::drawing = false;
    CHECK(small.flags == 77);

    // ---- a drag from source to target: dragging while held, delivered once on release with the bytes ----
    int drops = 0, others = 0;
    Seen last;
    move(70, 60);
    frame();
    button(0, true);
    frame();
    move(150, 60);
    s = frame();
    move(270, 60);
    s = frame();
    CHECK(s.dragging);
    CHECK(s.p.flags & KILN_UI_POINTER_DRAGGING);
    CHECK(s.p.held == 1u);
    CHECK(!s.dropped);  // hovering is not a drop
    button(0, false);
    s = frame();
    CHECK(s.dropped && s.payload == "item:42" && s.len == 7);
    s = frame();
    CHECK(!s.dropped && !s.dragging && (s.p.flags & KILN_UI_POINTER_DRAGGING) == 0);

    drag_to(270, 60, &drops, &others, &last);
    CHECK(drops == 1 && others == 0);
    // released over the target of another kind: no drop anywhere
    drag_to(270, 160, &drops, &others, &last);
    CHECK(drops == 0 && others == 0);
    // released over nothing
    drag_to(600, 500, &drops, &others, &last);
    CHECK(drops == 0 && others == 0);
    // a payload longer than the buffer: cut, the whole length reported
    g_capacity = 4;
    drag_to(270, 60, &drops, &others, &last);
    CHECK(drops == 1 && last.dropped && last.payload == "item" && last.len == 7);
    g_capacity = 64;
    // an empty payload is a payload
    g_payload.clear();
    drag_to(270, 60, &drops, &others, &last);
    CHECK(drops == 1 && last.dropped && last.len == 0 && last.payload.empty());
    g_payload = "item:42";
    // no buffer at all: still delivered once, with the whole length (a caller that only needs to know)
    g_no_buffer = true;
    drag_to(270, 60, &drops, &others, &last);
    CHECK(drops == 1 && last.dropped && last.len == 7);
    g_no_buffer = false;
    // a source without an id (text, an image): SourceAllowNullID makes one from its rectangle
    g_id_less = true;
    drag_to(270, 60, &drops, &others, &last);
    CHECK(drops == 1 && last.dropped && last.payload == "item:42");
    g_id_less = false;

    // ---- the source stops being drawn while the button is held (Dear ImGui 1.93 without PayloadAutoExpire): the drag
    // goes on with its last payload until the release; released over the target it is delivered once, over nothing it ends
    for (const bool over_target : {true, false}) {
        move(70, 60);
        frame();
        button(0, true);
        frame();
        move(150, 60);
        frame();
        move(270, 60);
        s = frame();
        CHECK(s.dragging && (s.p.flags & KILN_UI_POINTER_DRAGGING));
        g_source_on = false;
        g_payload = "changed";  // never set again: the drag keeps the last payload set
        drops = 0;
        for (int i = 0; i < 3; ++i) {
            s = frame();
            drops += s.dropped;
            CHECK(!s.dragging && (s.p.flags & KILN_UI_POINTER_DRAGGING));
        }
        if (!over_target) move(600, 500);
        frame();
        button(0, false);
        s = frame();
        drops += s.dropped;
        CHECK(over_target ? (s.dropped && s.payload == "item:42") : !s.dropped);
        s = frame();
        drops += s.dropped;
        CHECK((s.p.flags & KILN_UI_POINTER_DRAGGING) == 0);
        CHECK(drops == (over_target ? 1 : 0));
        g_source_on = true;
        g_payload = "item:42";
    }
    // and the next drag works as before
    drag_to(270, 60, &drops, &others, &last);
    CHECK(drops == 1 && last.payload == "item:42");

    // ---- refusals (inside a frame) ----
    ImGui::NewFrame();
    kui::drawing = true;
    ImGui::Begin("r");
    ImGui::Button("b");
    const std::string big(KILN_UI_DRAG_PAYLOAD_MAX + 1, 'x');
    CHECK(!kui_draw::t_drag_source(S(""), "x", 1, S("")));                       // no kind
    CHECK(!kui_draw::t_drag_source(S("_IMGUI"), "x", 1, S("")));                 // ImGui's own
    CHECK(!kui_draw::t_drag_source(S(std::string(33, 'k')), "x", 1, S("")));   // 33 bytes
    CHECK(!kui_draw::t_drag_source(KilnStr{"a\0b", 3}, "x", 1, S("")));        // a NUL inside
    CHECK(!kui_draw::t_drag_source(S("k"), big.data(), big.size(), S("")));     // too long
    CHECK(!kui_draw::t_drag_source(S("k"), nullptr, 3, S("")));                 // no bytes
    CHECK(!kui_draw::t_drop_target(S("_x"), out, sizeof out, &n));
    CHECK(!kui_draw::t_drop_target(S("k"), nullptr, 4, &n));
    CHECK(!kui_draw::t_drag_source(S(std::string(32, 'k')), "x", 1, S("")));   // 32 bytes is fine (not dragged: false)
    ImGui::End();
    kui::drawing = false;
    ImGui::Render();

    ImGui::DestroyContext();
    std::printf("pointer_drag: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
