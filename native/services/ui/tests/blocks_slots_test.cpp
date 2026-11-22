// SPDX-License-Identifier: MIT
// kiln.ui.blocks v1's drawing slots (draw_slots_blocks.inl, 0.2.0-dev.31, dossier 09 §5.1) on a headless Dear ImGui
// context under ASan/UBSan: nothing outside a frame; begin_tab_item_ex's KUI_TAB_SELECT makes its tab the shown one (and
// the player's own pick stays once it is no longer passed); set_next_item_open opens and closes a tree node with
// KUI_COND_ALWAYS and ignores any other condition; item_state of a text field (active, edited, deactivated after an edit on
// Enter); input_text_ex's flags (ENTER_RETURNS true only on Enter, READ_ONLY never changes the text, MULTILINE takes Enter
// as a new line and stays active), a field cut to its capacity at a UTF-8 boundary, no buffer; the table's layout.
#include "../draw_slots.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace kui {
bool drawing = false;
bool in_frame() { return drawing; }
void style_push(uint32_t) {}
void style_pop() {}
bool style_push_font(uint32_t) { return false; }
void image_draw(KilnHandle, float, float) {}
bool image_button(const char*, KilnHandle, float, float) { return false; }
}  // namespace kui

static_assert(offsetof(KilnUiBlocksApi, begin_tab_item_ex) == 8 && offsetof(KilnUiBlocksApi, set_next_item_open) == 16 &&
              offsetof(KilnUiBlocksApi, item_state) == 24 && offsetof(KilnUiBlocksApi, input_text_ex) == 32 &&
              sizeof(KilnUiBlocksApi) == 40);
static_assert(KUI_TAB_SELECT == 1 && KUI_COND_ALWAYS == 0 && KUI_ITEM_ACTIVE == 1 && KUI_ITEM_DEACTIVATED == 2 &&
              KUI_ITEM_DEACTIVATED_AFTER_EDIT == 4 && KUI_ITEM_EDITED == 8 && KUI_INPUT_ENTER_RETURNS == 1 &&
              KUI_INPUT_READ_ONLY == 2 && KUI_INPUT_MULTILINE == 4);

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

static float g_dt = 1.0f / 60;  // one frame's time; a click after a long frame is never a double click
static void begin_frame() {
    ImGui::GetIO().DeltaTime = g_dt;
    g_dt = 1.0f / 60;
    ImGui::NewFrame();
    kui::drawing = true;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(500, 400));
    ImGui::Begin("blocks", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
}
static void end_frame() {
    ImGui::End();
    kui::drawing = false;
    ImGui::Render();
}
static void move(float x, float y) { ImGui::GetIO().AddMousePosEvent(x, y); }
static void button(bool down) { ImGui::GetIO().AddMouseButtonEvent(0, down); }
static void key(ImGuiKey k) {
    ImGui::GetIO().AddKeyEvent(k, true);
}
static void key_up(ImGuiKey k) { ImGui::GetIO().AddKeyEvent(k, false); }

