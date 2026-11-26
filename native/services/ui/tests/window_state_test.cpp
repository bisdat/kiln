// SPDX-License-Identifier: MIT
// kiln.ui's window_state slot (draw_slots_window.inl, GitHub issue 149) on a headless Dear ImGui context under
// ASan/UBSan, and the ImGui behaviour Kiln.Ui's window docking (UiDock.cs) is built on, played with real windows and a
// real mouse: nothing outside a frame or for an unknown name (and no window made by asking); a window's place before its
// begin; a player's drag already applied before the dragged window's begin (MOVING), in every frame of the drag; a resize
// grip held (RESIZING); the current window from inside its begin and from a child region; a follower placed from its
// leader's rect of the same frame (SetNextWindowPos Always with a pivot) touching it in every frame of a drag of either one
// and of a resize of the shared edge; an auto-sized follower's first shown frame already beside its target; a dragged
// window kept inside the viewport by an Always position while the drag goes on; a window held by its resize grip and
// moved by an Always position grows by the move (why Kiln.Ui never moves a group's leader the player resizes); and the
// table's append-only layout.
#include "../draw_slots.h"
#include <cmath>
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

static_assert(offsetof(KilnUiApi, popups) == 1080 && offsetof(KilnUiApi, window_state) == 1088 && sizeof(KilnUiApi) == 1096);
static_assert(sizeof(KilnUiWindowState) == 24 && offsetof(KilnUiWindowState, flags) == 4 && offsetof(KilnUiWindowState, x) == 8 &&
              offsetof(KilnUiWindowState, y) == 12 && offsetof(KilnUiWindowState, width) == 16 && offsetof(KilnUiWindowState, height) == 20);
static_assert(KILN_UI_WINDOW_STATE_EXISTS == 1 && KILN_UI_WINDOW_STATE_MOVING == 2 && KILN_UI_WINDOW_STATE_RESIZING == 4);

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
static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float r() const { return x + w; }
    float b() const { return y + h; }
};
static Rect state_rect(const KilnUiWindowState& s) { return {s.x, s.y, s.width, s.height}; }
static KilnUiWindowState state(const char* name) {
    KilnUiWindowState s{};
    s.size = sizeof s;
    kui_draw::t_window_state(S(name), &s);
    return s;
}
static void move(float x, float y) { ImGui::GetIO().AddMousePosEvent(x, y); }
static void button(bool down) { ImGui::GetIO().AddMouseButtonEvent(0, down); }

// What one frame of the two windows saw.
struct Seen {
    KilnUiWindowState a_before{}, b_before{}, a_inside{}, child{};
    Rect a, b;
    bool b_led = false;
};

// One overlay frame the way Kiln.Ui docks B to the right of A: the leader (the window the player drags or resizes, else
// A) is begun first with no position of Kiln's; the other is placed from the leader's rect of this frame, B with its top
// left corner on A's top right corner, A with its top right corner on B's top left corner. clamp_x: A is kept at x >= it
// (the viewport's wall) with an Always position before its begin.
static Seen frame(bool first = false, float clamp_x = -1e9f) {
    Seen s;
    ImGui::GetIO().DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    kui::drawing = true;
    s.a_before = state("A###kiln.w1");
    s.b_before = state("B###kiln.w2");
    s.b_led = (s.b_before.flags & (KILN_UI_WINDOW_STATE_MOVING | KILN_UI_WINDOW_STATE_RESIZING)) != 0;
    auto begin_a = [&](bool placed, const Rect& from) {
        if (first) {
            ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(200, 150), ImGuiCond_Always);
        } else if (placed) {
            ImGui::SetNextWindowPos(ImVec2(from.x, from.y), ImGuiCond_Always, ImVec2(1, 0));
        } else if (s.a_before.x < clamp_x) {
            ImGui::SetNextWindowPos(ImVec2(clamp_x, s.a_before.y), ImGuiCond_Always);
        }
        ImGui::Begin("A###kiln.w1", nullptr, ImGuiWindowFlags_NoSavedSettings);
        s.a_inside = state("");
        ImGui::BeginChild("list", ImVec2(0, 60));
        s.child = state("");
        ImGui::EndChild();
        ImGui::Text("spells");
        const ImVec2 p = ImGui::GetWindowPos(), z = ImGui::GetWindowSize();
        s.a = {p.x, p.y, z.x, z.y};
        ImGui::End();
    };
    auto begin_b = [&](bool placed, const Rect& from) {
        if (placed) ImGui::SetNextWindowPos(ImVec2(from.r(), from.y), ImGuiCond_Always, ImVec2(0, 0));
        if (first) ImGui::SetNextWindowSize(ImVec2(180, 120), ImGuiCond_Always);
        ImGui::Begin("B###kiln.w2", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("grimoire");
        const ImVec2 p = ImGui::GetWindowPos(), z = ImGui::GetWindowSize();
        s.b = {p.x, p.y, z.x, z.y};
        ImGui::End();
    };
    if (s.b_led) {
        begin_b(false, {});
        begin_a(true, s.b);
    } else {
        begin_a(false, {});
        begin_b(true, s.a);
    }
    kui::drawing = false;
    ImGui::Render();
    return s;
}