// ---- tabs: which tab's contents were drawn this frame ----
static int tabs_frame(int select) {
    begin_frame();
    int shown = -1;
    if (ImGui::BeginTabBar("tabs")) {
        const char* names[] = {"Bag", "Quests", "Map"};
        for (int i = 0; i < 3; ++i) {
            if (kui_draw::t_begin_tab_item_ex(S(names[i]), i == select ? uint32_t{KUI_TAB_SELECT} : 0u)) {
                shown = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    end_frame();
    return shown;
}

// ---- trees: whether the node opened this frame; set < 0 calls no set_next_item_open ----
static bool tree_frame(int set, uint32_t condition = KUI_COND_ALWAYS) {
    begin_frame();
    if (set >= 0) kui_draw::t_set_next_item_open(set == 1, condition);
    const bool open = ImGui::TreeNode("Eastern hills");
    if (open) ImGui::TreePop();
    end_frame();
    return open;
}

// ---- a text field at 20..220 x 40..~60: its return and item_state ----
struct Field {
    bool changed = false;
    uint32_t state = 0;
};
static Field field_frame(char* buf, size_t cap, uint32_t flags, float h = 0) {
    begin_frame();
    ImGui::SetCursorScreenPos(ImVec2(20, 40));
    Field f;
    f.changed = kui_draw::t_input_text_ex(S("##note"), buf, cap, flags, 200, h);
    f.state = kui_draw::t_item_state();
    end_frame();
    return f;
}
static void click_field(char* buf, size_t cap, uint32_t flags, float h = 0) {
    move(60, 48);
    g_dt = 1.0f;  // a second since the last click: this one is a single click (a double click selects the word)
    field_frame(buf, cap, flags, h);
    button(true);
    field_frame(buf, cap, flags, h);
    button(false);
    field_frame(buf, cap, flags, h);
}
static void click_away(char* buf, size_t cap, uint32_t flags, float h = 0) {
    move(400, 350);
    g_dt = 1.0f;
    button(true);
    field_frame(buf, cap, flags, h);
    button(false);
    field_frame(buf, cap, flags, h);
}

int main() {
    // ---- outside a frame (and before any ImGui context): nothing, and never into ImGui ----
    CHECK(!kui_draw::t_begin_tab_item_ex(S("Bag"), KUI_TAB_SELECT));
    kui_draw::t_set_next_item_open(true, KUI_COND_ALWAYS);
    CHECK(kui_draw::t_item_state() == 0);
    char outside[8] = "keep";
    CHECK(!kui_draw::t_input_text_ex(S("f"), outside, sizeof outside, 0, 0, 0));
    CHECK(std::strcmp(outside, "keep") == 0);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    unsigned char* pixels = nullptr;
    int fw = 0, fh = 0;
    io.Fonts->AddFontDefault();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);

    // ---- tabs ----
    CHECK(tabs_frame(-1) == 0);  // the first tab by default
    int shown = tabs_frame(2);   // SELECT on Map: shown from this frame or the next
    if (shown != 2) shown = tabs_frame(2);
    CHECK(shown == 2);
    CHECK(tabs_frame(-1) == 2);  // the selection stays when SELECT is no longer passed
    CHECK(tabs_frame(-1) == 2);
    shown = tabs_frame(1);
    if (shown != 1) shown = tabs_frame(1);
    CHECK(shown == 1);
    CHECK(tabs_frame(1) == 1);  // passing it every frame keeps it

    // ---- trees ----
    CHECK(!tree_frame(-1));  // closed by default
    CHECK(tree_frame(1));    // opened by Kiln
    CHECK(tree_frame(-1));   // and stays open (ImGui's own state)
    CHECK(!tree_frame(0));   // closed by Kiln
    CHECK(!tree_frame(1, 1u));  // a condition other than Always: ignored (v1)
    CHECK(!tree_frame(-1));

    // ---- a text field: click, type, Enter ----
    char note[32] = "";
    Field f = field_frame(note, sizeof note, 0);
    CHECK(!f.changed && f.state == 0);
    click_field(note, sizeof note, 0);
    f = field_frame(note, sizeof note, 0);
    CHECK(f.state & KUI_ITEM_ACTIVE);
    io.AddInputCharactersUTF8("ab");
    f = field_frame(note, sizeof note, 0);
    CHECK(f.changed && (f.state & KUI_ITEM_EDITED) && (f.state & KUI_ITEM_ACTIVE));
    CHECK(std::strcmp(note, "ab") == 0);
    f = field_frame(note, sizeof note, 0);
    CHECK(!f.changed && !(f.state & KUI_ITEM_EDITED) && (f.state & KUI_ITEM_ACTIVE));  // edited is only that frame
    key(ImGuiKey_Enter);
    uint32_t seen = field_frame(note, sizeof note, 0).state;
    key_up(ImGuiKey_Enter);
    const uint32_t after = field_frame(note, sizeof note, 0).state;
    // Enter ends the edit: deactivated after an edit, once (in the Enter frame or the one after), then idle
    CHECK(((seen | after) & KUI_ITEM_DEACTIVATED_AFTER_EDIT) != 0);
    CHECK(!((seen & after) & KUI_ITEM_DEACTIVATED_AFTER_EDIT));
    f = field_frame(note, sizeof note, 0);
    CHECK(f.state == 0);
    CHECK(std::strcmp(note, "ab") == 0);

    // ---- ENTER_RETURNS: typing returns false, Enter returns true ----
    click_field(note, sizeof note, KUI_INPUT_ENTER_RETURNS);
    io.AddInputCharactersUTF8("c");
    f = field_frame(note, sizeof note, KUI_INPUT_ENTER_RETURNS);
    CHECK(!f.changed && (f.state & KUI_ITEM_EDITED));
    CHECK(std::strcmp(note, "abc") == 0 || !std::printf("  note is \"%s\"\n", note));
    key(ImGuiKey_Enter);
    f = field_frame(note, sizeof note, KUI_INPUT_ENTER_RETURNS);
    key_up(ImGuiKey_Enter);
    CHECK(f.changed);
    field_frame(note, sizeof note, KUI_INPUT_ENTER_RETURNS);

    // ---- blur also ends an edit (the field loses the keyboard when the player clicks elsewhere) ----
    click_field(note, sizeof note, 0);
    io.AddInputCharactersUTF8("d");
    field_frame(note, sizeof note, 0);
    move(400, 350);
    g_dt = 1.0f;
    button(true);
    seen = field_frame(note, sizeof note, 0).state;
    button(false);
    seen |= field_frame(note, sizeof note, 0).state;
    CHECK(seen & KUI_ITEM_DEACTIVATED_AFTER_EDIT);
    CHECK(std::strcmp(note, "abcd") == 0 || !std::printf("  note is \"%s\"\n", note));

    // ---- READ_ONLY: active, but typing changes nothing ----
    char shown_only[16] = "fixed";
    click_field(shown_only, sizeof shown_only, KUI_INPUT_READ_ONLY);
    io.AddInputCharactersUTF8("zz");
    f = field_frame(shown_only, sizeof shown_only, KUI_INPUT_READ_ONLY);
    CHECK(!f.changed && !(f.state & KUI_ITEM_EDITED));
    CHECK(std::strcmp(shown_only, "fixed") == 0);
    click_away(shown_only, sizeof shown_only, KUI_INPUT_READ_ONLY);

    // ---- MULTILINE: Enter is a new line and the field stays active; ENTER_RETURNS is ignored there ----
    char lines[64] = "one";
    const uint32_t ml = KUI_INPUT_MULTILINE | KUI_INPUT_ENTER_RETURNS;
    click_field(lines, sizeof lines, ml, 60);
    key(ImGuiKey_End);
    field_frame(lines, sizeof lines, ml, 60);
    key_up(ImGuiKey_End);
    key(ImGuiKey_Enter);
    f = field_frame(lines, sizeof lines, ml, 60);
    key_up(ImGuiKey_Enter);
    CHECK(f.changed && (f.state & KUI_ITEM_ACTIVE));
    CHECK(std::strchr(lines, '\n') != nullptr);
    f = field_frame(lines, sizeof lines, ml, 60);
    CHECK(f.state & KUI_ITEM_ACTIVE);
    click_away(lines, sizeof lines, ml, 60);

    // ---- capacity: the text is cut to capacity - 1 bytes, never inside a character ----
    char small[6] = "";
    click_field(small, sizeof small, 0);
    io.AddInputCharactersUTF8("ab\xc3\xa9\xc3\xa9");  // "abéé": 6 bytes, room for 5
    field_frame(small, sizeof small, 0);
    CHECK(std::strcmp(small, "ab\xc3\xa9") == 0);
    click_away(small, sizeof small, 0);

    // ---- a capacity that grows while the field is active (review S4, the reviewer's repro): ImGui sized its own copy of
    // the text when the field was activated, so typing past that size would write past its buffer. The slot keeps the
    // activation's capacity until the field lets go of the keyboard; the next activation takes the new one. ----
    std::vector<char> big(4097, 0);
    click_field(big.data(), 8, 0);
    CHECK(ImGui::GetActiveID() != 0);
    const int activated = GImGui->InputTextState.TextA.Size;
    for (int i = 0; i < 64; ++i) {
        io.AddInputCharacter('a' + (i % 26));
        field_frame(big.data(), big.size(), 0);
    }
    CHECK(GImGui->InputTextState.TextA.Size == activated);
    CHECK(GImGui->InputTextState.TextLen <= activated - 1);
    CHECK(std::strlen(big.data()) == 7);  // the capacity at activation, 8 bytes with the NUL
    click_away(big.data(), big.size(), 0);
    click_field(big.data(), big.size(), 0);  // activated again: the whole new capacity
    io.AddInputCharactersUTF8("xyz");
    field_frame(big.data(), big.size(), 0);
    CHECK(std::strlen(big.data()) == 10);
    click_away(big.data(), big.size(), 0);
    // a capacity that shrinks while active is ImGui's own bound: never more than it is given
    std::memset(big.data(), 0, big.size());
    click_field(big.data(), 64, 0);
    for (int i = 0; i < 20; ++i) {
        io.AddInputCharacter('k');
        field_frame(big.data(), 6, 0);
    }
    CHECK(std::strlen(big.data()) <= 5);
    click_away(big.data(), 6, 0);

    // ---- no buffer: false, nothing drawn into ----
    begin_frame();
    CHECK(!kui_draw::t_input_text_ex(S("##none"), nullptr, 16, 0, 0, 0));
    char one[1] = {'x'};
    CHECK(!kui_draw::t_input_text_ex(S("##zero"), one, 0, 0, 0, 0));
    CHECK(one[0] == 'x');
    end_frame();

    ImGui::DestroyContext();
    std::printf("blocks_slots: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