static bool touching(const Seen& s) { return near(s.b.x, s.a.r()) && near(s.b.y, s.a.y); }

int main() {
    // ---- outside a frame and before any ImGui context: nothing, and never into ImGui ----
    KilnUiWindowState st{};
    st.size = sizeof st;
    CHECK(!kui_draw::t_window_state(S("A###kiln.w1"), &st));
    CHECK(st.flags == 0);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;  // ImGui resizes from edges only with a cursor-capable backend
    unsigned char* pixels = nullptr;
    int fw = 0, fh = 0;
    io.Fonts->AddFontDefault();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    move(10, 10);

    // ---- refused calls ----
    ImGui::NewFrame();
    kui::drawing = true;
    CHECK(!kui_draw::t_window_state(S("A###kiln.w1"), nullptr));
    KilnUiWindowState small{};
    small.size = sizeof small - 4;
    small.flags = 77;
    CHECK(!kui_draw::t_window_state(S("A###kiln.w1"), &small) && small.flags == 77);  // too small: nothing written
    const int windows = ImGui::GetCurrentContext()->Windows.Size;
    KilnUiWindowState none = state("Nobody###kiln.w9");
    CHECK(none.flags == 0 && none.x == 0 && none.width == 0);
    CHECK(ImGui::GetCurrentContext()->Windows.Size == windows);  // asking makes no window
    KilnUiWindowState outside = state("");
    CHECK(outside.flags == 0 || outside.flags == KILN_UI_WINDOW_STATE_EXISTS);  // the implicit "Debug##Default" window at most
    kui::drawing = false;
    ImGui::Render();

    // ---- the first frame places them; then each is known before its begin ----
    Seen s = frame(true);
    CHECK(near(s.a.x, 100) && near(s.a.y, 100) && near(s.a.w, 200) && near(s.a.h, 150));
    CHECK(touching(s));
    s = frame();
    CHECK(s.a_before.flags == KILN_UI_WINDOW_STATE_EXISTS);
    CHECK(near(s.a_before.x, 100) && near(s.a_before.y, 100) && near(s.a_before.width, 200) && near(s.a_before.height, 150));
    CHECK(s.b_before.flags == KILN_UI_WINDOW_STATE_EXISTS && near(s.b_before.x, 300) && near(s.b_before.width, 180));
    CHECK(s.a_inside.flags == KILN_UI_WINDOW_STATE_EXISTS && near(s.a_inside.x, 100) && near(s.a_inside.width, 200));
    CHECK(s.child.flags == KILN_UI_WINDOW_STATE_EXISTS && near(s.child.x, 100) && near(s.child.width, 200));  // the top-level window
    CHECK(touching(s));

    // ---- the player drags A by its title bar: moved before its begin, B beside it in the same frame ----
    move(150, 108);
    frame();
    button(true);
    frame();
    int moving = 0, together = 0, frames = 0;
    for (int i = 1; i <= 10; ++i) {
        move(150 + 12.0f * i, 108 + 5.0f * i);
        s = frame();
        ++frames;
        if (s.a_before.flags & KILN_UI_WINDOW_STATE_MOVING) ++moving;
        // the position read before A's begin is the one A is drawn at: this frame's
        if (near(s.a_before.x, s.a.x) && near(s.a_before.y, s.a.y) && touching(s)) ++together;
        CHECK(near(s.a_inside.x, s.a.x) && (s.a_inside.flags & KILN_UI_WINDOW_STATE_MOVING));
        CHECK(s.child.flags & KILN_UI_WINDOW_STATE_MOVING);  // from a child region: its window's state
        CHECK((s.b_before.flags & KILN_UI_WINDOW_STATE_MOVING) == 0);
    }
    CHECK(moving == frames);
    CHECK(together == frames);
    CHECK(near(s.a.x, 220) && near(s.a.y, 150));  // 100 + 120, 100 + 50
    button(false);
    s = frame();
    CHECK(s.a_before.flags == KILN_UI_WINDOW_STATE_EXISTS);  // released: no longer moving
    s = frame();
    CHECK((s.a_before.flags & KILN_UI_WINDOW_STATE_MOVING) == 0 && touching(s));

    // ---- holding still mid-drag is still a drag ----
    move(s.a.x + 50, s.a.y + 8);
    frame();
    button(true);
    frame();
    move(s.a.x + 60, s.a.y + 8);
    frame();
    s = frame();  // the pointer did not move this frame
    CHECK(s.a_before.flags & KILN_UI_WINDOW_STATE_MOVING);
    button(false);
    frame();
    s = frame();

    // ---- the player drags B, the docked one: B leads, A follows it in the same frame ----
    const float bx = s.b.x + 40, by = s.b.y + 8;
    move(bx, by);
    frame();
    button(true);
    frame();
    int led = 0;
    frames = together = 0;
    for (int i = 1; i <= 8; ++i) {
        move(bx - 10.0f * i, by + 20.0f * i);
        s = frame();
        ++frames;
        if (s.b_led && (s.b_before.flags & KILN_UI_WINDOW_STATE_MOVING)) ++led;
        if (touching(s)) ++together;
    }
    CHECK(led == frames && together == frames);
    button(false);
    frame();
    s = frame();
    CHECK(!s.b_led && touching(s));

    // ---- the player resizes A from its lower right grip: A's right edge moves, B moves with it in the same frame ----
    const float w0 = s.a.w;
    const float gx = s.a.r() - 3, gy = s.a.b() - 3;
    move(gx, gy);
    frame();
    button(true);
    frame();
    int resizing = 0;
    frames = together = 0;
    for (int i = 1; i <= 6; ++i) {
        move(gx + 15.0f * i, gy + 4.0f * i);
        s = frame();
        ++frames;
        if (s.a_before.flags & KILN_UI_WINDOW_STATE_RESIZING) ++resizing;
        CHECK((s.a_before.flags & KILN_UI_WINDOW_STATE_MOVING) == 0);
        if (touching(s)) ++together;
    }
    CHECK(resizing == frames);
    CHECK(together == frames);
    CHECK(near(s.a.w, w0 + 90));
    button(false);
    frame();
    s = frame();
    CHECK((s.a_before.flags & KILN_UI_WINDOW_STATE_RESIZING) == 0 && touching(s));

    // ---- the player resizes B from its lower left grip (the shared edge): B leads, A moves along with B's left edge ----
    // B to the front first (a click on its body): over the shared edge the front window's border has the pointer, and
    // A, resized last, is in front
    move(s.b.x + 90, s.b.y + 60);
    frame();
    button(true);
    frame();
    button(false);
    frame();
    s = frame();
    CHECK(touching(s));
    const float lx = s.b.x + 7, ly = s.b.b() - 7;
    move(lx, ly);
    frame();
    button(true);
    frame();
    frames = together = led = 0;
    const float bw0 = s.b.w, br0 = s.b.r();
    for (int i = 1; i <= 5; ++i) {
        move(lx - 10.0f * i, ly);
        s = frame();
        ++frames;
        if (s.b_led && (s.b_before.flags & KILN_UI_WINDOW_STATE_RESIZING)) ++led;
        if (touching(s)) ++together;
    }
    CHECK(led == frames && together == frames);
    CHECK(near(s.b.w, bw0 + 50) && near(s.b.r(), br0));  // B grew to the left, its right edge stayed
    button(false);
    frame();
    s = frame();
    CHECK(touching(s));

    // ---- a dragged window kept inside the viewport by an Always position: a wall, and the drag goes on ----
    const float a0 = s.a.x, ax = s.a.x + 60, ay = s.a.y + 8;
    move(ax, ay);
    frame(false, 0);
    button(true);
    frame(false, 0);
    for (int i = 1; i <= 10; ++i) {
        move(ax - 60.0f * i, ay);  // far past the left edge
        s = frame(false, 0);
        CHECK(s.a.x >= 0 && touching(s));
    }
    CHECK(near(s.a.x, 0) && (s.a_before.flags & KILN_UI_WINDOW_STATE_MOVING));
    move(ax + 40, ay);  // back to the right: the drag follows the pointer again at once
    s = frame(false, 0);
    CHECK(near(s.a.x, a0 + 40) && (s.a_before.flags & KILN_UI_WINDOW_STATE_MOVING) && touching(s));
    button(false);
    frame();
    s = frame();

    // ---- an auto-sized window placed beside A with a pivot: its first shown frame is already beside A ----
    const Rect a{600, 100, 0, 0};
    int shown = 0, beside = 0;
    for (int i = 0; i < 4; ++i) {
        ImGui::GetIO().DeltaTime = 1.0f / 60;
        ImGui::NewFrame();
        kui::drawing = true;
        ImGui::SetNextWindowPos(ImVec2(a.x, a.y), ImGuiCond_Always);
        ImGui::Begin("A###kiln.w1", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::End();
        const KilnUiWindowState c_before = state("C###kiln.w3");
        if (i == 0) CHECK(c_before.flags == 0);  // never begun: unknown
        ImGui::SetNextWindowPos(ImVec2(a.x, a.y), ImGuiCond_Always, ImVec2(1, 0));  // its right edge on A's left edge
        ImGui::Begin("C###kiln.w3", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("a window sized by its contents");
        ImGui::Button("and a button", ImVec2(140, 0));
        ImGuiWindow* c = ImGui::GetCurrentWindow();
        if (!c->Hidden) {
            ++shown;
            if (near(c->Pos.x + c->Size.x, a.x) && near(c->Pos.y, a.y) && c->Size.x > 100) ++beside;
        }
        ImGui::End();
        kui::drawing = false;
        ImGui::Render();
    }
    CHECK(shown >= 3 && beside == shown);

    // ---- a window held by its resize grip and moved by an Always position grows: ImGui keeps the grip at the pointer and
    // the top left corner where the position put it (UpdateWindowManualResize, absolute from the pointer), so Kiln.Ui never
    // moves a group's leader the player resizes to keep the group inside the viewport (UiDock.cs Clamp): it would grow by
    // that move every frame ----
    {
        auto frame_e = [](float set_x) {
            ImGui::GetIO().DeltaTime = 1.0f / 60;
            ImGui::NewFrame();
            kui::drawing = true;
            if (set_x > -1e8f) ImGui::SetNextWindowPos(ImVec2(set_x, 420), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(200, 150), ImGuiCond_FirstUseEver);
            ImGui::Begin("E###kiln.w5", nullptr, ImGuiWindowFlags_NoSavedSettings);
            ImGui::Text("resized");
            const ImVec2 p = ImGui::GetWindowPos(), z = ImGui::GetWindowSize();
            ImGui::End();
            kui::drawing = false;
            ImGui::Render();
            return Rect{p.x, p.y, z.x, z.y};
        };
        frame_e(880);
        Rect e = frame_e(-1e9f);
        CHECK(near(e.x, 880) && near(e.w, 200));
        const float ex = e.r() - 3, ey = e.b() - 3;
        move(ex, ey);
        frame_e(-1e9f);
        button(true);
        frame_e(-1e9f);
        move(ex + 30, ey);
        const Rect held = frame_e(-1e9f);
        CHECK(near(held.x, e.x) && near(held.w, e.w + 30));  // resized by the pointer
        const Rect moved = frame_e(held.x - 40);             // the pointer still, the window moved 40 to the left
        CHECK(near(moved.x, held.x - 40));
        CHECK(near(moved.r(), held.r()));   // the grip stayed at the pointer
        CHECK(near(moved.w, held.w + 40));  // so it grew by the move
        const Rect again = frame_e(moved.x - 40);
        CHECK(near(again.r(), held.r()) && near(again.w, held.w + 80));  // and again the next frame
        button(false);
        frame_e(-1e9f);
    }

    // ---- a window that does not move (NoMove) is never MOVING, even held ----
    for (int i = 0; i < 5; ++i) {
        if (i == 1) move(850, 408);
        if (i == 2) button(true);
        if (i == 3) move(900, 450);
        ImGui::NewFrame();
        kui::drawing = true;
        if (i >= 3) CHECK(state("Pinned###kiln.w4").flags == KILN_UI_WINDOW_STATE_EXISTS);
        ImGui::SetNextWindowPos(ImVec2(800, 400), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(200, 100), ImGuiCond_Always);
        ImGui::Begin("Pinned###kiln.w4", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::End();
        kui::drawing = false;
        ImGui::Render();
    }
    button(false);

    ImGui::DestroyContext();
    std::printf("window_state: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
